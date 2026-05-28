#pragma once
#include "fs.h"
#include "journal.h"
#include "profile.h"
#include "generic_cache/objpool.h"
#include "generic_cache/backend.h"
#include "generic_cache/replace_policy.h"
#include "generic_cache/sharded_cache.h"
extern "C" {
#include "runtime/storage.h"
#include "spdk/env.h"
}

class BlockPool {
public:
    struct Block { char data[BLOCK_SIZE]; };
private:
    void*                buffer_;  // 记录 SPDK 分配的底层大内存地址，用于析构时释放
    ObjectPool<Block>*   pool_;
    spinlock_t           lock_;

public:
    explicit BlockPool(size_t blocknum)
    {
        spin_lock_init(&lock_);

        buffer_ = spdk_dma_zmalloc(blocknum * BLOCK_SIZE, BLOCK_SIZE, NULL);
        if (buffer_) log_info("SPDK buffer allocated: base_addr=%p, size=%zu, using this buffer as BlockCache", buffer_, blocknum * BLOCK_SIZE);
        else throw std::bad_alloc(); // SPDK 内存分配失败
        
        pool_ = new ObjectPool<Block>(blocknum, buffer_);   // 初始化 ObjectPool，接管这片 buffer
    }
    ~BlockPool() 
    {
        if (pool_)   { delete pool_;           pool_   = nullptr; }
        if (buffer_) { spdk_dma_free(buffer_); buffer_ = nullptr; }
    }
    BlockPool(const BlockPool&)            = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    char* alloc() 
    {
        SpinGuardNP g(&lock_);
        return (char*)pool_->alloc();
    }
    void free(char* b) 
    {
        if (!b) return;
        SpinGuardNP g(&lock_);
        pool_->free((Block*)b);
    }
};

extern BlockPool* blockpool_ptr;

struct BlockData { 
    char* data; 
    BlockData()  { data = blockpool_ptr->alloc(); }
    ~BlockData() { blockpool_ptr->free(data);     }
    BlockData(const BlockData&)            = delete;
    BlockData& operator=(const BlockData&) = delete;
    BlockData(BlockData&&)                 = delete;
    BlockData& operator=(BlockData&&)      = delete;
};
bool bc_write_backend(BlockID id, const BlockData& value);

class MetadataAwareLRUPolicy : public ReplacementPolicy<BlockID, BlockData> {
    using EntryType = CacheEntry<BlockID, BlockData>;

private:
    struct list_head lru_list;
    static constexpr uint32_t kVictimScanLimit = 64;

    static inline EntryType* entry_from_node(struct list_node* node)
    {
        return list_entry(node, EntryType, policy_node);
    }

    static inline bool prefer_skip(EntryType* entry)
    {
#if CRASH_CONSISTENCY
        if (!atomic_read(&entry->dirty) || !atomic_read(&entry->valid)) return false;
        return true;
#else
        return atomic_read(&entry->dirty) && atomic_read(&entry->valid);
#endif
    }

public:
    MetadataAwareLRUPolicy()
    {
        list_head_init(&lru_list);
    }

    void touch(EntryType* entry) override
    {
        if (entry->policy_meta == 1)
        {
            if (lru_list.n.next == &entry->policy_node) return;
            list_del(&entry->policy_node);
        }
        list_add(&lru_list, &entry->policy_node);
        entry->policy_meta = 1;
    }

    void remove(EntryType* entry) override
    {
        if (entry->policy_meta == 1)
        {
            list_del(&entry->policy_node);
            entry->policy_meta = 0;
        }
    }

    EntryType* getVictim() override
    {
        if (list_empty(&lru_list)) return nullptr;

        EntryType* fallback = nullptr;
        struct list_node* node = lru_list.n.prev;
        uint32_t scanned = 0;
        while (node != &lru_list.n && scanned++ < kVictimScanLimit)
        {
            EntryType* entry = entry_from_node(node);
            if (!prefer_skip(entry)) return entry;
            if (fallback == nullptr) fallback = entry;
            node = node->prev;
        }
        return fallback ? fallback : entry_from_node(lru_list.n.prev);
    }
};

class NVMeSSD : public Backend<BlockID, BlockData> {
public:
    bool read(const BlockID& key, BlockData& value)
    {
        SHAOFS_PROFILE_SCOPE(SHAOFS_PROF_BC_BACKEND_READ);
        shaofs_profile_record_blocks(SHAOFS_PROF_BC_BACKEND_READ, 1);
        return DMA_read_block(static_cast<void*>(value.data), key);
    }
    bool write(const BlockID& key, BlockData const& value)
    {
        return bc_write_backend(key, value);
    }
    bool write_cleans_entry(const BlockID& key) const override
    {
#if CRASH_CONSISTENCY
        return journal_commit_returns_after_checkpoint(key);
#else
        return true;
#endif
    }
    static NVMeSSD& getInstance()   // singleton，全局只有 1 个 NVMeSSD 实例
    {
        static NVMeSSD instance;
        return instance;
    }
    NVMeSSD(const NVMeSSD&) = delete;
    void operator=(const NVMeSSD&) = delete;
private:
    NVMeSSD() = default;
};

#define DEFAULT_BLOCKCACHE_CAPACITY 65536    // 默认 BlockCache 容量（单位：块数），即 256MB（65536 * 4096 Bytes）。可以通过 init_block_cache() 的参数调整。
#define DEFAULT_SHARD_NUM           256      // 分片数量

using GlobalBlockCache = ShardedCache<BlockID, BlockData>;
GlobalBlockCache& get_block_cache();
void init_block_cache(size_t capacity = DEFAULT_BLOCKCACHE_CAPACITY, size_t shard_num = DEFAULT_SHARD_NUM);

using BlockHandle = GlobalBlockCache::Handle;
BlockHandle bc_get_handle(BlockID id);          // 获取 Block 的 handle，用于直接操作 BlockCache 中的数据，无需 memcpy（注意：通过 handle 修改数据后需要调用 handle.access().mark_dirty() 来标记脏数据）
BlockHandle bc_get_handle(BlockID id, bool fetch_on_miss);

bool bc_mark_block_dirty(BlockHandle& h);       // 标记 block 脏
bool bc_is_cached_valid(BlockID id);            // 只探测缓存中是否已有有效副本，不触发 miss 分配或后端读
void bc_flush_all();                            // 将 cache 中所有脏数据刷回后端存储
bool bc_flush_block(BlockID id);                // 将指定 block 刷写到后端（如果在缓存中且为脏）
bool bc_flush_block_batched(BlockID id);        // fsync 前台路径使用：metadata block 进入 journal batch lane
bool bc_flush_blocks_contiguous(BlockID start, uint32_t count);  // 批量刷写连续 block 中已经缓存且为脏的条目
void bc_invalidate_block(BlockID id);           // 使指定 block 的 cache entry 失效（不写回）
