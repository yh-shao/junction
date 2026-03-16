#pragma once

#include <iostream>
#include <unordered_map>
#include "backend.h"
#include "replace_policy.h"
#include "utili.h"
#include "objpool.h"

/**
 * @brief A generic Cache framework implementing Read-Through, Write-Back and Write-Allocate strategies.
 * * Uses fine-grained locking (Map Lock vs. Entry Lock) and Reference Counting to manage concurrency.
 */
template <typename Key, typename Value>
class Cache {
private:
    Backend<Key, Value>*    backend;
    ReplacementPolicy<Key>* policy;

    struct CacheEntry {
        void* _freelist_hook;   // 牺牲位：专门给 ObjectPool 存放 next 指针用，这样 ObjectPool 覆写前 8 字节时，只会覆盖这个无用的指针，不会破坏 key。

        Key key;
        Value data;     // 这里直接存放 Value 对象，而不是指针
        
        bool valid; 
        bool dirty;     // Dirty Bit: Indicates if data differs from the Backend (for Write-Back).

        spinlock_t mtx; // Entry-level Mutex  （TODO: 修改成读写锁）

        // Atomic variables (using volatile int with GCC built-ins)
        volatile int ref_count;  // Number of threads currently holding a pointer to this entry.
        volatile int loading;    // 1 indicates data is currently being fetched from backend.
        volatile int evicting;   // 1 indicates data is currently being pushed to backend.

        CacheEntry()      : _freelist_hook(nullptr), valid(false), dirty(false), ref_count(0), loading(0), evicting(0) { spin_lock_init(&mtx); }
        CacheEntry(Key k) : key(k), valid(false), dirty(false), ref_count(0), loading(0), evicting(0) { spin_lock_init(&mtx); }

        void reset(const Key& k) 
        {
            key   = k;       
            valid = false;  // data 保持原样即可，valid=false 会阻止读取脏数据
            dirty = false;
            atomic_write(&ref_count, 0);
            atomic_write(&loading, 0);
            atomic_write(&evicting, 0);
        }
    };
    ObjectPool<CacheEntry>* entryPool;
    static constexpr size_t POOL_PADDING = 100;  // （处于安全考虑）处理高并发下的超额请求

    size_t capacity;
    std::unordered_map<Key, CacheEntry*> cacheMap;    // （TODO: 修改成细粒度的给每个“桶”加锁，需要手写 hash map）
    mutable spinlock_t map_mutex;   // Global Map Mutex: Protects the structure of cacheMap

    bool evict()  // 删除一个CacheEntry 并将其中的内容写到后端。执行该函数前需要需要在持有 map_mutex 锁，执行完毕后也应持有该锁。
    {
        size_t max_attempts = cacheMap.size();
        size_t attempts = 0;

        while (attempts++ < max_attempts) 
        {
            Key victimKey = policy->getVictim();   // Identify victim via policy.
            
            auto it = cacheMap.find(victimKey);
            if (it == cacheMap.end()) // Consistency Check: Policy layer and Map layer are out of sync, or Policy is empty.
            {
                log_err("[FATAL ERROR] Hash map layer don't match Policy layer, or CacheSize=0");
                return false;
            }

            CacheEntry* victimEntry = it->second;
            if (atomic_read(&victimEntry->ref_count) > 0 || atomic_read(&victimEntry->evicting) > 0)   // If ref_count > 0, other threads are reading/writing this entry. It cannot be safely evicted yet.
            {
                policy->touch(victimKey);  // trick

                // Back-off Strategy: Release the global lock and yield CPU to allow active threads to complete. Then re-acquire lock and retry the eviction loop.
                spin_unlock(&map_mutex);
                thread_yield();
                spin_lock(&map_mutex);
                
                continue;
            }

            atomic_write(&victimEntry->evicting, 1);
            spin_unlock(&map_mutex);   // Release map_mutex before performing IO. We don't want to block the entire cache lookup while writing a CacheEntry to disk.

            bool write_success = true;
            if (victimEntry->dirty) 
            {
                // std::cout << "[Cache] Evicting dirty block: " << victimKey << ". Writing back.\n";
                write_success = backend->write(victimKey, victimEntry->data);
                if (!write_success)
                {
                    log_err("[Error] Failed to write-back dirty block: %lu", victimKey);
                }
            }

            spin_lock(&map_mutex);         // Re-acquire lock to maintain caller's context (caller expects lock held).

            if (!write_success) 
            {
                atomic_write(&victimEntry->evicting, 0);
                return false; 
            }

            cacheMap.erase(it);
            policy->remove(victimKey);
            entryPool->free(victimEntry);

            return true;
        }

        log_warn("Cache highly congested, all entries pinned. Eviction downgraded.");
        return false;
    }

    CacheEntry* _find_or_allocate(const Key& key)   // 找到该 key 对应的 CacheEntry 或者为该 key 分配一个 CacheEntry
    {
        spin_lock(&map_mutex);         // Holds Map Lock （TODO：改为读写锁）
        CacheEntry* entry = nullptr;

        while (true)
        {
            auto it = cacheMap.find(key);
            if (it != cacheMap.end())  // Cache Hit
            {
                entry = it->second;
                if (atomic_read(&entry->loading) || atomic_read(&entry->evicting)) 
                {
                    spin_unlock(&map_mutex);
                    thread_yield();       
                    spin_lock(&map_mutex);
                    continue; 
                }

                break;
            }
            else   // Cache Miss
            {
                if (cacheMap.size() >= capacity)  // If capacity is reached, attempt eviction first. (evict() handles internal lock release/re-acquire).
                {
                    if (!evict()) 
                    {
                        spin_unlock(&map_mutex);
                        return nullptr; // 驱逐失败，返回 nullptr 触发降级
                    }
                    continue;
                }

                entry = entryPool->alloc();
                if (!entry) 
                {
                    spin_unlock(&map_mutex);
                    return nullptr;
                }
                entry->reset(key);
                cacheMap[key] = entry;
            }
        }
        
        atomic_inc(&entry->ref_count);   // Atomically increment reference count to "Pin" the entry in memory. This ensures it won't be evicted while we are using it outside the map lock.
        policy->touch(key);              // Update the policy data structure while holding global Map Lock.
        spin_unlock(&map_mutex);
        return entry;
    }

public:
    Cache(size_t cap, Backend<Key, Value>* backend_ptr, ReplacementPolicy<Key>* policy_ptr, void* pool_addr = nullptr) : capacity(cap), backend(backend_ptr), policy(policy_ptr) 
    { 
        spin_lock_init(&map_mutex); 
        entryPool = new ObjectPool<CacheEntry>(capacity + POOL_PADDING, pool_addr);  // 如果传入了 pool_addr，ObjectPool 将在给定的内存上构建对象；否则，它会在堆上自动 new 内存
    }
    ~Cache() { delete entryPool; }

    static size_t calculate_memory_size(size_t capacity) 
    {
        size_t total_count = capacity + POOL_PADDING;
        return total_count * sizeof(CacheEntry);
    }
    static size_t calculate_alignment() { return alignof(CacheEntry); }

    class Handle {      // 用户通过 Handle 访问数据，Handle 析构时自动释放引用计数
    private:
        CacheEntry* entry = nullptr;
        friend class Cache;   // 允许 Cache 类访问 Handle 的私有成员（如 entry）

        void acquire(CacheEntry* e) 
        {
            entry = e;
            if (entry) atomic_inc(&entry->ref_count);
        }
        void release() 
        {
            if (entry) { atomic_dec(&entry->ref_count); entry = nullptr; }
        }

    public:
        Handle() = default;
        explicit Handle(CacheEntry* e) : entry(e) {}           // 接管 Entry，注意此时 Entry 的 ref_count 已经被 Cache::get 增加过了
        Handle(const Handle& other) { acquire(other.entry); }  // 拷贝构造：需要增加引用计数
        Handle(Handle&& other) noexcept : entry(other.entry) { other.entry = nullptr; }  // 移动构造：转移所有权，不改变引用计数
        ~Handle() { release(); }   // 析构函数：释放引用

        Handle& operator=(const Handle& other)   // 拷贝赋值运算符
        {
            if (this != &other) // 防止自赋值
            {
                release();                // 先释放当前的引用
                acquire(other.entry);     // 接管新的引用
            }
            return *this;
        }
        Handle& operator=(Handle&& other) noexcept  // 移动赋值运算符
        {
            if (this != &other) // 防止自赋值
            {
                release();                   // 释放当前的引用
                entry = other.entry;         // 窃取别人的引用
                other.entry = nullptr;
            }
            return *this;
        }

        operator bool() const { return entry != nullptr; }   // 判断空指针，供 if (handle) 使用

        class Accessor {
            CacheEntry* entry;
        public:
            Accessor(CacheEntry* e) : entry(e)
            {
                if (entry) 
                {
                    spin_lock(&entry->mtx);         // 构造时自动加锁
                    atomic_inc(&entry->ref_count);
                }
            }
            ~Accessor() 
            {
                if (entry) 
                {
                    spin_unlock(&entry->mtx);       // 析构时自动解锁
                    atomic_dec(&entry->ref_count);
                }
            }

            Accessor(const Accessor&) = delete;    // 禁止拷贝
            Accessor& operator=(const Accessor&) = delete;
            Accessor(Accessor&& other) noexcept : entry(other.entry) { other.entry = nullptr; }

            // 只有通过 Accessor 才能拿到数据指针
            Value* operator->() { return &entry->data; }
            Value& operator*()  { return  entry->data; }
            
            void mark_dirty() 
            {
                if (entry) entry->dirty = true;
            }
        };

        Accessor access() { return Accessor(entry); }
    };
    void mark_dirty(const Handle& h) 
    {
        if (h.entry) 
        {
            CacheEntry* e = h.entry;
            SpinGuard g(&e->mtx);
            e->dirty = true;
        }
    }

    Handle get(const Key& key)   // 返回一个 Handle 对象，Handle 析构前，数据保证在内存中且有效。
    {
        // --- Phase 1: Lookup & Reference Acquisition (Holds Map Lock) ---
        CacheEntry* entry = _find_or_allocate(key);
        if (!entry) 
        {
            log_err("[Cache] Failed to get entry for key: %lu", key);
            return Handle(nullptr);
        }

        // --- Phase 2: Data Access / Loading (Holds Entry Lock, No Map Lock) ---
        // Since we hold ref_count, the 'entry' pointer is safe to use (won't be evicted).

        spin_lock(&entry->mtx);
        
        while (atomic_read(&entry->loading)) 
        {
            spin_unlock(&entry->mtx);
            thread_yield();       
            spin_lock(&entry->mtx);
        }

        if (entry->valid) 
        {
            spin_unlock(&entry->mtx);
            return Handle(entry);
        }

        atomic_write(&entry->loading, 1);
        spin_unlock(&entry->mtx);

        bool success = backend->read(key, entry->data);

        spin_lock(&entry->mtx);
        atomic_write(&entry->loading, 0);

        if (success) 
        {
            entry->valid = true;
            entry->dirty = false;
        } 
        else 
        {
            // 读取失败，保持 invalid，允许后续请求重试
            log_err("Failed to load key: %lu", key);
        }

        spin_unlock(&entry->mtx);
        return Handle(entry);
    }

    bool put(const Key& key, const Value& val)
    {
        // --- Phase 1: Lookup & Reference Acquisition (Holds Map Lock) ---
        CacheEntry* entry = _find_or_allocate(key);
        if (!entry)   // Cache 容量耗尽且所有的块都被 Pin 死 (ref_count > 0)
        {
            log_warn("[Cache] failed to allocate for key: %lu, write to backend directly", key);
            return backend->write(key, val);  // 透传到后端，绕过缓存（降级方案）
        }

        // --- Phase 2: Modification (Holds Entry Lock) ---
        spin_lock(&entry->mtx);  // If it was a hit, we need to acquire the lock now; If it was a miss (new_entry), we already locked it inside the map lock.
        while (atomic_read(&entry->loading))     // 即使是 put，也必须等待 loading 结束
        {
            spin_unlock(&entry->mtx);
            thread_yield();
            spin_lock(&entry->mtx);
        }
        entry->data  = val;
        entry->dirty = true;  // Mark dirty (Write-Back strategy).
        entry->valid = true;  // data is valid
        // std::cout << "[Cache] Written to cache (dirty): " << key << "\n";
        spin_unlock(&entry->mtx);

        atomic_dec(&entry->ref_count);   // Release reference.
        return true;
    }
    
    void flush()  // Manually flush all dirty blocks to the backend. This locks the entire map for the duration of the scan.
    {
        SpinGuard g(&map_mutex);

        for (auto& pair : cacheMap) 
        {
            CacheEntry* entry = pair.second;
            SpinGuard g2(&entry->mtx);
            if (entry->dirty) 
            {
                if (backend->write(pair.first, entry->data)) 
                {
                    entry->dirty = false;
                } 
                else 
                {
                    log_err("[Cache Error] Flush failed for key: %lu", pair.first);
                    // Keep dirty==true on failure so it is possible to retry later.
                }
            }
        }
    }
};