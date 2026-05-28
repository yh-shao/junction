#pragma once

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif

template <typename Key, typename Value>
struct alignas(CACHELINE_SIZE) CacheEntry {
    void* _freelist_hook;      // 牺牲位：专门给 ObjectPool 存放 next 指针用，这样 ObjectPool 覆写前 8 字节时，只会覆盖这个无用的指针，不会破坏其它内容
    CacheEntry* hash_next;     // 供 CacheMap 使用的侵入式哈希链表指针

    struct list_node  policy_node;  // 供策略使用的链表节点（如果策略需要的话）
    uint64_t          policy_meta;  // 不透明的策略元数据 (例如: LFU 的 freq, CLOCK 的 ref_bit 等)

    Key   key;
    Value data;                // 这里直接存放 Value 对象，而不是指针
    
    alignas(CACHELINE_SIZE) rwmutex_t rw_mtx;          // 读写锁，保护这个 CachEntry 的读写

    volatile int valid;       // 该 CacheEntry 中的 data 是否有效（能否被 get() 返回给用户使用）
    volatile int dirty;       // 表明是否需要写回 backend
    volatile int ref_count;   // 引用计数（Pin），表示当前有多少个线程正在访问该 CacheEntry 的数据（无论读写）。ref_count > 0 表示该 CacheEntry 正在被使用，不能被驱逐。
    volatile uint64_t dirty_gen;
    volatile int writeback_queued;

    CacheEntry() : _freelist_hook(nullptr), hash_next(nullptr), policy_meta(0), valid(false), dirty(false), ref_count(0), dirty_gen(0), writeback_queued(0) { rwmutex_init(&rw_mtx); }

    void reset(const Key& k) 
    {
        key         = k;
        hash_next   = nullptr;
        policy_meta = 0;
        atomic_write(&valid, 0);
        atomic_write(&dirty, 0);
        atomic_write(&ref_count, 0);
        __atomic_store_n(&dirty_gen, 0, __ATOMIC_SEQ_CST);
        atomic_write(&writeback_queued, 0);
    }
};


// 用户通过 Handle 访问 CacheEntry，Handle 内部通过读写访问器提供对 CacheEntry 数据的访问。
// 在 Handle 的生命周期内，保证 CacheEntry 不会被驱逐（因为 ref_count 至少为 1）。
template <typename Key, typename Value>
class CacheEntryHandle {                         
    using EntryType = CacheEntry<Key, Value>;

private:
    EntryType* entry = nullptr;

    void acquire(EntryType* e) 
    {
        entry = e;
        if (entry) atomic_inc(&entry->ref_count);  // Pin 住这个 Entry，表示当前正在访问它
    }
    void release() 
    {
        if (entry) 
        {
            atomic_dec(&entry->ref_count);
            entry = nullptr;
        }
    }

public:
    CacheEntryHandle() = default;
    explicit CacheEntryHandle(EntryType* e) { acquire(e); }                                              // 构造时获取引用
    CacheEntryHandle(const CacheEntryHandle& other) { acquire(other.entry); }                            // 拷贝构造：需要增加引用计数
    CacheEntryHandle(CacheEntryHandle&& other) noexcept : entry(other.entry) { other.entry = nullptr; }  // 移动构造：转移所有权，不改变引用计数
    ~CacheEntryHandle() { release(); }                                                                   // 析构时释放引用
    CacheEntryHandle& operator=(const CacheEntryHandle& other)    // 拷贝赋值运算符
    {
        if (this != &other)        // 防止自赋值
        { 
            release();             // 先释放当前的引用
            acquire(other.entry);  // 接管新的引用
        }
        return *this;
    }
    CacheEntryHandle& operator=(CacheEntryHandle&& other) noexcept  // 移动赋值运算符
    {
        if (this != &other) // 防止自赋值
        {
            release();                   // 释放当前的引用
            entry = other.entry;         // 直接接管别人的资源
            other.entry = nullptr;       // 剥夺别人的资源
        }
        return *this;
    }
    operator bool() const { return entry != nullptr; }   // 判断空指针，供 if (handle) 使用
    EntryType* get_entry() const { return entry; }


    class ReadAccessor {   // 只读访问器：允许多个线程并发拷贝数据
        EntryType* entry;
    public:
        ReadAccessor(EntryType* e) : entry(e) 
        { 
            if (entry) rwmutex_rdlock(&entry->rw_mtx);    // 获取读锁
        }
        ~ReadAccessor() 
        { 
            if (entry) rwmutex_unlock(&entry->rw_mtx);     // 释放锁
        }
        ReadAccessor(const ReadAccessor&) = delete;    
        ReadAccessor& operator=(const ReadAccessor&) = delete;
        ReadAccessor(ReadAccessor&& other) noexcept : entry(other.entry) { other.entry = nullptr; }

        const Value* operator->() const { return &entry->data; }
        const Value& operator*()  const { return  entry->data; }
    };
    ReadAccessor read_access() const { return ReadAccessor(entry); }
    

    class WriteAccessor {   // 独占写访问器：排他性修改数据
        EntryType* entry;
    public:
        WriteAccessor(EntryType* e) : entry(e) 
        { 
            if (entry) rwmutex_wrlock(&entry->rw_mtx); // 获取写锁
        }
        ~WriteAccessor() 
        { 
            if (entry) rwmutex_unlock(&entry->rw_mtx); // 释放锁
        }
        WriteAccessor(const WriteAccessor&) = delete;    
        WriteAccessor& operator=(const WriteAccessor&) = delete;
        WriteAccessor(WriteAccessor&& other) noexcept : entry(other.entry) { other.entry = nullptr; }

        Value* operator->() { return &entry->data; }
        Value& operator*()  { return  entry->data; }

        void mark_dirty() { if (entry) atomic_write(&entry->dirty, 1); }
    };
    WriteAccessor write_access() const { return WriteAccessor(entry); }
};
