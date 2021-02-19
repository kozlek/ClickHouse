#include "CacheDictionary.h"

#include <memory>

#include <ext/range.h>
#include <ext/size.h>
#include <ext/map.h>
#include <ext/chrono_io.h>

#include <Core/Defines.h>
#include <Common/BitHelpers.h>
#include <Common/CurrentMetrics.h>
#include <Common/HashTable/Hash.h>
#include <Common/HashTable/HashSet.h>
#include <Common/ProfileEvents.h>
#include <Common/ProfilingScopedRWLock.h>
#include <Common/typeid_cast.h>
#include <Common/setThreadName.h>
#include <IO/WriteBufferFromOStream.h>
#include <Dictionaries/DictionaryBlockInputStream.h>
#include <Dictionaries/CacheDictionaryStorage.h>
#include <Dictionaries/SSDCacheDictionaryStorage.h>
#include <Dictionaries/DictionaryFactory.h>

namespace ProfileEvents
{
extern const Event DictCacheKeysRequested;
extern const Event DictCacheKeysRequestedMiss;
extern const Event DictCacheKeysRequestedFound;
extern const Event DictCacheKeysExpired;
extern const Event DictCacheKeysNotFound;
extern const Event DictCacheKeysHit;
extern const Event DictCacheRequestTimeNs;
extern const Event DictCacheRequests;
extern const Event DictCacheLockWriteNs;
extern const Event DictCacheLockReadNs;
}

namespace CurrentMetrics
{
extern const Metric DictCacheRequests;
}

namespace DB
{
namespace ErrorCodes
{
    extern const int CACHE_DICTIONARY_UPDATE_FAIL;
    extern const int TYPE_MISMATCH;
    extern const int BAD_ARGUMENTS;
    extern const int UNSUPPORTED_METHOD;
    extern const int TOO_SMALL_BUFFER_SIZE;
}

template <DictionaryKeyType dictionary_key_type>
CacheDictionary<dictionary_key_type>::CacheDictionary(
    const StorageID & dict_id_,
    const DictionaryStructure & dict_struct_,
    DictionarySourcePtr source_ptr_,
    CacheDictionaryStoragePtr cache_storage_ptr_,
    CacheDictionaryUpdateQueueConfiguration update_queue_configuration_,
    DictionaryLifetime dict_lifetime_,
    bool allow_read_expired_keys_)
    : IDictionary(dict_id_)
    , dict_struct(dict_struct_)
    , source_ptr{std::move(source_ptr_)}
    , cache_storage_ptr(cache_storage_ptr_)
    , update_queue(
        dict_id_.getNameForLogs(),
        update_queue_configuration_,
        [this](CacheDictionaryUpdateUnitPtr<dictionary_key_type> & unit_to_update)
        {
            update(unit_to_update);
        })
    , dict_lifetime(dict_lifetime_)
    , log(&Poco::Logger::get("ExternalDictionaries"))
    , allow_read_expired_keys(allow_read_expired_keys_)
    , rnd_engine(randomSeed())
{
    if (!source_ptr->supportsSelectiveLoad())
        throw Exception{full_name + ": source cannot be used with CacheDictionary", ErrorCodes::UNSUPPORTED_METHOD};

    setupHierarchicalAttribute();
}

template <DictionaryKeyType dictionary_key_type>
CacheDictionary<dictionary_key_type>::~CacheDictionary()
{
    update_queue.stopAndWait();
}

template <DictionaryKeyType dictionary_key_type>
size_t CacheDictionary<dictionary_key_type>::getElementCount() const
{
    const ProfilingScopedReadRWLock read_lock{rw_lock, ProfileEvents::DictCacheLockReadNs};
    return cache_storage_ptr->getSize();
}

template <DictionaryKeyType dictionary_key_type>
size_t CacheDictionary<dictionary_key_type>::getBytesAllocated() const
{
    /// In case of existing string arena we check the size of it.
    /// But the same appears in setAttributeValue() function, which is called from update() function
    /// which in turn is called from another thread.
    const ProfilingScopedReadRWLock read_lock{rw_lock, ProfileEvents::DictCacheLockReadNs};
    return cache_storage_ptr->getBytesAllocated();
}

template <DictionaryKeyType dictionary_key_type>
double CacheDictionary<dictionary_key_type>::getLoadFactor() const
{
    const ProfilingScopedReadRWLock read_lock{rw_lock, ProfileEvents::DictCacheLockReadNs};
    return static_cast<double>(cache_storage_ptr->getSize()) / cache_storage_ptr->getMaxSize();
}

template <DictionaryKeyType dictionary_key_type>
std::exception_ptr CacheDictionary<dictionary_key_type>::getLastException() const
{
    const ProfilingScopedReadRWLock read_lock{rw_lock, ProfileEvents::DictCacheLockReadNs};
    return last_exception;
}

template <DictionaryKeyType dictionary_key_type>
const IDictionarySource * CacheDictionary<dictionary_key_type>::getSource() const
{
    /// Mutex required here because of the getSourceAndUpdateIfNeeded() function
    /// which is used from another thread.
    std::lock_guard lock(source_mutex);
    return source_ptr.get();
}

template <DictionaryKeyType dictionary_key_type>
void CacheDictionary<dictionary_key_type>::toParent(const PaddedPODArray<UInt64> & ids [[maybe_unused]], PaddedPODArray<UInt64> & out [[maybe_unused]]) const
{
    if constexpr (dictionary_key_type == DictionaryKeyType::simple)
    {
        /// Run update on requested keys before fetch from storage
        const auto & attribute_name = hierarchical_attribute->name;
        auto result_type = std::make_shared<DataTypeUInt64>();
        auto column = getColumnsImpl({attribute_name}, {result_type->createColumn()}, ids, {nullptr}).front();
        const auto & values = assert_cast<const ColumnVector<UInt64> &>(*column);
        out.assign(values.getData());
    }
    else
        throw Exception("Hierarchy is not supported for complex key CacheDictionary", ErrorCodes::UNSUPPORTED_METHOD);
}


/// Allow to use single value in same way as array.
static inline UInt64 getAt(const PaddedPODArray<UInt64> & arr, const size_t idx)
{
    return arr[idx];
}
static inline UInt64 getAt(const UInt64 & value, const size_t)
{
    return value;
}

template <DictionaryKeyType dictionary_key_type>
template <typename AncestorType>
void CacheDictionary<dictionary_key_type>::isInImpl(const PaddedPODArray<Key> & child_ids, const AncestorType & ancestor_ids, PaddedPODArray<UInt8> & out) const
{
    /// Transform all children to parents until ancestor id or null_value will be reached.

    size_t out_size = out.size();
    memset(out.data(), 0xFF, out_size); /// 0xFF means "not calculated"

    const auto null_value = hierarchical_attribute->null_value.get<UInt64>();

    PaddedPODArray<Key> children(out_size, 0);
    PaddedPODArray<Key> parents(child_ids.begin(), child_ids.end());

    for (size_t i = 0; i < DBMS_HIERARCHICAL_DICTIONARY_MAX_DEPTH; ++i)
    {
        size_t out_idx = 0;
        size_t parents_idx = 0;
        size_t new_children_idx = 0;

        while (out_idx < out_size)
        {
            /// Already calculated
            if (out[out_idx] != 0xFF)
            {
                ++out_idx;
                continue;
            }

            /// No parent
            if (parents[parents_idx] == null_value)
            {
                out[out_idx] = 0;
            }
            /// Found ancestor
            else if (parents[parents_idx] == getAt(ancestor_ids, parents_idx))
            {
                out[out_idx] = 1;
            }
            /// Loop detected
            else if (children[new_children_idx] == parents[parents_idx])
            {
                out[out_idx] = 1;
            }
            /// Found intermediate parent, add this value to search at next loop iteration
            else
            {
                children[new_children_idx] = parents[parents_idx];
                ++new_children_idx;
            }

            ++out_idx;
            ++parents_idx;
        }

        if (new_children_idx == 0)
            break;

        /// Transform all children to its parents.
        children.resize(new_children_idx);
        parents.resize(new_children_idx);

        toParent(children, parents);
    }
}

template <DictionaryKeyType dictionary_key_type>
void CacheDictionary<dictionary_key_type>::isInVectorVector(
    const PaddedPODArray<UInt64> & child_ids, const PaddedPODArray<UInt64> & ancestor_ids, PaddedPODArray<UInt8> & out) const
{
    isInImpl(child_ids, ancestor_ids, out);
}

template <DictionaryKeyType dictionary_key_type>
void CacheDictionary<dictionary_key_type>::isInVectorConstant(const PaddedPODArray<UInt64> & child_ids, const UInt64 ancestor_id, PaddedPODArray<UInt8> & out) const
{
    isInImpl(child_ids, ancestor_id, out);
}

template <DictionaryKeyType dictionary_key_type>
void CacheDictionary<dictionary_key_type>::isInConstantVector(const UInt64 child_id, const PaddedPODArray<UInt64> & ancestor_ids, PaddedPODArray<UInt8> & out) const
{
    /// Special case with single child value.

    const auto null_value = hierarchical_attribute->null_value.get<UInt64>();

    PaddedPODArray<Key> child(1, child_id);
    PaddedPODArray<Key> parent(1);
    std::vector<Key> ancestors(1, child_id);

    /// Iteratively find all ancestors for child.
    for (size_t i = 0; i < DBMS_HIERARCHICAL_DICTIONARY_MAX_DEPTH; ++i)
    {
        toParent(child, parent);

        if (parent[0] == null_value)
            break;

        child[0] = parent[0];
        ancestors.push_back(parent[0]);
    }

    /// Assuming short hierarchy, so linear search is Ok.
    for (size_t i = 0, out_size = out.size(); i < out_size; ++i)
        out[i] = std::find(ancestors.begin(), ancestors.end(), ancestor_ids[i]) != ancestors.end();
}

template <DictionaryKeyType dictionary_key_type>
void CacheDictionary<dictionary_key_type>::setupHierarchicalAttribute()
{
    /// TODO: Move this to DictionaryStructure
    for (const auto & attribute : dict_struct.attributes)
    {
        if (attribute.hierarchical)
        {
            hierarchical_attribute = &attribute;

            if (attribute.underlying_type != AttributeUnderlyingType::utUInt64)
                throw Exception{full_name + ": hierarchical attribute must be UInt64.", ErrorCodes::TYPE_MISMATCH};
        }
    }
}

template <DictionaryKeyType dictionary_key_type>
ColumnPtr CacheDictionary<dictionary_key_type>::getColumn(
    const std::string & attribute_name,
    const DataTypePtr & result_type,
    const Columns & key_columns,
    const DataTypes & key_types,
    const ColumnPtr & default_values_column) const
{
    return getColumns({attribute_name}, { result_type }, key_columns, key_types, { default_values_column }).front();
}

template <DictionaryKeyType dictionary_key_type>
Columns CacheDictionary<dictionary_key_type>::getColumns(
    const Strings & attribute_names,
    const DataTypes &,
    const Columns & key_columns,
    const DataTypes &,
    const Columns & default_values_columns) const
{
    DictionaryKeysExtractor<dictionary_key_type> extractor(key_columns);
    auto & keys = extractor.getKeys();
    return getColumnsImpl(attribute_names, key_columns, keys, default_values_columns);
}

template <DictionaryKeyType dictionary_key_type>
Columns CacheDictionary<dictionary_key_type>::getColumnsImpl(
    const Strings & attribute_names,
    const Columns & key_columns [[maybe_unused]],
    const PaddedPODArray<KeyType> & keys,
    const Columns & default_values_columns) const
{
    DictionaryStorageFetchRequest request(dict_struct, attribute_names);

    FetchResult result_of_fetch_from_storage;

    {
        /// Write lock on storage
        const ProfilingScopedWriteRWLock write_lock{rw_lock, ProfileEvents::DictCacheLockWriteNs};

        auto fetch_result = cache_storage_ptr->fetchColumnsForKeys(keys, request);
        result_of_fetch_from_storage = std::move(fetch_result);
    }

    size_t expired_keys_size = result_of_fetch_from_storage.expired_keys_to_fetched_columns_index.size();
    size_t found_keys_size = result_of_fetch_from_storage.found_keys_to_fetched_columns_index.size();
    size_t not_found_keys_size = keys.size() - (expired_keys_size + found_keys_size);

    ProfileEvents::increment(ProfileEvents::DictCacheKeysExpired, expired_keys_size);
    ProfileEvents::increment(ProfileEvents::DictCacheKeysNotFound, not_found_keys_size);
    ProfileEvents::increment(ProfileEvents::DictCacheKeysHit, found_keys_size);

    query_count.fetch_add(keys.size());
    hit_count.fetch_add(found_keys_size);

    MutableColumns & fetched_columns_from_storage = result_of_fetch_from_storage.fetched_columns;

    std::shared_ptr<CacheDictionaryUpdateUnit<dictionary_key_type>> update_unit;

    if constexpr (dictionary_key_type == DictionaryKeyType::simple)
        update_unit = std::make_shared<CacheDictionaryUpdateUnit<dictionary_key_type>>(std::move(result_of_fetch_from_storage.not_found_or_expired_keys), request);
    else
    {
        auto & indexes_of_rows_to_update = result_of_fetch_from_storage.not_found_or_expired_keys_indexes;

        std::vector<size_t> requested_complex_key_rows;
        requested_complex_key_rows.reserve(indexes_of_rows_to_update.size());
        requested_complex_key_rows.assign(indexes_of_rows_to_update.begin(), indexes_of_rows_to_update.end());

        update_unit = std::make_shared<CacheDictionaryUpdateUnit<dictionary_key_type>>(key_columns, std::move(requested_complex_key_rows), request);
    }

    HashMap<KeyType, size_t> requested_keys_to_fetched_columns_during_update_index;
    MutableColumns fetched_columns_during_update = request.makeAttributesResultColumns();

    bool source_returns_fetched_columns_in_order_of_keys = cache_storage_ptr->returnsFetchedColumnsInOrderOfRequestedKeys();

    if (not_found_keys_size == 0 && expired_keys_size == 0)
    {
        /// All keys were found in storage

        if (source_returns_fetched_columns_in_order_of_keys)
            return request.filterRequestedColumns(fetched_columns_from_storage);
        else
        {
            /// Reorder result from storage to requested keys indexes
            MutableColumns aggregated_columns = aggregateColumnsInOrderOfKeys(
                keys,
                request,
                fetched_columns_from_storage,
                result_of_fetch_from_storage.found_keys_to_fetched_columns_index,
                result_of_fetch_from_storage.expired_keys_to_fetched_columns_index);

            return request.filterRequestedColumns(aggregated_columns);
        }
    }
    else if (not_found_keys_size == 0 && expired_keys_size > 0 && allow_read_expired_keys)
    {
        /// Start async update only if allow read expired keys and all keys are found
        update_queue.tryPushToUpdateQueueOrThrow(update_unit);

        if (source_returns_fetched_columns_in_order_of_keys)
            return request.filterRequestedColumns(fetched_columns_from_storage);
        else
        {
            /// Reorder result from storage to requested keys indexes
            MutableColumns aggregated_columns = aggregateColumnsInOrderOfKeys(
                keys,
                request,
                fetched_columns_from_storage,
                result_of_fetch_from_storage.found_keys_to_fetched_columns_index,
                result_of_fetch_from_storage.expired_keys_to_fetched_columns_index);

            return request.filterRequestedColumns(aggregated_columns);
        }
    }
    else
    {
        /// Start sync update
        update_queue.tryPushToUpdateQueueOrThrow(update_unit);
        update_queue.waitForCurrentUpdateFinish(update_unit);

        requested_keys_to_fetched_columns_during_update_index = std::move(update_unit->requested_keys_to_fetched_columns_during_update_index);
        fetched_columns_during_update = std::move(update_unit->fetched_columns_during_update);
    }

    std::vector<DefaultValueProvider> default_value_providers;
    default_value_providers.reserve(dict_struct.attributes.size());

    size_t default_values_column_index = 0;
    for (const auto & dictionary_attribute : dict_struct.attributes)
    {
         if (request.containsAttribute(dictionary_attribute.name))
        {
            default_value_providers.emplace_back(dictionary_attribute.null_value, default_values_columns[default_values_column_index]);
            ++default_values_column_index;
        }
        else
            default_value_providers.emplace_back(dictionary_attribute.null_value);
    }

    MutableColumns aggregated_columns = aggregateColumns(
        keys,
        request,
        fetched_columns_from_storage,
        result_of_fetch_from_storage.found_keys_to_fetched_columns_index,
        fetched_columns_during_update,
        requested_keys_to_fetched_columns_during_update_index,
        default_value_providers);

    return request.filterRequestedColumns(aggregated_columns);
}

template <DictionaryKeyType dictionary_key_type>
ColumnUInt8::Ptr CacheDictionary<dictionary_key_type>::hasKeys(const Columns & key_columns, const DataTypes &) const
{
    DictionaryKeysExtractor<dictionary_key_type> extractor(key_columns);
    const auto & keys = extractor.getKeys();

    /// We make empty request just to fetch if keys exists
    DictionaryStorageFetchRequest request(dict_struct, {});

    FetchResult result_of_fetch_from_storage;

    {
        /// Write lock on storage
        const ProfilingScopedWriteRWLock write_lock{rw_lock, ProfileEvents::DictCacheLockWriteNs};

        auto fetch_result = cache_storage_ptr->fetchColumnsForKeys(keys, request);
        result_of_fetch_from_storage = std::move(fetch_result);
    }

    size_t expired_keys_size = result_of_fetch_from_storage.expired_keys_to_fetched_columns_index.size();
    size_t found_keys_size = result_of_fetch_from_storage.found_keys_to_fetched_columns_index.size();
    size_t not_found_keys_size = keys.size() - (found_keys_size + expired_keys_size);

    ProfileEvents::increment(ProfileEvents::DictCacheKeysExpired, expired_keys_size);
    ProfileEvents::increment(ProfileEvents::DictCacheKeysNotFound, not_found_keys_size);
    ProfileEvents::increment(ProfileEvents::DictCacheKeysHit, found_keys_size);

    query_count.fetch_add(keys.size());
    hit_count.fetch_add(found_keys_size);

    std::shared_ptr<CacheDictionaryUpdateUnit<dictionary_key_type>> update_unit;

    if constexpr (dictionary_key_type == DictionaryKeyType::simple)
        update_unit = std::make_shared<CacheDictionaryUpdateUnit<dictionary_key_type>>(std::move(result_of_fetch_from_storage.not_found_or_expired_keys), request);
    else
    {
        auto & indexes_of_rows_to_update = result_of_fetch_from_storage.not_found_or_expired_keys_indexes;

        std::vector<size_t> requested_complex_key_rows;
        requested_complex_key_rows.reserve(indexes_of_rows_to_update.size());
        requested_complex_key_rows.assign(indexes_of_rows_to_update.begin(), indexes_of_rows_to_update.end());

        update_unit = std::make_shared<CacheDictionaryUpdateUnit<dictionary_key_type>>(key_columns, std::move(requested_complex_key_rows), request);
    }

    HashMap<KeyType, size_t> requested_keys_to_fetched_columns_during_update_index;

    if (not_found_keys_size == 0 && expired_keys_size == 0)
    {
        /// All keys were found in storage
        return ColumnUInt8::create(keys.size(), true);
    }
    else if (not_found_keys_size == 0 && expired_keys_size > 0 && allow_read_expired_keys)
    {
        /// Start async update only if allow read expired keys and all keys are found
        update_queue.tryPushToUpdateQueueOrThrow(update_unit);

        return ColumnUInt8::create(keys.size(), true);
    }
    else if (not_found_keys_size > 0)
    {
        /// Start sync update
        update_queue.tryPushToUpdateQueueOrThrow(update_unit);
        update_queue.waitForCurrentUpdateFinish(update_unit);

        requested_keys_to_fetched_columns_during_update_index = std::move(update_unit->requested_keys_to_fetched_columns_during_update_index);
    }

    auto result = ColumnUInt8::create(keys.size(), false);
    auto & data = result->getData();

    for (size_t key_index = 0; key_index < keys.size(); ++key_index)
    {
        auto key = keys[key_index];

        if (result_of_fetch_from_storage.found_keys_to_fetched_columns_index.has(key))
        {
            /// Check if key was fetched from cache
            data[key_index] = true;
        }
        else if (requested_keys_to_fetched_columns_during_update_index.has(key))
        {
            /// Check if key was not in cache and was fetched during update
            data[key_index] = true;
        }
    }

    return result;
}

template <DictionaryKeyType dictionary_key_type>
MutableColumns CacheDictionary<dictionary_key_type>::aggregateColumnsInOrderOfKeys(
    const PaddedPODArray<KeyType> & keys,
    const DictionaryStorageFetchRequest & request,
    const MutableColumns & fetched_columns,
    const HashMap<KeyType, size_t> & found_keys_to_fetched_columns_index,
    const HashMap<KeyType, size_t> & expired_keys_to_fetched_columns_index)
{
    MutableColumns aggregated_columns = request.makeAttributesResultColumns();

    for (size_t fetch_request_index = 0; fetch_request_index < request.attributesSize(); ++fetch_request_index)
    {
        if (!request.shouldFillResultColumnWithIndex(fetch_request_index))
            continue;

        const auto & aggregated_column = aggregated_columns[fetch_request_index];
        const auto & fetched_column = fetched_columns[fetch_request_index];

        size_t column_index = 0;

        for (auto key : keys)
        {
            auto * expired_key_iterator = expired_keys_to_fetched_columns_index.find(key);

            if (expired_key_iterator)
            {
                /// Check and insert value if key was fetched from cache
                aggregated_column->insertFrom(*fetched_column, expired_key_iterator->getMapped());
                ++column_index;
                continue;
            }

            /// Check and insert value if key was not in cache and was fetched during update
            auto * found_key_iterator = found_keys_to_fetched_columns_index.find(key);
            if (found_key_iterator)
            {
                aggregated_column->insertFrom(*fetched_column, found_key_iterator->getMapped());
                ++column_index;
                continue;
            }
        }
    }

    return aggregated_columns;
}

template <DictionaryKeyType dictionary_key_type>
MutableColumns CacheDictionary<dictionary_key_type>::aggregateColumns(
        const PaddedPODArray<KeyType> & keys,
        const DictionaryStorageFetchRequest & request,
        const MutableColumns & fetched_columns_from_storage,
        const HashMap<KeyType, size_t> & found_keys_to_fetched_columns_from_storage_index,
        const MutableColumns & fetched_columns_during_update,
        const HashMap<KeyType, size_t> & found_keys_to_fetched_columns_during_update_index,
        const std::vector<DefaultValueProvider> & default_value_providers)
{
    MutableColumns aggregated_columns = request.makeAttributesResultColumns();

    for (size_t fetch_request_index = 0; fetch_request_index < request.attributesSize(); ++fetch_request_index)
    {
        if (!request.shouldFillResultColumnWithIndex(fetch_request_index))
            continue;

        const auto & aggregated_column = aggregated_columns[fetch_request_index];
        const auto & fetched_column_from_storage = fetched_columns_from_storage[fetch_request_index];
        const auto & fetched_column_during_update = fetched_columns_during_update[fetch_request_index];
        const auto & default_value_provider = default_value_providers[fetch_request_index];

        for (size_t key_index = 0; key_index < keys.size(); ++key_index)
        {
            auto key = keys[key_index];

            const auto * find_iterator_in_cache = found_keys_to_fetched_columns_from_storage_index.find(key);
            if (find_iterator_in_cache)
            {
                /// Check and insert value if key was fetched from cache
                aggregated_column->insertFrom(*fetched_column_from_storage, find_iterator_in_cache->getMapped());
                continue;
            }

            /// Check and insert value if key was not in cache and was fetched during update
            const auto * find_iterator_in_fetch_during_update = found_keys_to_fetched_columns_during_update_index.find(key);
            if (find_iterator_in_fetch_during_update)
            {
                aggregated_column->insertFrom(*fetched_column_during_update, find_iterator_in_fetch_during_update->getMapped());
                continue;
            }

            /// Insert default value
            aggregated_column->insert(default_value_provider.getDefaultValue(key_index));
        }
    }

    return aggregated_columns;
}

template <DictionaryKeyType dictionary_key_type>
BlockInputStreamPtr CacheDictionary<dictionary_key_type>::getBlockInputStream(const Names & column_names, size_t max_block_size) const
{
    using BlockInputStreamType = DictionaryBlockInputStream<Key>;

    if constexpr (dictionary_key_type == DictionaryKeyType::simple)
        return std::make_shared<BlockInputStreamType>(shared_from_this(), max_block_size, cache_storage_ptr->getCachedSimpleKeys(), column_names);
    else
    {
        auto keys = cache_storage_ptr->getCachedComplexKeys();
        return std::make_shared<BlockInputStreamType>(shared_from_this(), max_block_size, keys, column_names);
    }
}

template <DictionaryKeyType dictionary_key_type>
void CacheDictionary<dictionary_key_type>::update(CacheDictionaryUpdateUnitPtr<dictionary_key_type> & update_unit_ptr)
{
    CurrentMetrics::Increment metric_increment{CurrentMetrics::DictCacheRequests};
    ProfileEvents::increment(ProfileEvents::DictCacheKeysRequested, update_unit_ptr->requested_simple_keys.size());

    size_t found_num = 0;

    std::vector<UInt64> requested_keys_vector;

    size_t requested_keys_size = 0;

    if constexpr (dictionary_key_type == DictionaryKeyType::simple)
    {
        const PaddedPODArray<UInt64> & requested_keys = update_unit_ptr->requested_simple_keys;

        requested_keys_vector.reserve(requested_keys.size());
        requested_keys_vector.assign(requested_keys.begin(), requested_keys.end());

        requested_keys_size = requested_keys.size();
    }
    else
        requested_keys_size = update_unit_ptr->requested_complex_key_rows.size();

    const auto & fetch_request = update_unit_ptr->request;

    const auto now = std::chrono::system_clock::now();

    if (now > backoff_end_time.load())
    {
        try
        {
            auto current_source_ptr = getSourceAndUpdateIfNeeded();

            Stopwatch watch;
            BlockInputStreamPtr stream;

            if constexpr (dictionary_key_type == DictionaryKeyType::simple)
                stream = current_source_ptr->loadIds(requested_keys_vector);
            else
            {
                const auto & requested_complex_keys_columns = update_unit_ptr->requested_complex_key_columns;
                const auto & requested_complex_keys_rows = update_unit_ptr->requested_complex_key_rows;

                stream = current_source_ptr->loadKeys(requested_complex_keys_columns, requested_complex_keys_rows);
            }

            stream->readPrefix();

            /// Lock for cache modification
            ProfilingScopedWriteRWLock write_lock{rw_lock, ProfileEvents::DictCacheLockWriteNs};

            size_t skip_keys_size_offset = dict_struct.getKeysSize();

            while (Block block = stream->read())
            {
                Columns key_columns;
                key_columns.reserve(skip_keys_size_offset);

                auto block_columns = block.getColumns();

                /// Remove keys columns
                for (size_t i = 0; i < skip_keys_size_offset; ++i)
                {
                    key_columns.emplace_back(*block_columns.begin());
                    block_columns.erase(block_columns.begin());
                }

                DictionaryKeysExtractor<dictionary_key_type> keys_extractor(key_columns, *update_unit_ptr->complex_key_arena);
                const auto & keys = keys_extractor.getKeys();

                cache_storage_ptr->insertColumnsForKeys(keys, block_columns);

                for (size_t index_of_attribute = 0; index_of_attribute < update_unit_ptr->fetched_columns_during_update.size(); ++index_of_attribute)
                {
                    auto & column_to_update = update_unit_ptr->fetched_columns_during_update[index_of_attribute];

                    if (fetch_request.shouldFillResultColumnWithIndex(index_of_attribute))
                    {
                        auto column = block.safeGetByPosition(skip_keys_size_offset + index_of_attribute).column;
                        column_to_update->insertRangeFrom(*column, 0, keys.size());
                    }
                }

                for (size_t i = 0; i < keys.size(); ++i)
                {
                    auto fetched_key_from_source = keys[i];
                    size_t column_offset = found_num;
                    update_unit_ptr->requested_keys_to_fetched_columns_during_update_index[fetched_key_from_source] = column_offset + i;
                }

                found_num += keys.size();
            }

            stream->readSuffix();

            error_count = 0;
            last_exception = std::exception_ptr{};
            backoff_end_time = std::chrono::system_clock::time_point{};

            ProfileEvents::increment(ProfileEvents::DictCacheRequestTimeNs, watch.elapsed());
        }
        catch (...)
        {
            /// Lock just for last_exception safety
            ProfilingScopedWriteRWLock write_lock{rw_lock, ProfileEvents::DictCacheLockWriteNs};
            ++error_count;
            last_exception = std::current_exception();
            backoff_end_time = now + std::chrono::seconds(calculateDurationWithBackoff(rnd_engine, error_count));

            tryLogException(last_exception, log,
                            "Could not update cache dictionary '" + getDictionaryID().getNameForLogs() +
                            "', next update is scheduled at " + ext::to_string(backoff_end_time.load()));
            try
            {
                std::rethrow_exception(last_exception);
            }
            catch (...)
            {
                throw DB::Exception(ErrorCodes::CACHE_DICTIONARY_UPDATE_FAIL,
                    "Update failed for dictionary {} : {}",
                    getDictionaryID().getNameForLogs(),
                    getCurrentExceptionMessage(true /*with stack trace*/,
                                               true /*check embedded stack trace*/));
            }
        }

        ProfileEvents::increment(ProfileEvents::DictCacheKeysRequestedMiss, requested_keys_size - found_num);
        ProfileEvents::increment(ProfileEvents::DictCacheKeysRequestedFound, found_num);
        ProfileEvents::increment(ProfileEvents::DictCacheRequests);
    }
    else
    {
        /// Won't request source for keys
        throw DB::Exception(ErrorCodes::CACHE_DICTIONARY_UPDATE_FAIL,
            "Query contains keys that are not present in cache or expired. Could not update cache dictionary {} now, because nearest update is scheduled at {}. Try again later.",
            getDictionaryID().getNameForLogs(),
            ext::to_string(backoff_end_time.load()));
    }
}

template class CacheDictionary<DictionaryKeyType::simple>;
template class CacheDictionary<DictionaryKeyType::complex>;

namespace
{

    CacheDictionaryStorageConfiguration parseCacheStorageConfiguration(
        const std::string & full_name,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & layout_prefix,
        const DictionaryLifetime & dict_lifetime,
        bool is_complex)
    {
        std::string dictionary_type_prefix = is_complex ? ".complex_key_cache." : ".cache.";
        std::string dictionary_configuration_prefix = layout_prefix + dictionary_type_prefix;

        const size_t size = config.getUInt64(dictionary_configuration_prefix + "size_in_cells");
        if (size == 0)
            throw Exception{full_name + ": dictionary of layout 'cache' cannot have 0 cells",
                            ErrorCodes::TOO_SMALL_BUFFER_SIZE};

        const size_t strict_max_lifetime_seconds =
                config.getUInt64(dictionary_configuration_prefix + "strict_max_lifetime_seconds",
                static_cast<size_t>(dict_lifetime.max_sec));

        // size_t rounded_size = roundUpToPowerOfTwoOrZero(size);

        CacheDictionaryStorageConfiguration storage_configuration {
            size,
            strict_max_lifetime_seconds,
            dict_lifetime
        };

        return storage_configuration;
    }

    SSDCacheDictionaryStorageConfiguration parseSSDCacheStorageConfiguration(
        const std::string & full_name,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & layout_prefix,
        const DictionaryLifetime & dict_lifetime,
        bool is_complex)
    {
        std::string dictionary_type_prefix = is_complex ? ".complex_key_ssd_cache." : ".ssd_cache.";
        std::string dictionary_configuration_prefix = layout_prefix + dictionary_type_prefix;

        const size_t strict_max_lifetime_seconds =
                config.getUInt64(dictionary_configuration_prefix + "strict_max_lifetime_seconds",
                static_cast<size_t>(dict_lifetime.max_sec));

        static constexpr size_t DEFAULT_SSD_BLOCK_SIZE_BYTES = DEFAULT_AIO_FILE_BLOCK_SIZE;
        static constexpr size_t DEFAULT_FILE_SIZE_BYTES = 4 * 1024 * 1024 * 1024ULL;
        static constexpr size_t DEFAULT_READ_BUFFER_SIZE_BYTES = 16 * DEFAULT_SSD_BLOCK_SIZE_BYTES;
        static constexpr size_t DEFAULT_WRITE_BUFFER_SIZE_BYTES = DEFAULT_SSD_BLOCK_SIZE_BYTES;

        static constexpr size_t DEFAULT_MAX_STORED_KEYS = 100000;
        static constexpr size_t DEFAULT_PARTITIONS_COUNT = 16;

        const size_t max_partitions_count = config.getInt(dictionary_configuration_prefix + "ssd_cache.max_partitions_count", DEFAULT_PARTITIONS_COUNT);

        const size_t block_size = config.getInt(dictionary_configuration_prefix + "block_size", DEFAULT_READ_BUFFER_SIZE_BYTES);
        const size_t file_blocks_size = config.getInt64(dictionary_configuration_prefix + "file_size", DEFAULT_FILE_SIZE_BYTES);
        if (file_blocks_size % block_size != 0)
            throw Exception{full_name + ": file_size must be a multiple of block_size", ErrorCodes::BAD_ARGUMENTS};

        const size_t read_buffer_blocks_size = config.getInt64(dictionary_configuration_prefix + "read_buffer_size", DEFAULT_READ_BUFFER_SIZE_BYTES);
        if (read_buffer_blocks_size % block_size != 0)
            throw Exception{full_name + ": read_buffer_size must be a multiple of block_size", ErrorCodes::BAD_ARGUMENTS};

        const size_t write_buffer_blocks_size = config.getInt64(dictionary_configuration_prefix + "write_buffer_size", DEFAULT_WRITE_BUFFER_SIZE_BYTES);
        if (write_buffer_blocks_size % block_size != 0)
            throw Exception{full_name + ": write_buffer_size must be a multiple of block_size", ErrorCodes::BAD_ARGUMENTS};

        auto directory_path = config.getString(dictionary_configuration_prefix + "path");
        if (directory_path.empty())
            throw Exception{full_name + ": dictionary of layout 'ssd_cache' cannot have empty path",
                            ErrorCodes::BAD_ARGUMENTS};
        if (directory_path.at(0) != '/')
            directory_path = std::filesystem::path{config.getString("path")}.concat(directory_path).string();

        const size_t max_stored_keys_in_partition = config.getInt64(dictionary_configuration_prefix + "max_stored_keys", DEFAULT_MAX_STORED_KEYS);
        /// TODO: FIX
        // const size_t rounded_size = roundUpToPowerOfTwoOrZero(max_stored_keys_in_partition);

        SSDCacheDictionaryStorageConfiguration configuration {
            strict_max_lifetime_seconds,
            dict_lifetime,
            directory_path,
            max_partitions_count,
            max_stored_keys_in_partition,
            block_size,
            file_blocks_size / block_size,
            read_buffer_blocks_size / block_size,
            write_buffer_blocks_size / block_size
        };

        return configuration;
    }

    CacheDictionaryUpdateQueueConfiguration parseCacheDictionaryUpdateQueueConfiguration(
        const std::string & full_name,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & layout_prefix,
        bool is_complex)
    {
        std::string type = is_complex ? "complex_key_cache" : "cache";

        const size_t max_update_queue_size =
                config.getUInt64(layout_prefix + ".cache.max_update_queue_size", 100000);
        if (max_update_queue_size == 0)
            throw Exception{full_name + ": dictionary of layout'" +  type + "'cannot have empty update queue of size 0",
                            ErrorCodes::TOO_SMALL_BUFFER_SIZE};

        const size_t update_queue_push_timeout_milliseconds =
                config.getUInt64(layout_prefix + ".cache.update_queue_push_timeout_milliseconds", 10);
        if (update_queue_push_timeout_milliseconds < 10)
            throw Exception{full_name + ": dictionary of layout'" + type + "'have too little update_queue_push_timeout",
                            ErrorCodes::BAD_ARGUMENTS};

        const size_t query_wait_timeout_milliseconds =
                config.getUInt64(layout_prefix + ".cache.query_wait_timeout_milliseconds", 60000);

        const size_t max_threads_for_updates =
                config.getUInt64(layout_prefix + ".max_threads_for_updates", 4);
        if (max_threads_for_updates == 0)
            throw Exception{full_name + ": dictionary of layout'"+ type +"'cannot have zero threads for updates.",
                            ErrorCodes::BAD_ARGUMENTS};

        CacheDictionaryUpdateQueueConfiguration update_queue_configuration {
            max_update_queue_size,
            max_threads_for_updates,
            update_queue_push_timeout_milliseconds,
            query_wait_timeout_milliseconds };

        return update_queue_configuration;
    }
}

void registerDictionaryCache(DictionaryFactory & factory)
{
    auto create_simple_cache_layout = [=](const std::string & full_name,
                             const DictionaryStructure & dict_struct,
                             const Poco::Util::AbstractConfiguration & config,
                             const std::string & config_prefix,
                             DictionarySourcePtr source_ptr) -> DictionaryPtr
    {
        if (dict_struct.key)
            throw Exception{"'key' is not supported for dictionary of layout 'cache'",
                            ErrorCodes::UNSUPPORTED_METHOD};

        if (dict_struct.range_min || dict_struct.range_max)
            throw Exception{full_name
                                + ": elements .structure.range_min and .structure.range_max should be defined only "
                                  "for a dictionary of layout 'range_hashed'",
                            ErrorCodes::BAD_ARGUMENTS};

        const bool require_nonempty = config.getBool(config_prefix + ".require_nonempty", false);
        if (require_nonempty)
            throw Exception{full_name + ": dictionary of layout 'cache' cannot have 'require_nonempty' attribute set",
                            ErrorCodes::BAD_ARGUMENTS};


        const auto & layout_prefix = config_prefix + ".layout";

        const auto dict_id = StorageID::fromDictionaryConfig(config, config_prefix);

        const DictionaryLifetime dict_lifetime{config, config_prefix + ".lifetime"};

        const bool allow_read_expired_keys =
                config.getBool(layout_prefix + ".cache.allow_read_expired_keys", false);

        auto storage_configuration = parseCacheStorageConfiguration(full_name, config, layout_prefix, dict_lifetime, false);
        auto storage = std::make_shared<CacheDictionaryStorage<DictionaryKeyType::simple>>(storage_configuration);

        auto update_queue_configuration = parseCacheDictionaryUpdateQueueConfiguration(full_name, config, layout_prefix, false);

        return std::make_unique<CacheDictionary<DictionaryKeyType::simple>>(
                dict_id,
                dict_struct,
                std::move(source_ptr),
                storage,
                update_queue_configuration,
                dict_lifetime,
                allow_read_expired_keys);
    };

    factory.registerLayout("cache", create_simple_cache_layout, false);

    auto create_complex_key_cache_layout = [=](const std::string & full_name,
                             const DictionaryStructure & dict_struct,
                             const Poco::Util::AbstractConfiguration & config,
                             const std::string & config_prefix,
                             DictionarySourcePtr source_ptr) -> DictionaryPtr
    {
        if (dict_struct.id)
            throw Exception{"'id' is not supported for dictionary of layout 'complex_key_cache'",
                            ErrorCodes::UNSUPPORTED_METHOD};

        if (dict_struct.range_min || dict_struct.range_max)
            throw Exception{full_name
                                + ": elements .structure.range_min and .structure.range_max should be defined only "
                                  "for a dictionary of layout 'range_hashed'",
                            ErrorCodes::BAD_ARGUMENTS};

        const bool require_nonempty = config.getBool(config_prefix + ".require_nonempty", false);
        if (require_nonempty)
            throw Exception{full_name + ": dictionary of layout 'cache' cannot have 'require_nonempty' attribute set",
                            ErrorCodes::BAD_ARGUMENTS};


        const auto & layout_prefix = config_prefix + ".layout";

        const auto dict_id = StorageID::fromDictionaryConfig(config, config_prefix);

        const DictionaryLifetime dict_lifetime{config, config_prefix + ".lifetime"};

        const bool allow_read_expired_keys =
                config.getBool(layout_prefix + ".cache.allow_read_expired_keys", false);

        auto storage_configuration = parseCacheStorageConfiguration(full_name, config, layout_prefix, dict_lifetime, true);
        auto storage = std::make_shared<CacheDictionaryStorage<DictionaryKeyType::complex>>(storage_configuration);

        auto update_queue_configuration = parseCacheDictionaryUpdateQueueConfiguration(full_name, config, layout_prefix, true);

        return std::make_unique<CacheDictionary<DictionaryKeyType::complex>>(
                dict_id,
                dict_struct,
                std::move(source_ptr),
                storage,
                update_queue_configuration,
                dict_lifetime,
                allow_read_expired_keys);
    };

    factory.registerLayout("complex_key_cache", create_complex_key_cache_layout, true);

    auto create_simple_ssd_cache_layout = [=](const std::string & full_name,
                             const DictionaryStructure & dict_struct,
                             const Poco::Util::AbstractConfiguration & config,
                             const std::string & config_prefix,
                             DictionarySourcePtr source_ptr) -> DictionaryPtr
    {
        if (dict_struct.key)
            throw Exception{"'key' is not supported for dictionary of layout 'cache'",
                            ErrorCodes::UNSUPPORTED_METHOD};

        if (dict_struct.range_min || dict_struct.range_max)
            throw Exception{full_name
                                + ": elements .structure.range_min and .structure.range_max should be defined only "
                                  "for a dictionary of layout 'range_hashed'",
                            ErrorCodes::BAD_ARGUMENTS};

        const bool require_nonempty = config.getBool(config_prefix + ".require_nonempty", false);
        if (require_nonempty)
            throw Exception{full_name + ": dictionary of layout 'cache' cannot have 'require_nonempty' attribute set",
                            ErrorCodes::BAD_ARGUMENTS};

        const auto & layout_prefix = config_prefix + ".layout";

        const auto dict_id = StorageID::fromDictionaryConfig(config, config_prefix);

        const DictionaryLifetime dict_lifetime{config, config_prefix + ".lifetime"};

        const bool allow_read_expired_keys =
                config.getBool(layout_prefix + ".cache.allow_read_expired_keys", false);

        auto storage_configuration = parseSSDCacheStorageConfiguration(full_name, config, layout_prefix, dict_lifetime, false);
        auto storage = std::make_shared<SSDCacheDictionaryStorage<DictionaryKeyType::simple>>(storage_configuration);

        auto update_queue_configuration = parseCacheDictionaryUpdateQueueConfiguration(full_name, config, layout_prefix, false);

        return std::make_unique<CacheDictionary<DictionaryKeyType::simple>>(
                dict_id,
                dict_struct,
                std::move(source_ptr),
                storage,
                update_queue_configuration,
                dict_lifetime,
                allow_read_expired_keys);
    };

    factory.registerLayout("ssd_cache", create_simple_ssd_cache_layout, false);

    auto create_complex_key_ssd_cache_layout = [=](const std::string & full_name,
                             const DictionaryStructure & dict_struct,
                             const Poco::Util::AbstractConfiguration & config,
                             const std::string & config_prefix,
                             DictionarySourcePtr source_ptr) -> DictionaryPtr
    {
        if (dict_struct.id)
            throw Exception{"'id' is not supported for dictionary of layout 'complex_key_cache'",
                            ErrorCodes::UNSUPPORTED_METHOD};

        if (dict_struct.range_min || dict_struct.range_max)
            throw Exception{full_name
                                + ": elements .structure.range_min and .structure.range_max should be defined only "
                                  "for a dictionary of layout 'range_hashed'",
                            ErrorCodes::BAD_ARGUMENTS};

        const bool require_nonempty = config.getBool(config_prefix + ".require_nonempty", false);
        if (require_nonempty)
            throw Exception{full_name + ": dictionary of layout 'cache' cannot have 'require_nonempty' attribute set",
                            ErrorCodes::BAD_ARGUMENTS};


        const auto & layout_prefix = config_prefix + ".layout";

        const auto dict_id = StorageID::fromDictionaryConfig(config, config_prefix);

        const DictionaryLifetime dict_lifetime{config, config_prefix + ".lifetime"};

        const bool allow_read_expired_keys =
                config.getBool(layout_prefix + ".cache.allow_read_expired_keys", false);

        auto storage_configuration = parseSSDCacheStorageConfiguration(full_name, config, layout_prefix, dict_lifetime, true);
        auto storage = std::make_shared<SSDCacheDictionaryStorage<DictionaryKeyType::complex>>(storage_configuration);

        auto update_queue_configuration = parseCacheDictionaryUpdateQueueConfiguration(full_name, config, layout_prefix, true);

        return std::make_unique<CacheDictionary<DictionaryKeyType::complex>>(
                dict_id,
                dict_struct,
                std::move(source_ptr),
                storage,
                update_queue_configuration,
                dict_lifetime,
                allow_read_expired_keys);
    };

    factory.registerLayout("complex_key_ssd_cache", create_complex_key_ssd_cache_layout, true);
}

}
