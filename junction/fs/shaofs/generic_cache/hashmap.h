#pragma once
#include <functional>
#include <cstddef>
#include <stdexcept>
#include <algorithm>

/* 本 hashmap 不含内置锁，并发安全由所在 shard 负责 */

/* 注意：EntryType 必须包含 KeyType key 和 EntryType* hash_next 两个成员变量 */
template <typename Key, typename EntryType, typename Hash = std::hash<Key>>
class IntrusiveCacheMap {
private:
    size_t num_buckets;
    EntryType** buckets;
    size_t current_size;   // hashmap 中当前存储的元素数量

    Hash hasher;
    bool external_memory;

    inline size_t get_bucket_idx(const Key& key) const { return hasher(key) % num_buckets; }

public:
    IntrusiveCacheMap(size_t buckets_count, void* buckets_addr = nullptr)  : num_buckets(buckets_count), current_size(0), external_memory(buckets_addr != nullptr) 
    {
        if (num_buckets < 1) throw std::invalid_argument("Bucket count must be greater than 0");

        if (external_memory)
        {
            buckets = static_cast<EntryType**>(buckets_addr);
            std::fill_n(buckets, num_buckets, nullptr);
        }
        else
        {
            buckets = new EntryType*[num_buckets]();
        } 
    }
    ~IntrusiveCacheMap() 
    {
        if (!external_memory) delete[] buckets;
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