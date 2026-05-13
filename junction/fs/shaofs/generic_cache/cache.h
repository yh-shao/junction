#pragma once
#include "backend.h"
#include "replace_policy.h"
#include "objpool.h"
#include "cache_entry.h"
#include "hashmap.h"
#include <vector>


template <typename Key, typename Value, typename Hash = std::hash<Key>>
class Cache {
public:
    using EntryType = CacheEntry<Key, Value>;
    using Handle    = CacheEntryHandle<Key, Value>;

private:
    Backend<Key, Value>*            backend;    // 后端数据库
    ReplacementPolicy<Key, Value>*  policy;     // 用于记录 victim CacheEntry 是哪个
    ObjectPool<EntryType>*          entryPool;  // 用于分配 CacheEntry

    size_t capacity;
    IntrusiveCacheMap<Key, EntryType, Hash>* hashmap;
    
    spinlock_t shard_lock;    // 全局锁，保护 hashmap、LRU 链表、objpool（因此在这个 cache 框架中使用的 hashmap、LRU 链表、objpool 子模块都无需自带锁）

    uint32_t touch_counter = 0;                       // 概率化计数器
    static constexpr uint32_t TOUCH_SAMPLE = 16;      // 每 16 次 hit 才 touch 一次 policy（必须是 2 的幂，从而可以使用位运算优化）

    // 调用 evict_locked() 前必须持有全局锁，从而没有其它线程可以再访问 Hashmap、LRU 链表，中间可能会临时释放全局锁，函数返回时仍持有全局锁。返回值表示是否成功腾出了一个 CacheEntry
    bool evict_locked()   //  驱逐一个 cache entry，并写回后端（如果 dirty）
    {
        size_t max_attempts = hashmap->size();
        for (size_t i = 0; i < max_attempts; i++)
        {
            EntryType* victim = policy->getVictim();
            if (victim == nullptr) 
            {
                log_err("Eviction failed: no victim found");
                return false;   // 没有可驱逐的对象，触发调用方降级
            }

            if (atomic_read(&victim->ref_count) > 0)   // 如果 victim 正在被使用（ref_count > 0），则放弃这个 victim，继续寻找下一个 victim
            {
                policy->touch(victim);
                continue;
            }

            // 获取到一个可以驱逐的 CacheEntry，当前这个 CachEntry 的 refcnt 为 0 （且肯定没有线程在使用这个 CacheEntry）
            atomic_inc(&victim->ref_count);       // Pin 住该 CacheEntry （当前正在访问该 CacheEntry） 将不会被再次选中 evict

            rwmutex_rdlock(&victim->rw_mtx);    // 拿”读”锁：阻止其它线程写入该 CacheEntry，但允许其它线程并发读取，提高并发性
            spin_unlock(&shard_lock);           // 释放全局锁，允许其它线程访问 Hashmap，此时其它线程可能会访问到这个 victim cache entry

            bool write_success = true;
            if (atomic_read(&victim->dirty) && atomic_read(&victim->valid)) write_success = backend->write(victim->key, victim->data);
            if (write_success) atomic_write(&victim->dirty, 0);   // 此时该 CacheEntry 中的数据已经与后端一致
            rwmutex_unlock(&victim->rw_mtx);

            spin_lock(&shard_lock); // 重新获取全局大锁，保证没有新线程能从 hashmap 中查找到这个 cache entry
            if (write_success && atomic_read(&victim->ref_count) == 1 && atomic_read(&victim->dirty) == 0)  // 该 CacheEntry 此时没有被访问 且在释放全局锁期间没有被修改
            {
                hashmap->remove(victim->key);
                policy->remove(victim);
                entryPool->free(victim);        // 释放掉这个 cache entry，可被后续再分配使用（之前设置的 ref_count 也没用了）
                return true; // 成功腾出 1 个空间
            }
            else   // 二次检查：如果在释放全局锁期间，有其它线程访问/修改了这个 victim，则放弃驱逐
            {
                atomic_dec(&victim->ref_count);
                policy->touch(victim);
                continue; // 腾空间失败，继续寻找下一个 victim!
            }
        }

        return false;  // 触发调用方降级处理
    }

    // 调用前必须持有全局锁，函数返回时仍持有全局锁
    Handle find_or_allocate_locked(const Key& key)    // 查找是否 CacheEntry 已经存在；如果不存在，则分配一个新的 CacheEntry 并插入 Hashmap。返回的 CacheEntry 会被 Pin 住（ref_count 至少为 1）
    {
        EntryType* entry = hashmap->find(key);
        if (entry) 
        {
            if ((++touch_counter & (TOUCH_SAMPLE - 1)) == 0) policy->touch(entry);   // 减少 policy 更新开销
            return Handle(entry);
        }

        // Cache Miss: 检查容量并尝试驱逐
        if (hashmap->size() >= capacity)   
        {
            if (!evict_locked())   // 容量已满且驱逐失败（所有块都被 Pin 住了），触发调用方降级处理
            {
                // log_err("Allocation failed: capacity full and eviction failed");
                return Handle(nullptr);
            }

            // 二次检查：evict_locked() 内部临时释放过 shard_lock，其它线程可能已为相同 key 插入了条目
            entry = hashmap->find(key);
            if (entry)
            {
                policy->touch(entry);
                return Handle(entry);
            }
        }

        // 此处仍持有全局锁且容量充足，安全地分配新的 CacheEntry 并插入 Hashmap
        entry = entryPool->alloc(); // 分配新的 CacheEntry
        if (!entry) return Handle(nullptr); // 对象池耗尽
        entry->reset(key);
        hashmap->insert(entry);
        policy->touch(entry);
        return Handle(entry);
    }

public:
    Cache(size_t cap, Backend<Key, Value>* backend_ptr, ReplacementPolicy<Key, Value>* policy_ptr, void* pool_addr = nullptr) : capacity(cap), backend(backend_ptr), policy(policy_ptr)
    { 
        spin_lock_init(&shard_lock);
        hashmap   = new IntrusiveCacheMap<Key, EntryType, Hash>(capacity * 2);  // 根据 Cache 的容量（CacheEntry 的数目）来决定 hashmap bucket 的数目（降低负载因子）
        entryPool = new ObjectPool<EntryType>(capacity, pool_addr);             // 如果传入了 pool_addr，ObjectPool 将在给定的内存上构建对象；否则，它会在堆上自动 new 内存
    }
    ~Cache() 
    {
        flush_all();
        delete hashmap; 
        delete entryPool; 
    }


    // 独占访问：返回 Handle，调用者可通过 read_access()/write_access() 持锁操作。失败返回 Handle(nullptr)，由调用者自行 fallback
    Handle getHandle(const Key& key, bool fetch_on_miss)   
    {
        spin_lock(&shard_lock);
        Handle entryHandle = find_or_allocate_locked(key);
        spin_unlock(&shard_lock);

        if (!entryHandle) return Handle(nullptr); // 无法获取该 key 对应的 CacheEntry，触发调用方降级处理

        // 此时已经成功获取到一个 CacheEntry（可能是命中也可能是新分配的）
        EntryType* entry = entryHandle.get_entry();

        // Fast Path: 数据有效或者不需要从后端读取，则直接返回该 CacheEntry，调用者后续通过 read_access()/write_access() 按需加锁
        if (atomic_read(&entry->valid) || !fetch_on_miss) return entryHandle;   

        // Slow path: entry 尚未 valid，需要从后端加载数据
        {
            auto acc = entryHandle.write_access(); // 获取写访问器（尝试获取写锁）
            if (atomic_read(&entry->valid)) return entryHandle;  // 获取到写锁后，必须再次检查 valid。因为在你等待写锁的期间，可能有其他线程已经完成了读盘操作！

            // 此时确信我是唯一持有写锁，且数据无效的线程。开始发起后端 I/O。
            bool success = backend->read(key, entry->data); // 从后端读取数据到 CacheEntry 中
            if (success) 
            {
                atomic_write(&entry->dirty, 0); 
                atomic_write(&entry->valid, 1);
                return entryHandle;
            }
        }  // 释放写锁，那些阻塞在第一次/第二次检查的线程会被立即唤醒
    
        return Handle(nullptr);
    }

    // 简单读取：拷贝 value 后立即释放，分配失败时自动 fallback 到后端直读
    bool get(const Key& key, Value& value)
    {
        Handle entryHandle = getHandle(key);
        if (!entryHandle) return backend->read(key, value);   // 降级路径：如果 Cache 无法分配空间（全被 Pin 住）或后端加载彻底失败
        value = *(entryHandle.read_access());
        return true;
    }

    // 尝试将数据写入到 cache 中，若失败则 fallback 为直接写入到 backend
    bool put(const Key& key, const Value& value) 
    {
        Handle entryHandle = getHandle(key, false);           // 直接覆盖
        if (!entryHandle) return backend->write(key, value);  // 降级路径：无法获取 CacheEntry 时，直接写后端

        {
            auto acc = entryHandle.write_access();
            *acc = value;
            acc.mark_dirty();
            atomic_write(&entryHandle.get_entry()->valid, 1);
        }
        return true;
    }

    bool flush_entry(const Key& key)   // 将指定 key 的 CacheEntry 刷写到后端（如果在缓存中且为脏）
    {
        spin_lock(&shard_lock);
        Handle entryHandle(hashmap->find(key));  // Pin 住，防止被驱逐
        spin_unlock(&shard_lock);

        if (!entryHandle) return true;   // 不在缓存中，无需刷写

        EntryType* entry = entryHandle.get_entry();
        if (!atomic_read(&entry->dirty) || !atomic_read(&entry->valid)) return true;   // fast path: 不脏或者无效，无需刷写，直接返回成功，避免对读锁的获取  （不过在极端情况下可能存在并发问题，此处暂时可以接受）

        auto acc = entryHandle.read_access();   // 读锁即可：只是读数据写到后端，不修改 entry 的 data
        if (atomic_read(&entry->dirty) && atomic_read(&entry->valid))
        {
            if (backend->write(entry->key, entry->data)) atomic_write(&entry->dirty, 0);
            else return false;
        }
        return true;
    }
    
    void flush_all()
    {
        std::vector<Handle> entries;
        entries.reserve(capacity);

        spin_lock(&shard_lock);
        for (size_t i = 0; i < hashmap->bucket_count(); i++)
        {
            for (EntryType* curr = hashmap->get_bucket_head(i); curr; curr = curr->hash_next) 
                if (atomic_read(&curr->dirty) && atomic_read(&curr->valid)) entries.emplace_back(curr);
        }
        spin_unlock(&shard_lock);

        for (Handle& h : entries)
        {
            EntryType* entry = h.get_entry();
            if (!atomic_read(&entry->dirty) || !atomic_read(&entry->valid)) continue;

            auto acc = h.read_access();
            if (atomic_read(&entry->dirty) && atomic_read(&entry->valid))
            {
                if (backend->write(entry->key, entry->data)) atomic_write(&entry->dirty, 0);
                else log_err("Flush failed for a key");
            }
        }
    }

    void invalidate(const Key& key)   // 主动使指定 key 的 CachEntry 失效（不写回后端）
    {
        spin_lock(&shard_lock);
        Handle entryHandle = Handle(hashmap->find(key));  // 使用 Handle 接管，内部会自动执行 atomic_inc(&ref_count)
        spin_unlock(&shard_lock);

        if (!entryHandle) return;  // Cache Miss: 本身就不在缓存中，直接返回
        EntryType* victim = entryHandle.get_entry();

        {
            auto acc = entryHandle.write_access();  // 阻塞等待当前所有正在持有 ReadAccessor/WriteAccessor 的线程完成操作
            atomic_write(&victim->dirty, 0);
            atomic_write(&victim->valid, 0);  // 强制清空脏标记并设为无效。这样不仅不会写回后端，而且等待在锁上的其他 get() 线程被唤醒后，会发现 valid == 0，从而乖乖去后端拉取最新数据。
        }

        // 尝试将其从 Hashmap 和内存池中物理回收，以节省空间
        spin_lock(&shard_lock);
        if (atomic_read(&victim->ref_count) == 1 && atomic_read(&victim->valid) == 0)  // 二次检查：确保当前没有其它线程在使用它，且在我们释放读写锁到重新获取全局锁的间隙，没有其他线程又去后端拉取了数据让它 valid
        {
            hashmap->remove(victim->key);
            policy->remove(victim);
            entryHandle = Handle();  // 析构掉之前的 entryHandle  （不能在 entryPool->free(victim) 之后再调用 atomic_dec(&ref_count)）
            entryPool->free(victim); // 直接释放回对象池，被 free 的对象无需再 dec ref_count
        } 
        spin_unlock(&shard_lock);
    }

    static size_t CacheEntry_footprint(size_t capacity) { return capacity * sizeof(EntryType); }
};