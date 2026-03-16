#pragma once

#include <vector>
#include <functional>
#include <memory>
#include "replace_policy.h"
#include "cache.h"

/**
 * @brief Multi-shard Wrapper for the Cache.
 * Reduces lock contention by splitting the key space into N buckets (shards).
 * Note: All shards share the same backend pointer (Backend is assumed to be thread-safe or stateless).
 */
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class ShardedCache {
public:
    using ShardType = Cache<Key, Value>;
    using Handle = typename ShardType::Handle;

    static size_t calculate_shard_stride(size_t shard_cap) 
    {
        size_t raw_size = ShardType::calculate_memory_size(shard_cap);  // 获取单个分片所需的原始大小
        size_t align    = ShardType::calculate_alignment();             // 获取对齐要求
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
    std::vector<ShardType*>              shards;    // Each shard is a standalone Cache instance with its own lock.    
    std::vector<ReplacementPolicy<Key>*> policies;  // We need to track policies created by the factory to delete them later, since the inner Cache doesn't take ownership of deleting the policy pointer.

    Hash hasher;
    inline size_t get_shard_idx(const Key& key) const { return hasher(key) % num_shards; }  // Determine which shard a key belongs to.

public:
    /**
     * @param total_capacity Total capacity of the cache (will be divided evenly among shards).
     * @param num_shards     Number of shards (partitions). Higher number = less contention but more overhead.
     * @param backend        Pointer to the backend (shared across all shards).
     * @param policy_factory A function that returns a NEW ReplacementPolicy instance for each shard.
     */
    ShardedCache(size_t total_capacity, size_t num_shards, Backend<Key, Value>* backend, std::function<ReplacementPolicy<Key>*()> policy_factory, void* base_addr = nullptr)
    {
        if (num_shards == 0) num_shards = 1;
        this->num_shards = num_shards;
        this->shard_capacity = CEIL(total_capacity, num_shards);  // Ceiling division
        this->total_capacity = this->shard_capacity * this->num_shards;

        size_t stride = (base_addr) ? calculate_shard_stride(this->shard_capacity) : 0;
        char* curr_ptr = static_cast<char*>(base_addr);

        shards.reserve(num_shards);
        policies.reserve(num_shards);

        for (size_t i = 0; i < num_shards; i++) 
        {
            ReplacementPolicy<Key>* new_policy = policy_factory();  // 创建独立的 Policy
            policies.push_back(new_policy);

            void* shard_pool_addr = (base_addr) ? (curr_ptr + i * stride) : nullptr;  // 计算当前分片的内存地址
            shards.push_back(new ShardType(this->shard_capacity, backend, new_policy, shard_pool_addr));  // 注意：backend 是所有分片共享的 (前提是 Backend 线程安全)
        }
    }
    ~ShardedCache() 
    {
        for (auto shard : shards) delete shard;     // Delete Cache Shards
        for (auto policy : policies) delete policy; // Delete Policies (since we created them via factory)
    }

    // Hash -> Locate Shard -> Delegate to that Shard
    Handle get(const Key& key)                  { return shards[get_shard_idx(key)]->get(key);      }
    void  put(const Key& key, const Value& val) {        shards[get_shard_idx(key)]->put(key, val); }
    
    void flush()  // We iterate over all shards. Note that this doesn't offer a global snapshot, but ensures all currently dirty data in all shards is flushed.
    {
        for (auto shard : shards) 
            shard->flush();
    }

    size_t get_shard_count()    { return num_shards;     }
    size_t get_shard_capacity() { return shard_capacity; }
    size_t get_total_capacity() { return total_capacity; }
    size_t get_total_size() 
    {
        size_t total = 0;
        for (auto shard : shards) total += shard->size();
        return total;
    }
};