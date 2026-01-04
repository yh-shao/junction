#pragma once

#include "base.h"
// #include "LRU.h"
#include "LRUptr.h"
#include "blockpool.h"
#include "disk.h"

extern std::unique_ptr<BlockPool> block_pool;

struct BlockEntry
{
    BlockID     lba;
    char*       data;
    bool        valid;    // 是否从盘上读取了数据
    bool        dirty;    // 是否需要写回磁盘
    spinlock_t  mtx;

    // BlockEntry() = default;
    BlockEntry(BlockID lba) : lba(lba), data(block_pool->alloc_block()), dirty(false), valid(false) 
    {
        if (!data) 
        { 
            log_info("[create BlockEntry] ERROR: block pool exhausted");
            throw std::bad_alloc();
        }
        mtx.locked = 0;
        // log_info("BlockEntry created for lba %d", lba);
    }
};


using BlockCacheManager = ShardedLRUPtrSingleton<BlockID, BlockEntry>;


void init_block_cache(size_t capacity = DEFAULT_CACHE_SIZE);
// void read_block(BlockID lba, void* out_buf);
BlockEntry* read_block(BlockID lba);
void write_block(BlockID lba, const void* in_buf);
// void flush_dirty_blocks();

extern "C" {
    void flush_dirty_blocks();
}


class LockedBlockHandle    // 封装对 Cache Block 的访问，自动管理锁的生命周期
{   
    public:
        LockedBlockHandle(BlockEntry* block)  // block 必须非空
        {
            if (!block)
            {
                log_info("LockedBlockHandle: block is nullptr!");
                throw std::invalid_argument("LockedBlockHandle: block is nullptr!");
            }

            block_ = block;
            spin_lock(&block_->mtx);
        }
    
        ~LockedBlockHandle() 
        {
            spin_unlock(&block_->mtx);
        }
    
        // 禁止拷贝，只能移动
        LockedBlockHandle(const LockedBlockHandle&) = delete;
        LockedBlockHandle(LockedBlockHandle&& other) : block_(other.block_) { other.block_ = nullptr; }
    
        
        void ensure_data_valid()    // 核心逻辑：确保数据有效（如果 Cache Miss 则读盘）
        {
            if (block_->valid) return;
    
            readObj(block_->data, BLOCK_SIZE, block_->lba, 1);   
            block_->valid = true;
            block_->dirty = false;
        }
    
        char* data() const { return block_->data; }
    
    private:
        BlockEntry* block_;
    };