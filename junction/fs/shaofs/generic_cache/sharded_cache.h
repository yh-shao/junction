#pragma once
#include <vector>
#include <functional>
#include "replace_policy.h"
#include "cache.h"
#include <bit>

/*
注意：
  * shard 数目会自动增大为 2 的幂 （可能会被传入的 shard_num 参数大），从而整个 Cache 的容量可能会比传入的参数 totalcapacity 大
*/

template <typename Key, typename Value, typename Hash = std::hash<Key>>
class ShardedCache {
public:
    using ShardType = Cache<Key, Value, Hash>;
    using Handle    = typename ShardType::Handle;

private:
    size_t num_shards, shard_capacity, total_capacity;
    std::vector<ShardType*> shards;    
    std::vector<ReplacementPolicy<Key, Value>*> policies; 

    Hash hasher;
    inline size_t get_shard_idx(const Key& key) const { return hasher(key) & (num_shards - 1); }

public:
    ShardedCache(size_t totalcapacity, size_t shard_num, Backend<Key, Value>* backend, std::function<ReplacementPolicy<Key, Value>*()> policy_factory, void* objpool_base_addr = nullptr)
    {
        num_shards     = std::bit_ceil(shard_num);    // 保证为 2 的幂，从而可以使用位运算优化取模运算
        shard_capacity = CEIL(totalcapacity, num_shards); 
        total_capacity = shard_capacity * num_shards;

        shards.reserve(num_shards);
        policies.reserve(num_shards);

        size_t stride = ShardType::CacheEntry_footprint(shard_capacity);
        char* curr_ptr = static_cast<char*>(objpool_base_addr);
        for (size_t i = 0; i < num_shards; i++) 
        {
            ReplacementPolicy<Key, Value>* new_policy = policy_factory(); 
            policies.push_back(new_policy);

            void* shard_pool_addr = (objpool_base_addr) ? (curr_ptr + i * stride) : nullptr; 
            shards.push_back(new ShardType(this->shard_capacity, backend, new_policy, shard_pool_addr)); 
        }
    }
    
    ~ShardedCache() 
    {
        for (auto shard  : shards)   delete shard;     
        for (auto policy : policies) delete policy; 
    }

    Handle getHandle(const Key& key, bool fetch_on_miss = true) { return shards[get_shard_idx(key)]->getHandle(key, fetch_on_miss); }
    bool   get(const Key& key, Value& val)       { return shards[get_shard_idx(key)]->get(key, val);    }
    bool   put(const Key& key, const Value& val) { return shards[get_shard_idx(key)]->put(key, val);    }
    void   invalidate(const Key& key)            { return shards[get_shard_idx(key)]->invalidate(key);  }
    bool   flush_entry(const Key& key)           { return shards[get_shard_idx(key)]->flush_entry(key); }
    Handle find_cached(const Key& key)           { return shards[get_shard_idx(key)]->find_cached(key); }

    void flush_all() { for (auto shard : shards) shard->flush_all(); }
    
    size_t get_shard_count()    { return num_shards;     }
    size_t get_shard_capacity() { return shard_capacity; }
    size_t get_total_capacity() { return total_capacity; }

    static size_t CacheEntry_footprint(size_t total_capacity, size_t shard_num) 
    { 
        size_t actual_shards = std::bit_ceil(shard_num);
        size_t shard_cap     = CEIL(total_capacity, actual_shards);
        return ShardType::CacheEntry_footprint(shard_cap) * actual_shards; 
    }
    static size_t real_capacity(size_t total_capacity, size_t shard_num) 
    { 
        size_t actual_shards = std::bit_ceil(shard_num);
        size_t shard_cap     = CEIL(total_capacity, actual_shards);
        return actual_shards * shard_cap; 
    }
};