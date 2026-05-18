#include "fs.h"
#include "blockCache.h"
#include "journal.h"
#include "utili.h"
#include "generic_cache/backend.h"
#include <cstring>
#include <utility>

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

bool bc_flush_blocks_contiguous(BlockID start, uint32_t count)
{
    static constexpr uint32_t kMaxBatch = 16;
    struct StorageBlockEntryCompat { uint64_t lba; char* data; };

    BlockHandle handles[kMaxBatch];
    CacheEntry<BlockID, BlockData>* entries[kMaxBatch];
    StorageBlockEntryCompat sgl_entries[kMaxBatch];
    void* sgl_ptrs[kMaxBatch];

    uint32_t nr = 0;
    BlockID run_start = INVALID_BLOCK_ID;

    auto reset_run = [&]() {
        for (uint32_t i = 0; i < nr; i++) handles[i] = BlockHandle();
        nr = 0;
        run_start = INVALID_BLOCK_ID;
    };

    auto flush_run = [&]() -> bool {
        if (nr == 0) return true;

        for (uint32_t i = 0; i < nr; i++)
            rwmutex_rdlock(&entries[i]->rw_mtx);

        bool all_valid = true;
        bool any_dirty = false;
        for (uint32_t i = 0; i < nr; i++)
        {
            all_valid &= atomic_read(&entries[i]->valid);
            any_dirty |= atomic_read(&entries[i]->dirty);
        }

        bool ok = true;
        if (all_valid && any_dirty)
        {
            for (uint32_t i = 0; i < nr; i++)
            {
                sgl_entries[i] = {run_start + i, entries[i]->data.data};
                sgl_ptrs[i] = &sgl_entries[i];
            }

            ok = nr == 1 ? (storage_write(entries[0]->data.data, run_start, 1) == 0) : (write_blocks_to_disk(run_start, nr, sgl_ptrs) == 0);
            if (ok)
            {
                for (uint32_t i = 0; i < nr; i++)
                    atomic_write(&entries[i]->dirty, 0);
            }
        }

        for (uint32_t i = nr; i > 0; i--)
            rwmutex_unlock(&entries[i - 1]->rw_mtx);

        if (!ok) return false;
        reset_run();
        return true;
    };

    for (uint32_t i = 0; i < count; i++)
    {
        BlockID id = start + i;

#if CRASH_CONSISTENCY
        if (journal_is_metadata_block(id))
        {
            if (!flush_run() || !bc_flush_block(id)) return false;
            continue;
        }
#endif

        BlockHandle h = get_block_cache().find_cached(id);
        if (!h)
        {
            if (!flush_run()) return false;
            continue;
        }

        auto* entry = h.get_entry();
        if (!atomic_read(&entry->dirty) || !atomic_read(&entry->valid))
        {
            if (!flush_run()) return false;
            continue;
        }

        if (nr == 0) run_start = id;
        handles[nr] = std::move(h);
        entries[nr] = entry;
        nr++;

        if (nr == kMaxBatch && !flush_run()) return false;
    }

    return flush_run();
}

bool bc_write_backend(BlockID id, const BlockData& value)
{
#if CRASH_CONSISTENCY
    if (journal_is_metadata_block(id)) return journal_commit_single(id, value.data);
#endif
    return DMA_write_block(static_cast<const void*>(value.data), id);
}
