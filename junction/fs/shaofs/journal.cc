#include "journal.h"

#if CRASH_CONSISTENCY

#include "dir.h"
#include "blockCache.h"
#include "extent.h"
#include "utili.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <algorithm>

extern "C" {
#include "runtime/thread.h"
#include "runtime/timer.h"
}

struct JournalHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t entry_count;
    uint64_t seq;
    uint64_t checksum;
};
static_assert(sizeof(JournalHeader) <= BLOCK_SIZE, "JournalHeader must fit in one block");

struct JournalEntry {
    BlockID home_block;
    BlockID image_block;
    uint64_t checksum;
};
static_assert(sizeof(JournalEntry) <= BLOCK_SIZE, "JournalEntry must fit in one block");

static constexpr uint32_t kJournalMagic = 0x4a53484f;  // "JSHO"
static constexpr uint32_t kJournalVersion = 1;
static constexpr uint32_t kJournalEmpty = 0;
static constexpr uint32_t kJournalCommitted = 1;
static constexpr uint32_t kJournalDirty = 2;
static constexpr uint32_t kMaxJournalEntries = 64;
static constexpr uint32_t kJournalSlotBlocks = 2 + kMaxJournalEntries;
static constexpr uint32_t kMaxJournalSlots = 32;
static_assert(kMaxJournalEntries * sizeof(JournalEntry) <= BLOCK_SIZE, "journal entry table must fit in one block");

static spinlock_t metadata_lock;
static mutex_t journal_commit_lock;
static mutex_t group_lock;
static condvar_t group_cv;
static mutex_t checkpoint_lock;
static condvar_t checkpoint_cv;
static uint64_t journal_seq;

struct BlockRange {
    BlockID start;
    uint64_t count;
};
static constexpr uint32_t kMaxMetadataRanges = 65536;
static BlockRange metadata_ranges[kMaxMetadataRanges];
static uint32_t metadata_range_count;

static constexpr uint32_t kGroupCommitMaxReqs = 128;

struct JournalGroupReq {
    BlockID block;
    const void* image;
    bool done;
    bool ok;
    bool queued;
};

static JournalGroupReq* group_pending[kGroupCommitMaxReqs];
static uint32_t group_pending_count;
static bool group_committing;

struct JournalBatch {
    JournalGroupReq* reqs[kMaxJournalEntries];
    uint32_t count;
};

static bool journal_ready;
static uint32_t journal_slot_count;

struct CheckpointTask {
    uint32_t slot;
    BlockID header_lba;
    uint32_t count;
    BlockID blocks[kMaxJournalEntries];
    JournalEntry entries[kMaxJournalEntries];
    alignas(BLOCK_SIZE) char images[kMaxJournalEntries][BLOCK_SIZE];
};

static CheckpointTask checkpoint_tasks[kMaxJournalSlots];
static CheckpointTask* checkpoint_queue[kMaxJournalSlots];
static uint8_t checkpoint_slot_busy[kMaxJournalSlots];
static uint32_t checkpoint_head;
static uint32_t checkpoint_tail;
static uint32_t checkpoint_count;
static uint32_t checkpoint_inflight;
static bool checkpoint_active;
static bool checkpoint_running;
static bool checkpoint_failed;

static inline uint64_t fnv1a64(const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) 
    {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static inline bool journal_layout_valid()
{
    return sb.journal_blocknum >= 4 && sb.journal_blockstart > sb.group_blockstart && sb.journal_blockstart + sb.journal_blocknum <= sb.total_blocknum;
}

static inline BlockID mount_state_lba()
{
    return sb.journal_blockstart + sb.journal_blocknum - 1;
}

static inline uint32_t journal_slot_capacity()
{
    if (!journal_layout_valid()) return 0;
    uint64_t usable_blocks = mount_state_lba() - sb.journal_blockstart;
    uint64_t slots = usable_blocks / kJournalSlotBlocks;
    return MIN((uint64_t)kMaxJournalSlots, slots);
}

static inline BlockID journal_slot_base(uint32_t slot)
{
    return sb.journal_blockstart + (uint64_t)slot * kJournalSlotBlocks;
}

static inline uint32_t journal_slot_from_seq(uint64_t seq)
{
    return journal_slot_count ? ((seq - 1) % journal_slot_count) : 0;
}

static inline uint64_t header_checksum(const JournalHeader& hdr, const JournalEntry* entries)
{
    JournalHeader tmp = hdr;
    tmp.checksum = 0;

    uint64_t h = fnv1a64(&tmp, sizeof(tmp));
    h ^= fnv1a64(entries, sizeof(JournalEntry) * hdr.entry_count);
    h *= 1099511628211ULL;
    return h;
}

static bool write_zero_block(BlockID blk)
{
    alignas(BLOCK_SIZE) char zero[BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    return storage_write(zero, blk, 1) == 0;
}

static bool read_block_header(BlockID blk, JournalHeader* hdr)
{
    alignas(BLOCK_SIZE) char block[BLOCK_SIZE];
    if (storage_read(block, blk, 1) != 0) return false;
    memcpy(hdr, block, sizeof(*hdr));
    return true;
}

static bool write_block_header(BlockID blk, const JournalHeader& hdr)
{
    alignas(BLOCK_SIZE) char block[BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    memcpy(block, &hdr, sizeof(hdr));
    return storage_write(block, blk, 1) == 0;
}

static bool read_full_block(BlockID blk, void* out)
{
    return storage_read(out, blk, 1) == 0;
}

static bool write_full_block(BlockID blk, const void* image)
{
    if (journal_is_metadata_block(blk)) return journal_commit_single(blk, image);
    return storage_write(image, blk, 1) == 0;
}

static bool read_header_at(BlockID header_lba, JournalHeader* hdr, JournalEntry* entries)
{
    alignas(BLOCK_SIZE) char header_block[BLOCK_SIZE];
    alignas(BLOCK_SIZE) char entry_block[BLOCK_SIZE];

    if (storage_read(header_block, header_lba, 1) != 0) return false;
    memcpy(hdr, header_block, sizeof(*hdr));

    if (storage_read(entry_block, header_lba + 1, 1) != 0) return false;
    memcpy(entries, entry_block, sizeof(JournalEntry) * kMaxJournalEntries);
    return true;
}

static bool write_entry_table(BlockID entry_lba, const JournalEntry* entries, uint32_t count)
{
    alignas(BLOCK_SIZE) char entry_block[BLOCK_SIZE];
    memset(entry_block, 0, sizeof(entry_block));
    memcpy(entry_block, entries, sizeof(JournalEntry) * count);
    return storage_write(entry_block, entry_lba, 1) == 0;
}

static uint64_t journal_next_seq_locked()
{
    return journal_seq++;
}

struct StorageBlockEntryCompat {
    uint64_t lba;
    char* data;
};

static constexpr uint32_t kJournalVectorWriteMax = 16;

static bool journal_write_contiguous_blocks(BlockID start_lba, const void* const* images, uint32_t count, bool direct_dma)
{
    uint32_t done = 0;

    while (done < count)
    {
        uint32_t nr = std::min<uint32_t>(count - done, kJournalVectorWriteMax);

        if (!direct_dma || nr == 1)
        {
            for (uint32_t i = 0; i < nr; i++)
                if (storage_write(images[done + i], start_lba + done + i, 1) != 0) return false;
            done += nr;
            continue;
        }

        StorageBlockEntryCompat entries[kJournalVectorWriteMax];
        void* ptrs[kJournalVectorWriteMax];
        for (uint32_t i = 0; i < nr; i++)
        {
            entries[i].lba = start_lba + done + i;
            entries[i].data = const_cast<char*>(static_cast<const char*>(images[done + i]));
            ptrs[i] = &entries[i];
        }

        if (write_blocks_to_disk(start_lba + done, nr, ptrs) != 0) return false;
        done += nr;
    }

    return true;
}

static bool checkpoint_home_blocks(BlockID header_lba, const BlockID* blocks, const void* const* images, uint32_t count, bool direct_dma)
{
    uint32_t i = 0;
    while (i < count)
    {
        uint32_t run = 1;
        while (i + run < count && blocks[i + run] == blocks[i] + run)
            run++;

        if (!journal_write_contiguous_blocks(blocks[i], images + i, run, direct_dma)) return false;
        i += run;
    }

    return write_zero_block(header_lba);
}

static bool checkpoint_task_home_blocks(const CheckpointTask* task)
{
    for (uint32_t i = 0; i < task->count; i++)
    {
        if (storage_write(task->images[i], task->blocks[i], 1) != 0) return false;
        bc_clean_block_if_unchanged(task->blocks[i], task->images[i]);
    }

    return write_zero_block(task->header_lba);
}

static void checkpoint_task_complete(CheckpointTask* task, bool ok)
{
    mutex_lock(&checkpoint_lock);
    checkpoint_slot_busy[task->slot] = 0;
    checkpoint_inflight--;
    if (!ok) checkpoint_failed = true;
    condvar_broadcast(&checkpoint_cv);
    mutex_unlock(&checkpoint_lock);
}

static CheckpointTask* checkpoint_take_task()
{
    mutex_lock(&checkpoint_lock);
    while (checkpoint_running && checkpoint_count == 0)
        condvar_wait(&checkpoint_cv, &checkpoint_lock);

    CheckpointTask* task = nullptr;
    if (checkpoint_count > 0)
    {
        task = checkpoint_queue[checkpoint_head];
        checkpoint_head = (checkpoint_head + 1) % kMaxJournalSlots;
        checkpoint_count--;
    }
    mutex_unlock(&checkpoint_lock);
    return task;
}

static void checkpoint_worker(void*)
{
    while (true)
    {
        CheckpointTask* task = checkpoint_take_task();
        if (task == nullptr)
        {
            mutex_lock(&checkpoint_lock);
            bool done = !checkpoint_running && checkpoint_count == 0;
            mutex_unlock(&checkpoint_lock);
            if (done) break;
            timer_sleep(50);
            continue;
        }

        checkpoint_task_complete(task, checkpoint_task_home_blocks(task));
    }
}

static void checkpoint_reset_state()
{
    checkpoint_head = 0;
    checkpoint_tail = 0;
    checkpoint_count = 0;
    checkpoint_inflight = 0;
    checkpoint_active = false;
    checkpoint_running = false;
    checkpoint_failed = false;
    memset(checkpoint_slot_busy, 0, sizeof(checkpoint_slot_busy));
}

static void checkpoint_start_if_needed()
{
    if (checkpoint_active || journal_slot_count == 0) return;

    checkpoint_active = true;
    checkpoint_running = true;
    int ret = thread_spawn(checkpoint_worker, nullptr);
    if (ret != 0)
    {
        log_warn("[journal] failed to spawn checkpoint worker, ret=%d; falling back to synchronous checkpoint", ret);
        checkpoint_active = false;
        checkpoint_running = false;
    }
}

static bool checkpoint_wait_slot_free(uint32_t slot)
{
    mutex_lock(&checkpoint_lock);
    while (checkpoint_slot_busy[slot] && !checkpoint_failed)
        condvar_wait(&checkpoint_cv, &checkpoint_lock);
    bool ok = !checkpoint_failed;
    mutex_unlock(&checkpoint_lock);
    return ok;
}

static bool checkpoint_enqueue_task(uint32_t slot, BlockID header_lba, const BlockID* blocks, const void* const* images, const JournalEntry* entries, uint32_t count)
{
    mutex_lock(&checkpoint_lock);
    if (!checkpoint_active || checkpoint_failed || checkpoint_count == kMaxJournalSlots || checkpoint_slot_busy[slot])
    {
        mutex_unlock(&checkpoint_lock);
        return false;
    }

    CheckpointTask* task = &checkpoint_tasks[slot];
    task->slot = slot;
    task->header_lba = header_lba;
    task->count = count;
    for (uint32_t i = 0; i < count; i++)
    {
        task->blocks[i] = blocks[i];
        task->entries[i] = entries[i];
        memcpy(task->images[i], images[i], BLOCK_SIZE);
    }

    checkpoint_slot_busy[slot] = 1;
    checkpoint_queue[checkpoint_tail] = task;
    checkpoint_tail = (checkpoint_tail + 1) % kMaxJournalSlots;
    checkpoint_count++;
    checkpoint_inflight++;
    condvar_signal(&checkpoint_cv);
    mutex_unlock(&checkpoint_lock);
    return true;
}

void journal_drain_checkpoint()
{
    if (!journal_layout_valid()) return;

    mutex_lock(&checkpoint_lock);
    while (checkpoint_count != 0 || checkpoint_inflight != 0)
        condvar_wait(&checkpoint_cv, &checkpoint_lock);
    mutex_unlock(&checkpoint_lock);
}

static bool replay_committed(BlockID header_lba, const JournalHeader& hdr, const JournalEntry* entries)
{
    alignas(BLOCK_SIZE) char block[BLOCK_SIZE];

    for (uint32_t i = 0; i < hdr.entry_count; ++i) 
    {
        const JournalEntry& e = entries[i];
        if (e.home_block >= sb.total_blocknum || (e.home_block >= sb.journal_blockstart && e.home_block < sb.journal_blockstart + sb.journal_blocknum)) 
        {
            log_err("[journal] invalid replay target block %lu", e.home_block);
            return false;
        }
        if (e.image_block < header_lba + 2 || e.image_block >= header_lba + kJournalSlotBlocks)
        {
            log_err("[journal] invalid image block %lu", e.image_block);
            return false;
        }

        if (storage_read(block, e.image_block, 1) != 0) return false;
        if (fnv1a64(block, BLOCK_SIZE) != e.checksum) 
        {
            log_err("[journal] checksum mismatch for home block %lu", e.home_block);
            return false;
        }
        if (storage_write(block, e.home_block, 1) != 0) return false;
    }

    return write_zero_block(header_lba);
}

static inline void bitmap_set_local(unsigned long* bmap, uint32_t bit)
{
    bmap[bit / (sizeof(unsigned long) * 8)] |= 1UL << (bit % (sizeof(unsigned long) * 8));
}

static inline bool bitmap_test_local(const unsigned long* bmap, uint32_t bit)
{
    return (bmap[bit / (sizeof(unsigned long) * 8)] >> (bit % (sizeof(unsigned long) * 8))) & 1UL;
}

static inline void bitmap_clear_local(unsigned long* bmap, uint32_t bit)
{
    bmap[bit / (sizeof(unsigned long) * 8)] &= ~(1UL << (bit % (sizeof(unsigned long) * 8)));
}

template <typename T>
class CFreeBuffer {
    T* ptr_;

public:
    CFreeBuffer() : ptr_(nullptr) {}
    ~CFreeBuffer() { free(ptr_); }

    CFreeBuffer(const CFreeBuffer&) = delete;
    CFreeBuffer& operator=(const CFreeBuffer&) = delete;

    bool alloc_aligned(size_t bytes)
    {
        ptr_ = static_cast<T*>(aligned_alloc(BLOCK_SIZE, bytes));
        return ptr_ != nullptr;
    }

    bool alloc_zeroed(size_t count)
    {
        ptr_ = static_cast<T*>(calloc(count, sizeof(T)));
        return ptr_ != nullptr;
    }

    T* get() const { return ptr_; }
    T& operator[](size_t idx) { return ptr_[idx]; }
    const T& operator[](size_t idx) const { return ptr_[idx]; }
};

static bool group_for_data_block(BlockID block, uint32_t* gid, uint32_t* bit)
{
    if (block < sb.group_blockstart || block >= sb.journal_blockstart) return false;
    uint64_t off = block - sb.group_blockstart;
    uint64_t group = off / TOTALBLOCKS_PERGROUP;
    uint64_t in_group = off % TOTALBLOCKS_PERGROUP;
    if (group >= sb.group_num || in_group < BMAPNUM_PERGROUP) return false;
    *gid = group;
    *bit = in_group - BMAPNUM_PERGROUP;
    return *bit < DATABLOCKS_PERGROUP;
}

static bool mark_extent_allocated(unsigned long* group_bitmaps, uint32_t* used_counts, const iExtent& ext)
{
    for (uint64_t i = 0; i < ext.block_count; ++i) 
    {
        uint32_t gid, bit;
        if (!group_for_data_block(ext.physical_start + i, &gid, &bit)) return false;
        unsigned long* bmap = group_bitmaps + (uint64_t)gid * (BLOCK_SIZE / sizeof(unsigned long));
        if (!bitmap_test_local(bmap, bit)) 
        {
            bitmap_set_local(bmap, bit);
            used_counts[gid]++;
        }
    }
    return true;
}

struct ExtentValidationCtx {
    bool have_prev;
    BlockID prev_logical;
};

static bool validate_disk_extent_cb(const iExtent& ext, void* arg)
{
    ExtentValidationCtx* ctx = static_cast<ExtentValidationCtx*>(arg);
    if (ext.block_count == 0) return false;
    uint32_t gid, bit;
    if (!group_for_data_block(ext.physical_start, &gid, &bit)) return false;
    if (!group_for_data_block(ext.physical_start + ext.block_count - 1, &gid, &bit)) return false;
    if (ctx->have_prev && ext.logical_start <= ctx->prev_logical) return false;
    ctx->have_prev = true;
    ctx->prev_logical = ext.logical_start;
    return true;
}

struct MetadataBlockValidationCtx {
    bool skip_root;
};

static bool validate_metadata_block_cb(BlockID block, void* arg)
{
    MetadataBlockValidationCtx* ctx = static_cast<MetadataBlockValidationCtx*>(arg);
    if (ctx && ctx->skip_root)
    {
        ctx->skip_root = false;
        return true;
    }

    uint32_t gid, bit;
    return group_for_data_block(block, &gid, &bit);
}

static bool inode_extent_valid(const DInode& din)
{
    if (din.valid_extent_count > DIRECT_EXTENT_NUM + (uint32_t)EXTENT_TREE_ROOT_REFS * (uint32_t)EXTENT_TREE_LEAF_EXTENTS) return false;
    uint32_t direct_count = MIN(din.valid_extent_count, (uint32_t)DIRECT_EXTENT_NUM);
    for (uint32_t i = 0; i < direct_count; ++i) 
    {
        const iExtent& ext = din.direct_extents[i];
        if (ext.block_count == 0) return false;
        uint32_t gid, bit;
        if (!group_for_data_block(ext.physical_start, &gid, &bit)) return false;
        if (!group_for_data_block(ext.physical_start + ext.block_count - 1, &gid, &bit)) return false;
    }
    if (din.valid_extent_count > DIRECT_EXTENT_NUM && din.indirect_extent_block == 0) return false;

    ExtentValidationCtx ctx = {};
    if (!disk_inode_for_each_extent(&din, false, validate_disk_extent_cb, &ctx)) return false;
    MetadataBlockValidationCtx meta_ctx = { .skip_root = true };
    return disk_inode_for_each_extent_metadata_block(&din, validate_metadata_block_cb, &meta_ctx);
}

static bool inode_is_live(const DInode* inodes, uint32_t inum)
{
    if (inum >= sb.inode_num) return false;
    const DInode& din = inodes[inum];
    if (!din.used) return false;
    if (din.type == UNKNOWN) return false;
    return inode_extent_valid(din);
}

static bool scrub_directory_block(BlockID block, const DInode* inodes)
{
    alignas(BLOCK_SIZE) char buf[BLOCK_SIZE];
    if (storage_read(buf, block, 1) != 0) return false;

    bool changed = false;
    Dirent* ents = reinterpret_cast<Dirent*>(buf);
    constexpr uint32_t ents_per_block = BLOCK_SIZE / sizeof(Dirent);
    for (uint32_t i = 0; i < ents_per_block; ++i) 
    {
        Dirent& d = ents[i];
        if (d.inum == 0 && d.name[0] == '\0') continue;
        if (strncmp(d.name, ".", NAMESIZ) == 0 || strncmp(d.name, "..", NAMESIZ) == 0) continue;
        if (!inode_is_live(inodes, d.inum) || inodes[d.inum].type != d.filetype) 
        {
            memset(&d, 0, sizeof(d));
            changed = true;
        }
    }

    return !changed || journal_commit_single(block, buf);
}

struct RepairAllocCtx {
    unsigned long* group_bitmaps;
    uint32_t* used_counts;
};

static bool mark_disk_extent_allocated_cb(const iExtent& ext, void* arg)
{
    RepairAllocCtx* ctx = static_cast<RepairAllocCtx*>(arg);
    return mark_extent_allocated(ctx->group_bitmaps, ctx->used_counts, ext);
}

struct RepairMetadataAllocCtx {
    RepairAllocCtx* alloc;
    bool skip_root;
};

static bool mark_extent_metadata_allocated_cb(BlockID block, void* arg)
{
    RepairMetadataAllocCtx* ctx = static_cast<RepairMetadataAllocCtx*>(arg);
    if (ctx->skip_root)
    {
        ctx->skip_root = false;
        return true;
    }

    iExtent meta_ext = { .logical_start = 0, .physical_start = block, .block_count = 1 };
    return mark_extent_allocated(ctx->alloc->group_bitmaps, ctx->alloc->used_counts, meta_ext);
}

struct MetadataBlockRegisterCtx {
    bool skip_root;
};

static bool register_metadata_block_cb(BlockID block, void* arg)
{
    MetadataBlockRegisterCtx* ctx = static_cast<MetadataBlockRegisterCtx*>(arg);
    if (ctx && ctx->skip_root)
    {
        ctx->skip_root = false;
        return true;
    }

    journal_register_metadata_block(block);
    return true;
}

static bool register_dir_data_extent_cb(const iExtent& ext, void*)
{
    journal_register_metadata_extent(ext.physical_start, ext.block_count);
    return true;
}

static bool scrub_dir_extent_cb(const iExtent& ext, void* arg)
{
    const DInode* inodes = static_cast<const DInode*>(arg);
    for (uint64_t b = 0; b < ext.block_count; ++b)
        scrub_directory_block(ext.physical_start + b, inodes);
    return true;
}

static bool repair_load_inode_table(DInode* inodes)
{
    return storage_read(inodes, sb.itable_blockstart, sb.itable_blocknum) == 0;
}

static bool repair_rebuild_allocation_maps(DInode* inodes,
                                           unsigned long* new_imap,
                                           unsigned long* group_bitmaps,
                                           uint32_t* used_counts)
{
    for (uint32_t inum = 0; inum < sb.inode_num; ++inum) 
    {
        DInode& din = inodes[inum];
        if (!din.used) continue;
        if (din.type == UNKNOWN || !inode_extent_valid(din)) 
        {
            memset(&din, 0, sizeof(din));
            din.idx = inum;
            din.indirect_extent_block = sb.indirect_block_start + inum;
            continue;
        }

        bitmap_set_local(new_imap, inum);

        RepairAllocCtx alloc_ctx = { .group_bitmaps = group_bitmaps, .used_counts = used_counts };
        if (!disk_inode_for_each_extent(&din, true, mark_disk_extent_allocated_cb, &alloc_ctx)) return false;
        RepairMetadataAllocCtx meta_alloc_ctx = { .alloc = &alloc_ctx, .skip_root = true };
        if (!disk_inode_for_each_extent_metadata_block(&din, mark_extent_metadata_allocated_cb, &meta_alloc_ctx)) return false;
    }
    return true;
}

static bool repair_register_and_scrub_directories(DInode* inodes)
{
    for (uint32_t inum = 0; inum < sb.inode_num; ++inum) 
    {
        const DInode& din = inodes[inum];
        if (!din.used || din.type != DIRECTORY) continue;

        MetadataBlockRegisterCtx meta_reg_ctx = { .skip_root = true };
        if (!disk_inode_for_each_extent_metadata_block(&din, register_metadata_block_cb, &meta_reg_ctx)) return false;
        if (!disk_inode_for_each_extent(&din, true, register_dir_data_extent_cb, nullptr)) return false;
        if (!disk_inode_for_each_extent(&din, true, scrub_dir_extent_cb, inodes)) return false;
    }
    return true;
}

static bool repair_write_back_state(const DInode* inodes,
                                    const unsigned long* new_imap,
                                    size_t imap_bytes,
                                    const unsigned long* group_bitmaps,
                                    const uint32_t* used_counts,
                                    GroupDescriptor* gdt)
{
    size_t gmap_longs_per_group = BLOCK_SIZE / sizeof(unsigned long);
    if (!journal_write_metadata(new_imap, imap_bytes, sb.imap_blockstart, 0)) return false;

    for (uint32_t gid = 0; gid < sb.group_num; ++gid) 
    {
        BlockID bitmap_lba = sb.group_blockstart + (uint64_t)gid * TOTALBLOCKS_PERGROUP;
        const unsigned long* bmap = group_bitmaps + (uint64_t)gid * gmap_longs_per_group;
        if (!journal_commit_single(bitmap_lba, bmap)) return false;

        gdt[gid].free_blocks_count = DATABLOCKS_PERGROUP - used_counts[gid];
        gdt[gid].next_free_hint = 0;
        while (gdt[gid].next_free_hint < DATABLOCKS_PERGROUP && bitmap_test_local(bmap, gdt[gid].next_free_hint)) gdt[gid].next_free_hint++;
        if (gdt[gid].next_free_hint >= DATABLOCKS_PERGROUP) gdt[gid].next_free_hint = 0;
    }
    if (!journal_write_metadata(gdt, sb.group_num * sizeof(GroupDescriptor), sb.gdt_blockstart, 0)) return false;
    return journal_write_metadata(inodes, sb.itable_blocknum * BLOCK_SIZE, sb.itable_blockstart, 0);
}

static bool repair_filesystem_state()
{
    size_t imap_bytes = sb.imap_blocknum * BLOCK_SIZE;
    size_t inode_bytes = sb.itable_blocknum * BLOCK_SIZE;
    size_t group_bitmap_bytes = (uint64_t)sb.group_num * BLOCK_SIZE;
    size_t gdt_bytes = sb.gdt_blocknum * BLOCK_SIZE;

    CFreeBuffer<DInode> inodes;
    CFreeBuffer<unsigned long> new_imap;
    CFreeBuffer<unsigned long> group_bitmaps;
    CFreeBuffer<uint32_t> used_counts;
    CFreeBuffer<GroupDescriptor> gdt;

    if (!inodes.alloc_aligned(inode_bytes)) return false;
    if (!repair_load_inode_table(inodes.get())) return false;

    if (!new_imap.alloc_aligned(imap_bytes)) return false;
    if (!group_bitmaps.alloc_aligned(group_bitmap_bytes)) return false;
    if (!used_counts.alloc_zeroed(sb.group_num)) return false;
    if (!gdt.alloc_aligned(gdt_bytes)) return false;
    memset(new_imap.get(), 0, imap_bytes);
    memset(group_bitmaps.get(), 0, group_bitmap_bytes);
    memset(gdt.get(), 0, gdt_bytes);

    return repair_rebuild_allocation_maps(inodes.get(), new_imap.get(), group_bitmaps.get(), used_counts.get()) &&
           repair_register_and_scrub_directories(inodes.get()) &&
           repair_write_back_state(inodes.get(), new_imap.get(), imap_bytes, group_bitmaps.get(), used_counts.get(), gdt.get());
}

struct ReplayTxn {
    BlockID header_lba;
    JournalHeader hdr;
    JournalEntry entries[kMaxJournalEntries];
};

enum class JournalSlotClass {
    kEmpty,
    kCommitted,
    kClearedNeedsRepair,
    kFatal,
};

static JournalSlotClass classify_journal_slot(uint32_t slot, ReplayTxn* txn)
{
    BlockID header_lba = journal_slot_base(slot);
    JournalHeader hdr;
    JournalEntry entries[kMaxJournalEntries];
    if (!read_header_at(header_lba, &hdr, entries)) return JournalSlotClass::kFatal;
    if (hdr.magic == 0 || hdr.state == kJournalEmpty) return JournalSlotClass::kEmpty;

    if (hdr.magic != kJournalMagic || hdr.version != kJournalVersion) 
    {
        log_warn("[journal] unknown journal header slot=%u magic=0x%x version=%u, clearing", slot, hdr.magic, hdr.version);
        return write_zero_block(header_lba) ? JournalSlotClass::kClearedNeedsRepair : JournalSlotClass::kFatal;
    }

    if (hdr.entry_count == 0 || hdr.entry_count > kMaxJournalEntries)
    {
        log_warn("[journal] invalid entry_count=%u in slot=%u, clearing header", hdr.entry_count, slot);
        return write_zero_block(header_lba) ? JournalSlotClass::kClearedNeedsRepair : JournalSlotClass::kFatal;
    }

    if (hdr.checksum != header_checksum(hdr, entries)) 
    {
        log_warn("[journal] incomplete or torn transaction in slot=%u, clearing header", slot);
        return write_zero_block(header_lba) ? JournalSlotClass::kClearedNeedsRepair : JournalSlotClass::kFatal;
    }

    if (hdr.state == kJournalCommitted) 
    {
        txn->header_lba = header_lba;
        txn->hdr = hdr;
        memcpy(txn->entries, entries, sizeof(entries));
        return JournalSlotClass::kCommitted;
    }

    log_warn("[journal] uncommitted transaction state=%u in slot=%u, clearing", hdr.state, slot);
    return write_zero_block(header_lba) ? JournalSlotClass::kClearedNeedsRepair : JournalSlotClass::kFatal;
}

static bool collect_replay_transactions(ReplayTxn* txns, uint32_t* txn_count, bool* needs_repair)
{
    *txn_count = 0;
    for (uint32_t slot = 0; slot < journal_slot_count; slot++)
    {
        ReplayTxn txn = {};
        switch (classify_journal_slot(slot, &txn))
        {
          case JournalSlotClass::kEmpty:
            break;
          case JournalSlotClass::kCommitted:
            txns[(*txn_count)++] = txn;
            *needs_repair = true;
            break;
          case JournalSlotClass::kClearedNeedsRepair:
            *needs_repair = true;
            break;
          case JournalSlotClass::kFatal:
            return false;
        }
    }
    return true;
}

static bool replay_transactions(ReplayTxn* txns, uint32_t txn_count, uint64_t* max_replayed_seq)
{
    std::sort(txns, txns + txn_count, [](const ReplayTxn& a, const ReplayTxn& b) {
        return a.hdr.seq < b.hdr.seq;
    });

    *max_replayed_seq = 0;
    for (uint32_t i = 0; i < txn_count; i++)
    {
        log_info("[journal] replaying txn seq=%lu blocks=%u", txns[i].hdr.seq, txns[i].hdr.entry_count);
        if (!replay_committed(txns[i].header_lba, txns[i].hdr, txns[i].entries)) return false;
        if (txns[i].hdr.seq > *max_replayed_seq) *max_replayed_seq = txns[i].hdr.seq;
        if (txns[i].hdr.seq >= journal_seq) journal_seq = txns[i].hdr.seq + 1;
    }
    return true;
}

static bool clear_slots_before_next_live(uint64_t max_replayed_seq)
{
    if (max_replayed_seq == 0 || journal_slot_count == 0) return true;

    uint32_t oldest_live_slot = journal_slot_from_seq(max_replayed_seq + 1);
    for (uint32_t slot = 0; slot < oldest_live_slot; slot++)
        if (!write_zero_block(journal_slot_base(slot))) return false;
    return true;
}

void journal_init()
{
    spin_lock_init(&metadata_lock);
    mutex_init(&journal_commit_lock);
    mutex_init(&group_lock);
    condvar_init(&group_cv);
    mutex_init(&checkpoint_lock);
    condvar_init(&checkpoint_cv);
    journal_seq = 1;
    group_pending_count = 0;
    group_committing = false;
    journal_ready = false;
    journal_slot_count = journal_slot_capacity();
    metadata_range_count = 0;
    checkpoint_reset_state();
}

bool journal_recover()
{
    if (!journal_layout_valid()) 
    {
        log_warn("[journal] invalid or missing journal area, skip recovery");
        return true;
    }
    journal_ready = false;
    journal_slot_count = journal_slot_capacity();
    if (journal_slot_count == 0)
    {
        log_warn("[journal] no usable journal slots, skip recovery");
        journal_ready = true;
        return true;
    }

    JournalHeader mount_hdr;
    if (!read_block_header(mount_state_lba(), &mount_hdr)) return false;
    bool needs_repair = mount_hdr.magic == kJournalMagic && mount_hdr.version == kJournalVersion && mount_hdr.state == kJournalDirty;

    ReplayTxn txns[kMaxJournalSlots];
    uint32_t txn_count = 0;
    if (!collect_replay_transactions(txns, &txn_count, &needs_repair)) return false;

    uint64_t max_replayed_seq = 0;
    if (!replay_transactions(txns, txn_count, &max_replayed_seq)) return false;

    if (!clear_slots_before_next_live(max_replayed_seq)) return false;

    if (needs_repair) 
    {
        log_info("[journal] previous mount was dirty, repairing metadata state");
        journal_ready = true;
        if (!repair_filesystem_state()) return false;
        bool ok = write_zero_block(mount_state_lba());
        return ok;
    }
    journal_ready = true;
    return true;
}

void journal_mark_dirty()
{
    if (!journal_layout_valid()) return;

    JournalHeader hdr;
    JournalEntry entries[kMaxJournalEntries];
    memset(entries, 0, sizeof(entries));
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = kJournalMagic;
    hdr.version = kJournalVersion;
    hdr.state = kJournalDirty;
    hdr.entry_count = 0;
    mutex_lock(&journal_commit_lock);
    hdr.seq = journal_next_seq_locked();
    hdr.checksum = header_checksum(hdr, entries);
    write_block_header(mount_state_lba(), hdr);
    mutex_unlock(&journal_commit_lock);
}

void journal_mark_clean()
{
    if (!journal_layout_valid()) return;

    mutex_lock(&journal_commit_lock);
    write_zero_block(mount_state_lba());
    mutex_unlock(&journal_commit_lock);
}

bool journal_is_metadata_block(BlockID block)
{
    if (block >= sb.total_blocknum) return false;
    if (block < sb.group_blockstart) return true;
    if (block >= sb.journal_blockstart && block < sb.journal_blockstart + sb.journal_blocknum) return true;
    if (block >= sb.journal_blockstart) return false;

    uint64_t off = block - sb.group_blockstart;
    if ((off % TOTALBLOCKS_PERGROUP) < BMAPNUM_PERGROUP) return true;

    bool is_meta = false;
    SpinGuard guard(&metadata_lock);
    uint32_t left = 0;
    uint32_t right = metadata_range_count;
    while (left < right)
    {
        uint32_t mid = left + (right - left) / 2;
        const BlockRange& r = metadata_ranges[mid];
        if (block >= r.start && block < r.start + r.count)
        {
            is_meta = true;
            break;
        }
        if (block < r.start) right = mid;
        else left = mid + 1;
    }
    return is_meta;
}

void journal_register_metadata_block(BlockID block)
{
    journal_register_metadata_extent(block, 1);
}

void journal_register_metadata_extent(BlockID start, uint64_t count)
{
    if (count == 0 || start == 0 || start >= sb.total_blocknum) return;
    if (start >= sb.journal_blockstart) return;
    if (count > sb.journal_blockstart - start) count = sb.journal_blockstart - start;

    SpinGuard guard(&metadata_lock);
    BlockID end = start + count;

    uint32_t pos = 0;
    while (pos < metadata_range_count && metadata_ranges[pos].start + metadata_ranges[pos].count < start)
        pos++;

    if (pos < metadata_range_count && end < metadata_ranges[pos].start)
    {
        if (metadata_range_count >= kMaxMetadataRanges)
        {
            log_err("[journal] metadata range table full, block=%lu count=%lu", start, count);
            return;
        }
        for (uint32_t i = metadata_range_count; i > pos; i--)
            metadata_ranges[i] = metadata_ranges[i - 1];
        metadata_ranges[pos] = {start, count};
        metadata_range_count++;
        return;
    }

    if (pos == metadata_range_count)
    {
        if (metadata_range_count >= kMaxMetadataRanges)
        {
            log_err("[journal] metadata range table full, block=%lu count=%lu", start, count);
            return;
        }
        metadata_ranges[metadata_range_count++] = {start, count};
        return;
    }

    BlockRange& first = metadata_ranges[pos];
    if (start < first.start) first.start = start;
    if (end > first.start + first.count) first.count = end - first.start;

    uint32_t write = pos + 1;
    for (uint32_t read = pos + 1; read < metadata_range_count; read++)
    {
        BlockRange& cur = metadata_ranges[read];
        BlockID first_end = first.start + first.count;
        if (cur.start <= first_end)
        {
            BlockID cur_end = cur.start + cur.count;
            if (cur_end > first_end) first.count = cur_end - first.start;
        }
        else
        {
            if (write != read) metadata_ranges[write] = cur;
            write++;
        }
    }
    metadata_range_count = write;
}

static bool journal_commit_blocks_impl(const BlockID* blocks, const void* const* images, uint32_t count, bool checkpoint_async, bool direct_dma)
{

    if (count == 0) return true;
    if (!journal_layout_valid()) return false;
    if (count > kMaxJournalEntries)
    {
        log_err("[journal] transaction too large: %u blocks", count);
        return false;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        if (!journal_is_metadata_block(blocks[i]))
        {
            log_err("[journal] refusing to journal non-metadata block %lu", blocks[i]);
            return false;
        }
    }

    if (!journal_ready || journal_slot_count == 0) return false;

    JournalEntry entries[kMaxJournalEntries];
    memset(entries, 0, sizeof(entries));

    mutex_lock(&journal_commit_lock);
    if (checkpoint_async) checkpoint_start_if_needed();

    uint64_t seq = journal_next_seq_locked();
    uint32_t slot = journal_slot_from_seq(seq);
    if (checkpoint_async && checkpoint_active && !checkpoint_wait_slot_free(slot))
    {
        mutex_unlock(&journal_commit_lock);
        return false;
    }

    BlockID header_lba = journal_slot_base(slot);
    for (uint32_t i = 0; i < count; ++i) 
    {
        entries[i].home_block = blocks[i];
        entries[i].image_block = header_lba + 2 + i;
        entries[i].checksum = fnv1a64(images[i], BLOCK_SIZE);
    }

    if (!journal_write_contiguous_blocks(header_lba + 2, images, count, direct_dma))
    {
        mutex_unlock(&journal_commit_lock);
        return false;
    }

    if (!write_entry_table(header_lba + 1, entries, count))
    {
        mutex_unlock(&journal_commit_lock);
        return false;
    }

    JournalHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = kJournalMagic;
    hdr.version = kJournalVersion;
    hdr.state = kJournalCommitted;
    hdr.entry_count = count;
    hdr.seq = seq;
    hdr.checksum = header_checksum(hdr, entries);

    bool ok = write_block_header(header_lba, hdr);
    if (ok)
    {
        if (checkpoint_async && checkpoint_active)
        {
            ok = checkpoint_enqueue_task(slot, header_lba, blocks, images, entries, count);
            if (!ok)
            {
                journal_drain_checkpoint();
                ok = checkpoint_home_blocks(header_lba, blocks, images, count, direct_dma);
            }
        }
        else
        {
            ok = checkpoint_home_blocks(header_lba, blocks, images, count, direct_dma);
        }
    }
    mutex_unlock(&journal_commit_lock);

    return ok;
}

bool journal_commit_blocks(const BlockID* blocks, const void* const* images, uint32_t count)
{
    return journal_commit_blocks_impl(blocks, images, count, false, false);
}

static bool journal_commit_blocks_async_checkpoint(const BlockID* blocks, const void* const* images, uint32_t count)
{
    return journal_commit_blocks_impl(blocks, images, count, true, true);
}

bool journal_commit_returns_after_checkpoint(BlockID block)
{
    (void)block;
    return true;
}

static bool journal_commit_group(JournalGroupReq** reqs, uint32_t req_count)
{
    if (req_count == 0) return true;

    BlockID blocks[kMaxJournalEntries];
    const void* images[kMaxJournalEntries];
    uint32_t count = 0;

    for (uint32_t i = 0; i < req_count; i++)
    {
        JournalGroupReq* req = reqs[i];
        uint32_t pos = count;
        for (uint32_t j = 0; j < count; j++)
        {
            if (blocks[j] == req->block)
            {
                pos = j;
                break;
            }
        }

        if (pos == count)
        {
            if (count == kMaxJournalEntries) return false;
            blocks[count++] = req->block;
        }
        images[pos] = req->image;
    }

    return journal_commit_blocks_async_checkpoint(blocks, images, count);
}

static void journal_batch_enqueue_locked(JournalGroupReq* req)
{
    while (!req->queued)
    {
        while (group_pending_count == kGroupCommitMaxReqs)
            condvar_wait(&group_cv, &group_lock);

        group_pending[group_pending_count++] = req;
        req->queued = true;
        condvar_broadcast(&group_cv);
    }
}

static void journal_batch_take_locked(JournalBatch* batch)
{
    batch->count = std::min(group_pending_count, kMaxJournalEntries);
    for (uint32_t i = 0; i < batch->count; i++)
        batch->reqs[i] = group_pending[i];

    uint32_t remain = group_pending_count - batch->count;
    for (uint32_t i = 0; i < remain; i++)
        group_pending[i] = group_pending[batch->count + i];
    group_pending_count = remain;
}

static void journal_batch_complete_locked(const JournalBatch& batch, bool ok)
{
    for (uint32_t i = 0; i < batch.count; i++)
    {
        batch.reqs[i]->ok = ok;
        batch.reqs[i]->done = true;
    }
    group_committing = false;
    condvar_broadcast(&group_cv);
}

static bool journal_commit_single_grouped(BlockID block, const void* image)
{

    JournalGroupReq req = {
        .block = block,
        .image = image,
        .done = false,
        .ok = false,
        .queued = false,
    };

    mutex_lock(&group_lock);
    journal_batch_enqueue_locked(&req);

    while (!req.done)
    {
        if (group_committing)
        {
            condvar_wait(&group_cv, &group_lock);
            continue;
        }

        group_committing = true;

        JournalBatch batch;
        journal_batch_take_locked(&batch);
        mutex_unlock(&group_lock);

        bool ok = journal_commit_group(batch.reqs, batch.count);

        mutex_lock(&group_lock);
        journal_batch_complete_locked(batch, ok);
    }

    bool ok = req.ok;
    mutex_unlock(&group_lock);
    return ok;
}

bool journal_commit_single(BlockID block, const void* image)
{
    BlockID blocks[1] = {block};
    const void* images[1] = {image};
    return journal_commit_blocks(blocks, images, 1);
}

bool journal_commit_single_batched(BlockID block, const void* image)
{
    return journal_commit_single_grouped(block, image);
}

bool journal_write_metadata(const void* data, size_t size, BlockID lba_start, off_t offset)
{
    if (size == 0) return true;

    const char* src = static_cast<const char*>(data);
    uint64_t start_block = lba_start + offset / BLOCK_SIZE;
    size_t inner = offset % BLOCK_SIZE;
    size_t remaining = size;

    alignas(BLOCK_SIZE) char block[BLOCK_SIZE];
    while (remaining > 0) 
    {
        size_t n = MIN((size_t)BLOCK_SIZE - inner, remaining);
        if (n == BLOCK_SIZE) 
        {
            if (!write_full_block(start_block, src)) return false;
        } 
        else 
        {
            if (!read_full_block(start_block, block)) return false;
            memcpy(block + inner, src, n);
            if (!write_full_block(start_block, block)) return false;
        }

        src += n;
        remaining -= n;
        start_block++;
        inner = 0;
    }

    return true;
}

void journal_build_metadata_map()
{
    DInode* inode_block = static_cast<DInode*>(aligned_alloc(BLOCK_SIZE, BLOCK_SIZE));
    if (inode_block == nullptr) 
    {
        log_warn("[journal] failed to allocate inode scan buffer");
        return;
    }

    uint64_t scanned = 0;
    for (uint64_t b = 0; b < sb.itable_blocknum; ++b) 
    {
        if (storage_read(inode_block, sb.itable_blockstart + b, 1) != 0) break;

        for (uint32_t i = 0; i < INODENUM_PER_BLOCK && scanned < sb.inode_num; ++i, ++scanned) 
        {
            const DInode& din = inode_block[i];
            if (!din.used) continue;
            const bool is_dir = din.type == DIRECTORY;

            MetadataBlockRegisterCtx meta_reg_ctx = { .skip_root = true };
            disk_inode_for_each_extent_metadata_block(&din, register_metadata_block_cb, &meta_reg_ctx);

            if (is_dir)
                disk_inode_for_each_extent(&din, true, register_dir_data_extent_cb, nullptr);
        }
    }

    free(inode_block);
}

#endif
