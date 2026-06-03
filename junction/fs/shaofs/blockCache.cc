#include "fs.h"
#include "blockCache.h"
#include "journal.h"
#include "utili.h"
#include "generic_cache/backend.h"
#include <utility>

extern "C" {
#include "spdk/env.h"
#include "runtime/thread.h"
#include "runtime/timer.h"
}

static GlobalBlockCache* g_block_cache_ptr = nullptr;
static constexpr uint32_t kWritebackQueueSize = 131072;
static constexpr uint32_t kWritebackBatchMax = 32;
static constexpr uint32_t kWritebackWorkerCount = 4;
static constexpr uint32_t kWritebackIdleSleepUs = 100;
static constexpr uint32_t kWritebackFullDrainRounds = 16;

struct WritebackItem {
    BlockID block;
    uint64_t gen;
};

struct WritebackQueue {
    mutex_t lock;
    condvar_t cv;
    WritebackItem items[kWritebackQueueSize];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    bool running;
    bool stop;
};

static WritebackQueue wbq;
static volatile int wbq_initialized;
static volatile uint64_t global_dirty_gen;
static volatile uint64_t wbq_dropped;

static inline void wbq_init_once()
{
    if (likely(atomic_read(&wbq_initialized))) return;

    mutex_init(&wbq.lock);
    condvar_init(&wbq.cv);
    wbq.head = 0;
    wbq.tail = 0;
    wbq.count = 0;
    wbq.running = false;
    wbq.stop = false;
    __atomic_store_n(&global_dirty_gen, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&wbq_dropped, 0, __ATOMIC_SEQ_CST);
    atomic_write(&wbq_initialized, 1);
}

static inline uint64_t next_dirty_gen()
{
    return __atomic_fetch_add(&global_dirty_gen, 1, __ATOMIC_SEQ_CST);
}

enum class BlockFlushPolicy {
    kSyncWriteback,
    kFsyncMetadataBatch,
    kBackgroundDataWriteback,
};

static inline bool block_entry_dirty_valid(CacheEntry<BlockID, BlockData>* entry)
{
    return atomic_read(&entry->dirty) && atomic_read(&entry->valid);
}

static bool write_block_image(BlockID id, const void* image, BlockFlushPolicy policy)
{
#if CRASH_CONSISTENCY
    if (journal_is_metadata_block(id))
    {
        if (policy == BlockFlushPolicy::kFsyncMetadataBatch)
            return journal_commit_single_batched(id, image);
        return journal_commit_single(id, image);
    }
#endif

    if (policy == BlockFlushPolicy::kBackgroundDataWriteback)
        return storage_write(image, id, 1) == 0;
    return DMA_write_block(image, id);
}

static inline bool flush_policy_cleans_entry(BlockID id, BlockFlushPolicy policy)
{
#if CRASH_CONSISTENCY
    if (journal_is_metadata_block(id))
        return policy != BlockFlushPolicy::kFsyncMetadataBatch && journal_commit_returns_after_checkpoint(id);
#else
    (void)id;
#endif
    (void)policy;
    return true;
}

static bool flush_locked_block_entry(BlockID id, CacheEntry<BlockID, BlockData>* entry, BlockFlushPolicy policy, uint64_t expected_gen = 0, bool check_gen = false)
{
    if (!block_entry_dirty_valid(entry)) return true;

    uint64_t gen = __atomic_load_n(&entry->dirty_gen, __ATOMIC_SEQ_CST);
    if (check_gen && gen != expected_gen) return true;

    if (!write_block_image(id, entry->data.data, policy)) return false;

    if (flush_policy_cleans_entry(id, policy) &&
        (!check_gen || __atomic_load_n(&entry->dirty_gen, __ATOMIC_SEQ_CST) == expected_gen))
        atomic_write(&entry->dirty, 0);
    return true;
}

static bool flush_cached_block(BlockID id, BlockFlushPolicy policy)
{
    BlockHandle h = get_block_cache().find_cached(id);
    if (!h) return true;

    auto* entry = h.get_entry();
    auto acc = h.read_access();
    return flush_locked_block_entry(id, entry, policy);
}

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

    g_block_cache_ptr = new GlobalBlockCache(capacity, shard_num, &NVMeSSD::getInstance(), []() { return new MetadataAwareLRUPolicy(); }, buffer);
    wbq_init_once();
    log_info("block cache initialized with %zu shards, each shard has %zu capacity, total capacity is %zu", g_block_cache_ptr->get_shard_count(), g_block_cache_ptr->get_shard_capacity(), g_block_cache_ptr->get_total_capacity());
}

BlockHandle bc_get_handle(BlockID id, bool fetch_on_miss)
{
    return get_block_cache().getHandle(id, fetch_on_miss);
}

BlockHandle bc_get_handle(BlockID id) { return bc_get_handle(id, true); }

bool bc_flush_block(BlockID id)
{
    return flush_cached_block(id, BlockFlushPolicy::kSyncWriteback);
}

bool bc_flush_block_batched(BlockID id)
{
    return flush_cached_block(id, BlockFlushPolicy::kFsyncMetadataBatch);
}
void bc_invalidate_block(BlockID id)  {        get_block_cache().invalidate(id);  }
void bc_flush_all()                   {        get_block_cache().flush_all();     }

bool bc_mark_block_dirty(BlockHandle& h)
{
    if (unlikely(!h)) return false;
    atomic_write(&h.get_entry()->dirty, 1);
    __atomic_store_n(&h.get_entry()->dirty_gen, next_dirty_gen(), __ATOMIC_SEQ_CST);
    return true;
}

static bool wbq_enqueue(BlockID block, uint64_t gen)
{
    wbq_init_once();

    mutex_lock(&wbq.lock);
    if (unlikely(wbq.count == kWritebackQueueSize))
    {
        __atomic_add_fetch(&wbq_dropped, 1, __ATOMIC_RELAXED);
        mutex_unlock(&wbq.lock);
        return false;
    }

    wbq.items[wbq.tail] = {block, gen};
    wbq.tail = (wbq.tail + 1) % kWritebackQueueSize;
    wbq.count++;
    if (wbq.count >= kWritebackBatchMax) condvar_broadcast(&wbq.cv);
    else condvar_signal(&wbq.cv);
    mutex_unlock(&wbq.lock);
    return true;
}

static void writeback_requeue_if_dirty(CacheEntry<BlockID, BlockData>* entry, BlockID id)
{
    if (!atomic_read(&entry->dirty) || !atomic_read(&entry->valid)) return;

    int expected = 0;
    uint64_t gen = __atomic_load_n(&entry->dirty_gen, __ATOMIC_SEQ_CST);
    if (__atomic_compare_exchange_n(&entry->writeback_queued, &expected, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        if (unlikely(!wbq_enqueue(id, gen)))
            atomic_write(&entry->writeback_queued, 0);
    }
}

bool bc_mark_data_block_dirty(BlockHandle& h)
{
    if (unlikely(!h)) return false;

    auto* entry = h.get_entry();
    uint64_t gen = next_dirty_gen();
    __atomic_store_n(&entry->dirty_gen, gen, __ATOMIC_SEQ_CST);
    atomic_write(&entry->dirty, 1);

    int expected = 0;
    if (__atomic_compare_exchange_n(&entry->writeback_queued, &expected, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        if (likely(wbq_enqueue(entry->key, gen))) return true;
        atomic_write(&entry->writeback_queued, 0);
        return false;
    }

    return true;
}

bool bc_clean_block_if_unchanged(BlockID id, const void* image)
{
    BlockHandle h = get_block_cache().find_cached(id);
    if (!h) return true;

    auto* entry = h.get_entry();
    if (!block_entry_dirty_valid(entry)) return true;

    auto acc = h.read_access();
    if (block_entry_dirty_valid(entry) && memcmp(acc->data, image, BLOCK_SIZE) == 0)
        atomic_write(&entry->dirty, 0);
    return true;
}

bool bc_is_cached_valid(BlockID id)
{
    BlockHandle h = get_block_cache().find_cached(id);
    return h && atomic_read(&h.get_entry()->valid);
}

static bool writeback_flush_item(BlockID id, uint64_t queued_gen)
{
    BlockHandle h = get_block_cache().find_cached(id);
    if (!h) return true;

    auto* entry = h.get_entry();
    if (!block_entry_dirty_valid(entry))
    {
        atomic_write(&entry->writeback_queued, 0);
        return true;
    }

    if (__atomic_load_n(&entry->dirty_gen, __ATOMIC_SEQ_CST) != queued_gen)
    {
        atomic_write(&entry->writeback_queued, 0);
        writeback_requeue_if_dirty(entry, id);
        return true;
    }

    auto acc = h.read_access();
    (void)acc;
    bool ok = flush_locked_block_entry(id, entry, BlockFlushPolicy::kBackgroundDataWriteback, queued_gen, true);

    atomic_write(&entry->writeback_queued, 0);
    if (ok)
        writeback_requeue_if_dirty(entry, id);
    return ok;
}

struct StorageBlockEntryCompat {
    uint64_t lba;
    char* data;
};

static bool writeback_flush_single_items(const WritebackItem* items, uint32_t nr)
{
    bool ok = true;
    for (uint32_t i = 0; i < nr; i++)
        ok &= writeback_flush_item(items[i].block, items[i].gen);
    return ok;
}

static bool writeback_flush_contiguous_run(const WritebackItem* items, uint32_t nr)
{
    BlockHandle handles[kWritebackBatchMax];
    CacheEntry<BlockID, BlockData>* entries[kWritebackBatchMax];
    StorageBlockEntryCompat sgl_entries[kWritebackBatchMax];
    void* sgl_ptrs[kWritebackBatchMax];

    for (uint32_t i = 0; i < nr; i++)
    {
        handles[i] = get_block_cache().find_cached(items[i].block);
        if (!handles[i])
        {
            for (uint32_t j = 0; j < i; j++) handles[j] = BlockHandle();
            return writeback_flush_single_items(items, nr);
        }

        entries[i] = handles[i].get_entry();
        if (!block_entry_dirty_valid(entries[i]) ||
            __atomic_load_n(&entries[i]->dirty_gen, __ATOMIC_SEQ_CST) != items[i].gen)
        {
            for (uint32_t j = 0; j <= i; j++) handles[j] = BlockHandle();
            return writeback_flush_single_items(items, nr);
        }
    }

    for (uint32_t i = 0; i < nr; i++)
        rwmutex_rdlock(&entries[i]->rw_mtx);

    bool flushable = true;
    for (uint32_t i = 0; i < nr; i++)
    {
        if (!block_entry_dirty_valid(entries[i]) ||
            __atomic_load_n(&entries[i]->dirty_gen, __ATOMIC_SEQ_CST) != items[i].gen)
        {
            flushable = false;
            break;
        }
    }

    bool ok = true;
    if (flushable)
    {
        for (uint32_t i = 0; i < nr; i++)
        {
            sgl_entries[i] = {items[i].block, entries[i]->data.data};
            sgl_ptrs[i] = &sgl_entries[i];
        }

        ok = write_blocks_to_disk(items[0].block, nr, sgl_ptrs) == 0;
        if (ok)
        {
            for (uint32_t i = 0; i < nr; i++)
            {
                if (__atomic_load_n(&entries[i]->dirty_gen, __ATOMIC_SEQ_CST) == items[i].gen)
                    atomic_write(&entries[i]->dirty, 0);
            }
        }
    }

    for (uint32_t i = nr; i > 0; i--)
        rwmutex_unlock(&entries[i - 1]->rw_mtx);

    if (!flushable)
    {
        for (uint32_t i = 0; i < nr; i++) handles[i] = BlockHandle();
        return writeback_flush_single_items(items, nr);
    }

    for (uint32_t i = 0; i < nr; i++)
    {
        atomic_write(&entries[i]->writeback_queued, 0);
        if (ok) writeback_requeue_if_dirty(entries[i], items[i].block);
    }

    return ok;
}

static void writeback_flush_batch(const WritebackItem* batch, uint32_t nr)
{
    uint32_t i = 0;
    while (i < nr)
    {
        uint32_t j = i + 1;
        while (j < nr && batch[j].block == batch[j - 1].block + 1)
            j++;

        if (j - i > 1)
            writeback_flush_contiguous_run(&batch[i], j - i);
        else
            writeback_flush_item(batch[i].block, batch[i].gen);

        i = j;
    }
}

static uint32_t wbq_take(WritebackItem* out, uint32_t cap, bool wait)
{
    wbq_init_once();
    mutex_lock(&wbq.lock);
    while (wait && wbq.count == 0 && !wbq.stop)
        condvar_wait(&wbq.cv, &wbq.lock);

    uint32_t nr = MIN(cap, wbq.count);
    for (uint32_t i = 0; i < nr; i++)
    {
        out[i] = wbq.items[wbq.head];
        wbq.head = (wbq.head + 1) % kWritebackQueueSize;
        wbq.count--;
    }
    mutex_unlock(&wbq.lock);
    return nr;
}

static void block_writeback_worker(void*)
{
    WritebackItem batch[kWritebackBatchMax];
    while (true)
    {
        uint32_t nr = wbq_take(batch, kWritebackBatchMax, true);
        if (nr == 0)
        {
            mutex_lock(&wbq.lock);
            bool stop = wbq.stop && wbq.count == 0;
            mutex_unlock(&wbq.lock);
            if (stop) break;
            timer_sleep(kWritebackIdleSleepUs);
            continue;
        }

        writeback_flush_batch(batch, nr);
    }
}

void bc_start_writeback()
{
    wbq_init_once();

    mutex_lock(&wbq.lock);
    if (!wbq.running)
    {
        wbq.stop = false;
        wbq.running = true;
        uint32_t spawned = 0;
        for (uint32_t i = 0; i < kWritebackWorkerCount; i++)
        {
            int ret = thread_spawn(block_writeback_worker, nullptr);
            if (ret != 0)
            {
                log_warn("[blockCache] failed to spawn writeback worker %u, ret=%d", i, ret);
                continue;
            }
            spawned++;
        }
        if (spawned == 0)
            wbq.running = false;
    }
    mutex_unlock(&wbq.lock);
}

static void drain_writeback_queue_once()
{
    WritebackItem batch[kWritebackBatchMax];
    while (true)
    {
        uint32_t nr = wbq_take(batch, kWritebackBatchMax, false);
        if (nr == 0) break;
        writeback_flush_batch(batch, nr);
    }
}

void bc_drain_writeback()
{
    wbq_init_once();

    for (uint32_t i = 0; i < kWritebackFullDrainRounds; i++)
    {
        drain_writeback_queue_once();
        uint64_t dropped = __atomic_exchange_n(&wbq_dropped, 0, __ATOMIC_RELAXED);
        if (dropped == 0) break;
        bc_flush_all();
    }
}

void bc_stop_writeback_and_drain()
{
    wbq_init_once();

    bc_drain_writeback();

    mutex_lock(&wbq.lock);
    wbq.stop = true;
    condvar_broadcast(&wbq.cv);
    mutex_unlock(&wbq.lock);
}

bool bc_flush_blocks_contiguous(BlockID start, uint32_t count)
{
    static constexpr uint32_t kMaxBatch = 16;

    struct CachedFlushRun {
        BlockID start = INVALID_BLOCK_ID;
        uint32_t nr = 0;
        BlockHandle handles[kMaxBatch];
        CacheEntry<BlockID, BlockData>* entries[kMaxBatch];
        StorageBlockEntryCompat sgl_entries[kMaxBatch];
        void* sgl_ptrs[kMaxBatch];

        void clear_handles()
        {
            for (uint32_t i = 0; i < nr; i++) handles[i] = BlockHandle();
            nr = 0;
            start = INVALID_BLOCK_ID;
        }

        void add(BlockID id, BlockHandle&& h, CacheEntry<BlockID, BlockData>* entry)
        {
            if (nr == 0) start = id;
            handles[nr] = std::move(h);
            entries[nr] = entry;
            nr++;
        }

        void lock_entries()
        {
            for (uint32_t i = 0; i < nr; i++)
                rwmutex_rdlock(&entries[i]->rw_mtx);
        }

        void unlock_entries_reverse()
        {
            for (uint32_t i = nr; i > 0; i--)
                rwmutex_unlock(&entries[i - 1]->rw_mtx);
        }

        bool still_valid_and_dirty() const
        {
            bool all_valid = true;
            bool any_dirty = false;
            for (uint32_t i = 0; i < nr; i++)
            {
                all_valid &= atomic_read(&entries[i]->valid);
                any_dirty |= block_entry_dirty_valid(entries[i]);
            }
            return all_valid && any_dirty;
        }

        bool write_locked_run()
        {
            for (uint32_t i = 0; i < nr; i++)
            {
                sgl_entries[i] = {start + i, entries[i]->data.data};
                sgl_ptrs[i] = &sgl_entries[i];
            }

            bool ok = nr == 1 ? write_block_image(start, entries[0]->data.data, BlockFlushPolicy::kSyncWriteback)
                              : write_blocks_to_disk(start, nr, sgl_ptrs) == 0;
            if (!ok) return false;

            for (uint32_t i = 0; i < nr; i++)
            {
                BlockID id = start + i;
                if (flush_policy_cleans_entry(id, BlockFlushPolicy::kSyncWriteback))
                    atomic_write(&entries[i]->dirty, 0);
            }
            return true;
        }

        bool flush()
        {
            if (nr == 0) return true;

            lock_entries();
            bool ok = !still_valid_and_dirty() || write_locked_run();
            unlock_entries_reverse();

            if (!ok) return false;
            clear_handles();
            return true;
        }
    } run;

    for (uint32_t i = 0; i < count; i++)
    {
        BlockID id = start + i;

#if CRASH_CONSISTENCY
        if (journal_is_metadata_block(id))
        {
            if (!run.flush() || !bc_flush_block(id)) return false;
            continue;
        }
#endif

        BlockHandle h = get_block_cache().find_cached(id);
        if (!h)
        {
            if (!run.flush()) return false;
            continue;
        }

        auto* entry = h.get_entry();
        if (!block_entry_dirty_valid(entry))
        {
            if (!run.flush()) return false;
            continue;
        }

        run.add(id, std::move(h), entry);

        if (run.nr == kMaxBatch && !run.flush()) return false;
    }

    return run.flush();
}

bool bc_write_backend(BlockID id, const BlockData& value)
{
#if CRASH_CONSISTENCY
    if (journal_is_metadata_block(id))
    {
        return journal_commit_single(id, value.data);
    }
#endif
    return DMA_write_block(static_cast<const void*>(value.data), id);
}
