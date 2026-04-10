#pragma once
#include <vector>
#include <functional>
#include "replace_policy.h"
#include "cache.h"

/**
 * @brief Multi-shard Wrapper for the Cache.
 * Reduces lock contention by splitting the key space into N buckets (shards).
 */
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class ShardedCache {
public:
    using ShardType = Cache<Key, Value, Hash>;
    using Handle    = typename ShardType::Handle;

    static size_t calculate_shard_stride(size_t shard_cap) 
    {
        size_t raw_size = ShardType::calculate_memory_size(shard_cap); 
        size_t align    = ShardType::calculate_alignment();            
        return CEIL(raw_size, align) * align;
    }
    
    static size_t calculate_total_memory(size_t total_capacity, size_t num_shards) 
    {
        if (num_shards == 0) num_shards = 1;
        size_t shard_cap = CEIL(total_capacity, num_shards);
        return calculate_shard_stride(shard_cap) * num_shards;
    }
    
    static size_t calculate_alignment() { return ShardType::calculate_alignment(); }

private:
    size_t num_shards, shard_capacity, total_capacity;
    std::vector<ShardType*> shards;    
    
    std::vector<ReplacementPolicy<Key, Value>*> policies; 

    Hash hasher;
    inline size_t get_shard_idx(const Key& key) const { return hasher(key) % num_shards; }

public:
    ShardedCache(size_t total_capacity, size_t num_shards, Backend<Key, Value>* backend, std::function<ReplacementPolicy<Key, Value>*()> policy_factory, void* base_addr = nullptr)
    {
        if (num_shards == 0) num_shards = 1;
        this->num_shards = num_shards;
        this->shard_capacity = CEIL(total_capacity, num_shards); 
        this->total_capacity = this->shard_capacity * this->num_shards;

        size_t stride = (base_addr) ? calculate_shard_stride(this->shard_capacity) : 0;
        char* curr_ptr = static_cast<char*>(base_addr);

        shards.reserve(num_shards);
        policies.reserve(num_shards);

        for (size_t i = 0; i < num_shards; i++) 
        {
            ReplacementPolicy<Key, Value>* new_policy = policy_factory(); 
            policies.push_back(new_policy);

            void* shard_pool_addr = (base_addr) ? (curr_ptr + i * stride) : nullptr; 
            
            // 注意：Map 的桶数组内存(new EntryType*[])将自动在堆上分配。因为它只在启动时分配一次，所以完全满足运行时 0 内存分配的苛刻要求。
            shards.push_back(new ShardType(this->shard_capacity, backend, new_policy, shard_pool_addr, nullptr)); 
        }
    }
    
    ~ShardedCache() 
    {
        for (auto shard : shards) delete shard;     
        for (auto policy : policies) delete policy; 
    }

    Handle getHandle(const Key& key)             { return shards[get_shard_idx(key)]->getHandle(key);       }
    bool   get(const Key& key, Value& val)       { return shards[get_shard_idx(key)]->get(key, val);        }
    bool   put(const Key& key, const Value& val) { return shards[get_shard_idx(key)]->put(key, val);        }
    void   invalidate(const Key& key)            { return shards[get_shard_idx(key)]->invalidate(key);      }
    bool   flush_entry(const Key& key)           { return shards[get_shard_idx(key)]->flush_entry(key);     }

    void flush() { for (auto shard : shards) shard->flush(); }
    
    size_t get_shard_count()    { return num_shards;     }
    size_t get_shard_capacity() { return shard_capacity; }
    size_t get_total_capacity() { return total_capacity; }
};