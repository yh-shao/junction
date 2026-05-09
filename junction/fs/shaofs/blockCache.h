#pragma once
#include "fs.h"
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
        SpinGuard g(&lock_);
        return (char*)pool_->alloc();
    }
    void free(char* b) 
    {
        if (!b) return;
        SpinGuard g(&lock_);
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

class NVMeSSD : public Backend<BlockID, BlockData> {
public:
    bool read(const BlockID& key, BlockData& value)
    {
        // int ret = storage_read(static_cast<void*>(value.data), key, 1);
        // return (ret == 0);
        return DMA_read_block(static_cast<void*>(value.data), key);
    }
    bool write(const BlockID& key, BlockData const& value)
    {
        // int ret = storage_write(static_cast<const void*>(value.data), key, 1);
        // log_info("write to block %lu", key);                                                                    
        // return (ret == 0);
        return DMA_write_block(static_cast<const void*>(value.data), key);
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

void bc_flush_all();                            // 将 cache 中所有脏数据刷回后端存储
bool bc_flush_block(BlockID id);                // 将指定 block 刷写到后端（如果在缓存中且为脏）
void bc_invalidate_block(BlockID id);           // 使指定 block 的 cache entry 失效（不写回）