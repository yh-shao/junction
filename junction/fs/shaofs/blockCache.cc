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

void init_block_cache(size_t capacity, size_t shard_num)
{
    if (g_block_cache_ptr != nullptr) 
    {
        log_warn("BlockCache already initialized! Ignoring this init call.");
        return;
    }

    log_info("init block cache ...");

    size_t total_mem = GlobalBlockCache::calculate_total_memory(capacity, shard_num);
    size_t align     = GlobalBlockCache::calculate_alignment();
    log_info("Allocating %zu Bytes (Alignment: %zu) from SPDK hugepages", total_mem, align);

    char* buffer = (char*)spdk_dma_zmalloc(total_mem, align, NULL);
    if (unlikely(!buffer)) 
    { 
        log_err("[block_cache_init] ERROR: spdk_dma_zmalloc failed!");
        buffer = nullptr;
    }
    else log_info("spdk_dma_zmalloc(): base_addr=%p, size=%zu, using this buffer as BlockCache", buffer, total_mem);

    // g_block_cache_ptr = new GlobalBlockCache(capacity, shard_num, &NVMeSSD::getInstance(), LRUPolicy<BlockID>::create_policy_instance, buffer);
    g_block_cache_ptr = new GlobalBlockCache(capacity, shard_num, &NVMeSSD::getInstance(), []() { return new LRUPolicy<BlockID, BlockData>(); }, buffer);
    log_info("block cache initialized with %zu shards, each shard has %zu capacity, total capacity is %zu", g_block_cache_ptr->get_shard_count(), g_block_cache_ptr->get_shard_capacity(), g_block_cache_ptr->get_total_capacity());
}

bool bc_read(BlockID id, void* buffer)
{
    auto& cache = get_block_cache();

    auto handle = cache.getHandle(id);    // 获取这个 CacheEntry 的 handle，这一步是 blocking 的（如果 Cache Miss，内部会自动调用 backend->read() 加载数据）
    if (!handle) 
    {
        log_err("bc_read failed: BlockID %lu (IO Error or Pool Exhausted)", id);
        return false;
    }

    // 数据拷贝 (持有锁期间)
    {
        // auto acc = handle.access(); 
        auto acc = handle.read_access();
        std::memcpy(buffer, acc->data, BLOCK_SIZE);  
    } 

    return true; // handle 析构：自动减引用计数
}
void bc_write(BlockID id, const void* buffer) { get_block_cache().put(id, *(BlockData*)buffer); }
void bc_flush_all() { get_block_cache().flush(); }
bool bc_flush_block(BlockID id) { return get_block_cache().flush_entry(id); }
BlockHandle bc_get_handle(BlockID id) { return get_block_cache().getHandle(id); }


void bc_prefetch(BlockID id)
{
    bc_get_handle(id); 
}