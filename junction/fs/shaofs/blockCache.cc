#include "fs.h"
#include "blockCache.h"
#include "utili.h"
#include "generic_cache/backend.h"
#include <cstring>

extern "C" {
#include "spdk/env.h"
}

static GlobalBlockCache* g_block_cache_ptr = nullptr;

GlobalBlockCache& get_block_cache()
{
    if (unlikely(g_block_cache_ptr == nullptr)) 
    {
        log_err("FATAL: get_block_cache() called BEFORE init_block_cache()!");
        init_block_cache();
    }
    return *g_block_cache_ptr;
}

BlockPool* blockpool_ptr;   // 创建一个 BlockPool，其中是若干个 4KB 的 SPDK DMA buffer
void init_block_cache(size_t capacity, size_t shard_num)
{
    if (g_block_cache_ptr != nullptr) 
    {
        log_warn("BlockCache already initialized! Ignoring this init call.");
        return;
    }

    log_info("init block cache ...");

    blockpool_ptr = new BlockPool(GlobalBlockCache::real_capacity(capacity, shard_num));

    size_t total_mem = GlobalBlockCache::CacheEntry_footprint(capacity, shard_num);
    char* buffer = new char[total_mem];   // CacheEntry 框架本身无需使用 DMA 内存
    if (unlikely(!buffer)) 
    { 
        log_err("[block_cache_init] ERROR: Failed to allocate metadata memory on heap!");
        buffer = nullptr;
    }
    else log_info("Heap allocated: base_addr=%p, size=%zu, using this buffer as BlockCache", buffer, total_mem);

    g_block_cache_ptr = new GlobalBlockCache(capacity, shard_num, &NVMeSSD::getInstance(), []() { return new LRUPolicy<BlockID, BlockData>(); }, buffer);
    log_info("block cache initialized with %zu shards, each shard has %zu capacity, total capacity is %zu", g_block_cache_ptr->get_shard_count(), g_block_cache_ptr->get_shard_capacity(), g_block_cache_ptr->get_total_capacity());
}

BlockHandle bc_get_handle(BlockID id) { return get_block_cache().getHandle(id);   }
bool bc_flush_block(BlockID id)       { return get_block_cache().flush_entry(id); }
void bc_invalidate_block(BlockID id)  {        get_block_cache().invalidate(id);  }
void bc_flush_all()                   {        get_block_cache().flush_all();     }