#pragma once
#include <functional>
#include <cstddef>
#include <bit>

/** 侵入式哈希表
注意：
  * bucket 数目会自动增大为 2 的幂 （可能会被传入的 bucket 参数大），之后 bucket 数目将是固定的，不支持动态扩容
  * 本 hashmap 不含内置锁，并发安全由上层负责 
  * EntryType 必须包含 KeyType key 和 EntryType* hash_next 两个成员变量
  * 表中每个 key 都是唯一的，不支持重复插入
  * 经过初始化后，运行时无需内存分配
*/

template <typename Key, typename EntryType, typename Hash = std::hash<Key>>
class IntrusiveCacheMap {
private:
    size_t num_buckets;    // num_buckets 必须是 2 的幂，从而才能用位运算来优化取模运算
    EntryType** buckets;
    size_t current_size;   // hashmap 中当前存储的元素数量

    Hash hasher;

    static inline size_t mix_for_bucket(size_t h)
    {
        if constexpr (sizeof(size_t) >= sizeof(uint64_t))
        {
            uint64_t x = static_cast<uint64_t>(h);
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdULL;
            x ^= x >> 33;
            x *= 0xc4ceb9fe1a85ec53ULL;
            x ^= x >> 33;
            return static_cast<size_t>(x);
        }
        else
        {
            uint32_t x = static_cast<uint32_t>(h);
            x ^= x >> 16;
            x *= 0x7feb352dU;
            x ^= x >> 15;
            x *= 0x846ca68bU;
            x ^= x >> 16;
            return static_cast<size_t>(x);
        }
    }

    inline size_t get_bucket_idx(const Key& key) const
    {
        return mix_for_bucket(hasher(key)) & (num_buckets - 1);
    }   

public:
    IntrusiveCacheMap(size_t buckets_count)  : num_buckets(std::bit_ceil(buckets_count)), current_size(0)
    {
        buckets = new EntryType*[num_buckets]();
    }
    ~IntrusiveCacheMap() 
    {
        delete[] buckets;
    }
    IntrusiveCacheMap(const IntrusiveCacheMap&)            = delete;
    IntrusiveCacheMap& operator=(const IntrusiveCacheMap&) = delete;
    IntrusiveCacheMap(IntrusiveCacheMap&&)                 = delete; 
    IntrusiveCacheMap& operator=(IntrusiveCacheMap&&)      = delete;

    EntryType* find(const Key& key) const 
    {
        EntryType* curr = buckets[get_bucket_idx(key)];
        while (curr) 
        {
            if (curr->key == key) return curr;
            curr = curr->hash_next;
        }
        return nullptr;
    }

    size_t probe_depth(const Key& key) const
    {
        size_t depth = 0;
        EntryType* curr = buckets[get_bucket_idx(key)];
        while (curr)
        {
            depth++;
            if (curr->key == key) return depth;
            curr = curr->hash_next;
        }
        return 0;
    }

    void insert(EntryType* entry)   // 将 entry 串到链上（零拷贝）
    {
        size_t idx = get_bucket_idx(entry->key);
        EntryType* curr = buckets[idx];
        while (curr)                             // 遍历当前桶，检查是否已经存在相同的 key
        {
            if (curr->key == entry->key) return; // 已经存在，直接返回（一个key只能对应一个entry，重复插入不处理）
            curr = curr->hash_next;
        }
        
        // 头插法插入新节点
        entry->hash_next = buckets[idx];
        buckets[idx] = entry;
        current_size++;
    }

    void remove(const Key& key)    // 将 entry 从链上摘除
    {
        size_t idx = get_bucket_idx(key);
        EntryType** curr = &buckets[idx];
        while (*curr) 
        {
            if ((*curr)->key == key) 
            {
                EntryType* to_remove = *curr;
                *curr = (*curr)->hash_next;
                to_remove->hash_next = nullptr;
                current_size--;
                return;
            }
            curr = &(*curr)->hash_next;
        }
    }

    size_t     size()                      const { return current_size; }
    EntryType* get_bucket_head(size_t idx) const { return buckets[idx]; }
    size_t     bucket_count()              const { return num_buckets;  }
};