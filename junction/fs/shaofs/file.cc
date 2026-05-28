#include "inodeCache.h"
#include "blockCache.h"
#include "fs.h"
#include "file.h"
#include "extent.h"
#include <utility>
#include <sys/uio.h>
#include "group.h"
#include "dsa.h"
#include "journal.h"
#include "profile.h"
#include <cstdint>
#include <cstdlib>
#include <algorithm>
extern "C" {
#include "runtime/runtime.h"
#include "runtime/storage.h"
}

static inline bool user_dma_request_ok(const void* buf, off_t offset, size_t len)
{
    return offset >= 0 && (static_cast<uint64_t>(offset) & (BLOCK_SIZE - 1)) == 0 && (reinterpret_cast<uintptr_t>(buf) & (BLOCK_SIZE - 1)) == 0 && (len & (BLOCK_SIZE - 1)) == 0;
}

static constexpr size_t kFileBatchCopyMin = 64 * 1024;
static constexpr size_t kDirectCleanReadMinBytes = kFileBatchCopyMin;
static constexpr uint32_t kReadBounceSlotBlocks = 256;
static constexpr uint32_t kReadBounceSlotCount = 64;

struct ReadBounceSlot {
    volatile int busy;
    char* data;
};

static ReadBounceSlot read_bounce_slots[kReadBounceSlotCount];
static char* read_bounce_region;
static bool read_bounce_ready;

static uint32_t parse_u32_env(const char* name, uint32_t fallback)
{
    const char* value = getenv(name);
    if (!value || *value == '\0') return fallback;

    char* end = nullptr;
    unsigned long parsed = strtoul(value, &end, 0);
    if (end == value || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) return fallback;
    return static_cast<uint32_t>(parsed);
}

void init_file_io()
{
    if (read_bounce_ready) return;

    uint32_t slots = parse_u32_env("SHAOFS_READ_BOUNCE_SLOTS", kReadBounceSlotCount);
    slots = std::min(slots, kReadBounceSlotCount);
    size_t bytes = static_cast<size_t>(kReadBounceSlotBlocks) * BLOCK_SIZE * slots;

    read_bounce_region = static_cast<char*>(spdk_dma_zmalloc(bytes, BLOCK_SIZE, nullptr));
    if (!read_bounce_region)
    {
        log_warn("[shaofs] read bounce pool allocation failed; buffered reads will use BlockCache path");
        return;
    }

    for (uint32_t i = 0; i < slots; i++)
    {
        read_bounce_slots[i].busy = 0;
        read_bounce_slots[i].data = read_bounce_region + static_cast<size_t>(i) * kReadBounceSlotBlocks * BLOCK_SIZE;
    }
    for (uint32_t i = slots; i < kReadBounceSlotCount; i++)
    {
        read_bounce_slots[i].busy = 1;
        read_bounce_slots[i].data = nullptr;
    }
    read_bounce_ready = true;
    log_info("[shaofs] read bounce pool initialized: slots=%u slot_size=%uKB", slots, (kReadBounceSlotBlocks * BLOCK_SIZE) / 1024);
}

class ReadBounceGuard {
    ReadBounceSlot* slot_;

public:
    explicit ReadBounceGuard(bool enabled) : slot_(nullptr)
    {
        if (unlikely(!enabled || !read_bounce_ready)) return;

        unsigned int cpu;
        {
            kguard k;
            cpu = k->curr_cpu;
        }
        uint32_t start = cpu % kReadBounceSlotCount;
        while (true)
        {
            for (uint32_t i = 0; i < kReadBounceSlotCount; i++)
            {
                ReadBounceSlot* candidate = &read_bounce_slots[(start + i) % kReadBounceSlotCount];
                int expected = 0;
                if (__atomic_compare_exchange_n(&candidate->busy, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                {
                    slot_ = candidate;
                    return;
                }
            }
            thread_yield();
        }
    }

    ~ReadBounceGuard()
    {
        if (slot_) __atomic_store_n(&slot_->busy, 0, __ATOMIC_RELEASE);
    }

    explicit operator bool() const { return slot_ != nullptr; }
    char* data() const { return slot_->data; }
};

static inline void mark_inode_data_cache_dirty(MInode* inode, uint64_t start, uint64_t end)
{
    if (end <= start) return;
    SpinGuardNP g(&inode->dirty_lock);
    if (!atomic_read(&inode->has_dirty_data_cache))
    {
        inode->dirty_data_start = start;
        inode->dirty_data_end = end;
    }
    else
    {
        inode->dirty_data_start = MIN(inode->dirty_data_start, start);
        inode->dirty_data_end = MAX(inode->dirty_data_end, end);
    }
    inode->dirty_data_seq++;
    atomic_write(&inode->has_dirty_data_cache, 1);
}

static inline bool direct_clean_read_worthwhile(uint64_t aligned_clean_len)
{
    return aligned_clean_len >= kDirectCleanReadMinBytes;
}

static inline bool direct_clean_read_allowed(uint64_t aligned_clean_len, BlockID first_phys_blk)
{
    if (direct_clean_read_worthwhile(aligned_clean_len)) return true;
    return first_phys_blk == INVALID_BLOCK_ID || !bc_is_cached_valid(first_phys_blk);
}

static ssize_t file_read_direct_clean(int inum, char* buf, off_t offset, size_t len)
{
    SHAOFS_PROFILE_SCOPE(SHAOFS_PROF_FILE_READ_DIRECT_CLEAN);
    shaofs_profile_record_bytes(SHAOFS_PROF_FILE_READ_DIRECT_CLEAN, len);

    if (len == 0) return 0;
    if (unlikely(offset < 0 || (static_cast<uint64_t>(offset) & (BLOCK_SIZE - 1)) != 0)) return 0;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) return -1;

    auto write_acc = ih.write_access();
    if (unlikely(!write_acc->used)) return -1;
    if (write_acc->type != REGULAR) return 0;
    if (static_cast<uint64_t>(offset) >= write_acc->file_size) return 0;

    uint64_t available = write_acc->file_size - static_cast<uint64_t>(offset);
    uint64_t target_len = MIN(static_cast<uint64_t>(len), available);
    if (atomic_read(&write_acc->has_dirty_data_cache))
    {
        SpinGuardNP dirty_g(&write_acc->dirty_lock);
        if (atomic_read(&write_acc->has_dirty_data_cache))
        {
            if (write_acc->dirty_data_start <= static_cast<uint64_t>(offset)) return 0;
            target_len = MIN(target_len, write_acc->dirty_data_start - static_cast<uint64_t>(offset));
        }
    }
    target_len &= ~(static_cast<uint64_t>(BLOCK_SIZE) - 1);
    if (target_len == 0) return 0;

    iExtent first_ext = {};
    BlockID first_phys_blk = INVALID_BLOCK_ID;
    uint64_t first_logical_blk = static_cast<uint64_t>(offset) / BLOCK_SIZE;
    if (inode_lookup_extent_locked(&*write_acc, first_logical_blk, &first_ext))
        first_phys_blk = first_ext.physical_start + (first_logical_blk - first_ext.logical_start);
    if (!direct_clean_read_allowed(target_len, first_phys_blk)) return 0;

    bool user_dma = user_dma_request_ok(buf, offset, target_len);
    ReadBounceGuard bounce(!user_dma);
    if (!user_dma && !bounce) return 0;

    uint64_t bytes_read = 0;
    while (bytes_read < target_len)
    {
        uint64_t current_offset = static_cast<uint64_t>(offset) + bytes_read;
        uint64_t logical_blk = current_offset / BLOCK_SIZE;
        uint64_t request_blocks = (target_len - bytes_read) / BLOCK_SIZE;

        iExtent ext = {};
        if (!inode_lookup_extent_locked(&*write_acc, logical_blk, &ext))
        {
            memset(buf + bytes_read, 0, BLOCK_SIZE);
            bytes_read += BLOCK_SIZE;
            continue;
        }

        uint64_t extent_offset = logical_blk - ext.logical_start;
        uint64_t run_blocks = MIN(request_blocks, static_cast<uint64_t>(ext.block_count) - extent_offset);
        run_blocks = MIN(run_blocks, static_cast<uint64_t>(kReadBounceSlotBlocks));
        BlockID run_start = ext.physical_start + extent_offset;
        size_t run_bytes = run_blocks * BLOCK_SIZE;

        char* dst = buf + bytes_read;
        if (user_dma)
        {
            if (storage_read_aligned(dst, run_start, run_blocks) != 0)
            {
                if (bytes_read == 0) return 0;
                break;
            }
        }
        else
        {
            if (storage_read_aligned(bounce.data(), run_start, run_blocks) != 0)
            {
                if (bytes_read == 0) return 0;
                break;
            }
            dsa_copy_ex(dst, bounce.data(), run_bytes, SHAOFS_DSA_READ_TO_USER);
        }
        bytes_read += run_bytes;
    }

    shaofs_profile_record_blocks(SHAOFS_PROF_FILE_READ_DIRECT_CLEAN, bytes_read / BLOCK_SIZE);
    return bytes_read;
}

// 释放 inode 持有的所有数据块（direct + indirect extents）。调用前必须持有 inode 写锁。
static void free_inode_data_blocks(MInode* inode_ptr)
{
    auto invalidate_data_extent = [](const iExtent& ext, void*) -> bool {
        for (uint64_t i = 0; i < ext.block_count; i++)
            bc_invalidate_block(ext.physical_start + i);
        return true;
    };
    if (!inode_for_each_extent(inode_ptr, true, invalidate_data_extent, nullptr)) log_err("[free_inode_data_blocks] failed to invalidate cached extents for inode %d", inode_ptr->idx);

    auto free_data_extent = [](const iExtent& ext, void*) -> bool {
        free_extent(&ext);
        return true;
    };
    if (!inode_for_each_extent(inode_ptr, true, free_data_extent, nullptr)) log_err("[free_inode_data_blocks] failed to walk extents for inode %d", inode_ptr->idx);
    if (!inode_free_extent_metadata(inode_ptr)) log_err("[free_inode_data_blocks] failed to free extent metadata for inode %d", inode_ptr->idx);

    // 清空 inode 的 extent 元数据
    memset(inode_ptr->direct_extents, 0, sizeof(inode_ptr->direct_extents));
    inode_ptr->file_size = 0;
    inode_ptr->valid_extent_count = 0;
    mark_inode_metadata_dirty(inode_ptr);
    memset(&inode_ptr->extent_hint, 0, sizeof(inode_ptr->extent_hint));  // 清空 extent hint
    {
        SpinGuardNP g(&inode_ptr->dirty_lock);
        inode_ptr->clear_dirty_data_unlocked();
        inode_ptr->dirty_data_seq++;
    }
}

void truncate_inode(int inum)
{
    InodeHandle ih = ic_get_inode(inum);
    if (!ih) return;

    auto write_acc = ih.write_access();
    if (!write_acc->used || write_acc->file_size == 0) return;

    free_inode_data_blocks(&*write_acc);
    write_acc.mark_dirty();
}

static void flush_all_dirty_state(bool stop_writeback)
{
    if (stop_writeback) bc_stop_writeback_and_drain();
    else bc_drain_writeback();
    journal_drain_checkpoint();
    journal_write_metadata(imap, BITMAP_LONG_SIZE(sb.inode_num) * sizeof(unsigned long), sb.imap_blockstart, 0);
	sync_all_gdt();
	ic_flush_all();
    bc_flush_all();
    journal_drain_checkpoint();
}

void shaofs_sync_all()
{
    RuntimeFSBaseGuard g;
    flush_all_dirty_state(false);
}

void final_flush()
{
#if IO_PREEMPT
	atomic64_write(&runtime_info->spdk_uipi, 0);  // 停止让 IOKernel 检查 SPDK 完成情况
	barrier();
#endif

    RuntimeFSBaseGuard g;
    flush_all_dirty_state(true);
    dsa_dump_stats();
    shaofs_profile_dump();
    journal_mark_clean();
}

static inline void append_segment(Segment* vecs, size_t* vec_nr, size_t vec_cap, const Segment& seg)
{
    if (seg.len == 0) return;
    if (*vec_nr > 0)
    {
        Segment& last = vecs[*vec_nr - 1];
        char* last_dst_end = static_cast<char*>(last.dst) + last.len;
        const char* last_src_end = static_cast<const char*>(last.src) + last.len;
        if (last_dst_end == seg.dst && last_src_end == seg.src) { last.len += seg.len; return; }
    }
    if (unlikely(*vec_nr >= vec_cap)) return;
    vecs[(*vec_nr)++] = seg;
}

static ssize_t file_read_blockwise(int inum, char* buf, off_t offset, size_t len)
{   
    if (len == 0) return 0;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) 
    {
        log_err("[file_read] Failed to get inode %d from cache", inum);
        return -1;
    }

    uint64_t bytes_read = 0;
    while (bytes_read < len)
    {
        uint64_t current_offset = offset + bytes_read;
        uint64_t logical_blk = current_offset / BLOCK_SIZE, blk_offset = current_offset % BLOCK_SIZE;
        
        uint64_t copy_len = 0;

        {
            auto read_acc = ih.read_access();     // 获取 Inode【共享读锁】，多线程可并发读！
            if (!read_acc->used || current_offset >= read_acc->file_size) break;   // 文件是否已被逻辑删除，或读取起点是否已超越实时文件大小，直接跳出，copy_len 保持为 0

            // 动态计算本次循环真正安全的读取长度（防御并发写导致的文件大小抖动）
            uint64_t actual_remain = read_acc->file_size - current_offset, request_remain = len - bytes_read;
            copy_len = MIN(BLOCK_SIZE - blk_offset, MIN(request_remain, actual_remain));
            if (copy_len == 0) break;   // 正常抵达 EOF 或文件被并发删除，跳出主循环

            BlockID phys_blk = inode_bmap_locked(const_cast<MInode*>(&(*read_acc)), logical_blk, false, nullptr);  // 直接调用 bmap，内部可能会需要读取间接块，发生 IO 阻塞，当前线程会带锁休眠（不影响其他 reader）

            if (phys_blk == INVALID_BLOCK_ID) 
                memset(buf + bytes_read, 0, copy_len);  // 处理稀疏文件 (Sparse File)：读取遇到空洞，不触发任何磁盘 I/O，直接在内存补 0
            else 
            {
                BlockHandle bh = bc_get_handle(phys_blk);
                if (unlikely(!bh)) 
                {
                    log_err("[file_read] Failed to read physical block %lu", phys_blk);
                    break; // 底层 I/O 硬件级错误，返回目前已成功读取的字节数
                }

                {
                    auto block_read_acc = bh.read_access();     // 获取这一个物理块的共享读锁
                    // dsa_copy(buf + bytes_read, block_read_acc->data + blk_offset, copy_len);
                    memcpy(buf + bytes_read, block_read_acc->data + blk_offset, copy_len);
                }  // 自动释放物理块读锁
            }
        }

        if (copy_len == 0) break;
        bytes_read += copy_len;
    }
    
    return bytes_read;
}

static ssize_t file_read_batch(int inum, char* buf, off_t offset, size_t len)
{
    SHAOFS_PROFILE_SCOPE(SHAOFS_PROF_FILE_READ_BATCH);
    shaofs_profile_record_bytes(SHAOFS_PROF_FILE_READ_BATCH, len);
    if (len == 0) return 0;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih))
    {
        log_err("[file_read] Failed to get inode %d from cache", inum);
        return -1;
    }

    const size_t max_blocks = dsa_batch_task_num;
    BlockHandle handles[dsa_batch_task_num];
    CacheEntry<BlockID, BlockData>* entries[dsa_batch_task_num];
    Segment vecs[dsa_batch_task_num];
    
    uint64_t bytes_read = 0;
    while (bytes_read < len)
    {
        uint64_t batch_bytes = 0;
        size_t handle_nr = 0;
        size_t lock_nr = 0;
        size_t vec_nr = 0;

        auto release_locked_blocks = [&]() {
            for (size_t i = lock_nr; i > 0; i--)
                rwmutex_unlock(&entries[i - 1]->rw_mtx);
            for (size_t i = 0; i < handle_nr; i++)
                handles[i] = BlockHandle();
            lock_nr = 0;
            handle_nr = 0;
        };

        {
            auto read_acc = ih.read_access();
            if (!read_acc->used || offset + bytes_read >= read_acc->file_size) break;

            for (size_t blocks = 0; blocks < max_blocks && bytes_read + batch_bytes < len; blocks++)
            {
                uint64_t current_offset = offset + bytes_read + batch_bytes;
                if (current_offset >= read_acc->file_size) break;

                uint64_t logical_blk = current_offset / BLOCK_SIZE, blk_offset  = current_offset % BLOCK_SIZE;
                uint64_t actual_remain = read_acc->file_size - current_offset;
                uint64_t request_remain = len - bytes_read - batch_bytes;
                uint64_t copy_len = MIN(BLOCK_SIZE - blk_offset, MIN(request_remain, actual_remain));
                if (copy_len == 0) break;

                BlockID phys_blk = inode_bmap_locked(const_cast<MInode*>(&(*read_acc)), logical_blk, false, nullptr);
                if (phys_blk == INVALID_BLOCK_ID)
                {
                    memset(buf + bytes_read + batch_bytes, 0, copy_len);
                    batch_bytes += copy_len;
                    continue;
                }

                BlockHandle bh = bc_get_handle(phys_blk);
                if (unlikely(!bh))
                {
                    log_err("[file_read] Failed to read physical block %lu", phys_blk);
                    release_locked_blocks();
                    return bytes_read;
                }

                handles[handle_nr] = std::move(bh);
                entries[handle_nr] = handles[handle_nr].get_entry();
                rwmutex_rdlock(&entries[handle_nr]->rw_mtx);
                lock_nr++;
                append_segment(vecs, &vec_nr, dsa_batch_task_num, {buf + bytes_read + batch_bytes, entries[handle_nr]->data.data + blk_offset, copy_len});
                handle_nr++;
                batch_bytes += copy_len;
            }

            if (vec_nr != 0) dsa_copyv_ex(vecs, vec_nr, SHAOFS_DSA_READ_TO_USER);
            release_locked_blocks();
        }

        if (batch_bytes == 0) break;
        bytes_read += batch_bytes;
    }

    return bytes_read;
}

ssize_t file_read(int inum, char* buf, off_t offset, size_t len)
{
    if (len >= kFileBatchCopyMin)
    {
        ssize_t direct = file_read_direct_clean(inum, buf, offset, len);
        if (direct < 0 || len == 0) return direct;
        if (direct > 0)
        {
            if (static_cast<size_t>(direct) == len) return direct;
            ssize_t tail = file_read_batch(inum, buf + direct, offset + direct, len - direct);
            if (tail < 0) return direct;
            return direct + tail;
        }
        return file_read_batch(inum, buf, offset, len);
    }
    return file_read_blockwise(inum, buf, offset, len);
}

static ssize_t file_write_blockwise(int inum, const char* buf, off_t offset, size_t len)
{
    if (len == 0) return 0;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) 
    {
        log_err("[file_write] Failed to get inode %d from cache", inum);
        return -1;
    }

    uint64_t bytes_written = 0;
    while (bytes_written < len)
    {
        uint64_t current_offset = offset + bytes_written;
        uint64_t logical_blk = current_offset / BLOCK_SIZE, blk_offset = current_offset % BLOCK_SIZE;        
        uint64_t copy_len = MIN(BLOCK_SIZE - blk_offset, len - bytes_written);

        BlockID phys_blk = INVALID_BLOCK_ID;
        bool fast_path_success = false;

        // 判断能否走 Fast Path
        {
            auto read_acc = ih.read_access(); // 获取 Inode 共享读锁
            if (!read_acc->used) 
            {
                log_err("[file_write] Inode %d is not in use", inum);
                break;
            }

            if (current_offset + copy_len <= read_acc->file_size)  // 只有当写入范围完全在现有 file_size 内部时，才尝试 Fast Path
            {
                phys_blk = inode_bmap_locked(const_cast<MInode*>(&(*read_acc)), logical_blk, false, nullptr);
                if (phys_blk != INVALID_BLOCK_ID)  // 如果 phys_blk 有效，说明不是稀疏文件空洞，可以走 Fast Path
                {
                    BlockHandle bh = bc_get_handle(phys_blk);   
                    if (unlikely(!bh)) 
                    {
                        log_err("[file_write] Fast path failed to get cache handle");
                        break;
                    }

                    auto block_write_acc = bh.write_access(); // 获取 Block 独占写锁保证单块安全
                    dsa_copy_ex(block_write_acc->data + blk_offset, buf + bytes_written, copy_len, SHAOFS_DSA_WRITE_FROM_USER);
                    bc_mark_block_dirty(bh);
                    mark_inode_data_cache_dirty(const_cast<MInode*>(&(*read_acc)), current_offset, current_offset + copy_len);

                    bytes_written += copy_len;
                    fast_path_success = true;
                }
            }
        } // Inode 共享读锁释放
        if (fast_path_success) continue;

        // Slow Path: 需要分配新块和更新 Inode 元数据，持有 Inode 写锁
        bool is_new_block = false;

        {
            auto write_acc = ih.write_access();
            if (!write_acc->used) 
            {
                log_err("[file_write] Inode %d is not in use", inum);
                break;
            }

            phys_blk = inode_bmap_locked(&*write_acc, logical_blk, true, &is_new_block);
            if (phys_blk == INVALID_BLOCK_ID)
            {
                log_err("[file_write] Disk full or bmap failed at logical block %lu", logical_blk);
                break;
            }

            BlockHandle bh = is_new_block ? bc_get_handle(phys_blk, false) : bc_get_handle(phys_blk);
            if (unlikely(!bh))
            {
                log_err("[file_write] Failed to get cache handle for physical block %lu", phys_blk);
                break;
            }
            if (write_acc->type == DIRECTORY) journal_register_metadata_block(phys_blk);

            {
                auto block_write_acc = bh.write_access();
                if (is_new_block && copy_len < BLOCK_SIZE) memset(block_write_acc->data, 0, BLOCK_SIZE);   // 新分配的块若未写满，必须填 0
                dsa_copy_ex(block_write_acc->data + blk_offset, buf + bytes_written, copy_len, SHAOFS_DSA_WRITE_FROM_USER);
                atomic_write(&bh.get_entry()->valid, 1);
                bc_mark_block_dirty(bh);
                mark_inode_data_cache_dirty(&*write_acc, current_offset, current_offset + copy_len);
            }

            uint64_t new_end_pos = current_offset + copy_len;
            if (new_end_pos > write_acc->file_size)
            {
                write_acc->file_size = new_end_pos;
                mark_inode_metadata_dirty(&*write_acc);
            }
            write_acc.mark_dirty();
        } // inode 写锁释放

        bytes_written += copy_len;
    }

    return (bytes_written == 0 && len > 0) ? -1 : bytes_written;
}

static ssize_t file_write_scalar_locked(MInode* inode, const char* buf, uint64_t offset, size_t len)
{
    uint64_t bytes_written = 0;
    while (bytes_written < len)
    {
        uint64_t current_offset = offset + bytes_written;
        uint64_t logical_blk = current_offset / BLOCK_SIZE;
        uint64_t blk_offset = current_offset % BLOCK_SIZE;
        uint64_t copy_len = MIN(BLOCK_SIZE - blk_offset, len - bytes_written);
        bool is_new_block = false;

        BlockID phys_blk = inode_bmap_locked(inode, logical_blk, true, &is_new_block);
        if (phys_blk == INVALID_BLOCK_ID)
        {
            log_err("[file_write] Disk full or bmap failed at logical block %lu", logical_blk);
            break;
        }

        BlockHandle bh = is_new_block ? bc_get_handle(phys_blk, false) : bc_get_handle(phys_blk);
        if (unlikely(!bh))
        {
            log_err("[file_write] Failed to get cache handle for physical block %lu", phys_blk);
            break;
        }
        if (inode->type == DIRECTORY) journal_register_metadata_block(phys_blk);

        {
            auto block_write_acc = bh.write_access();
            if (is_new_block && copy_len < BLOCK_SIZE) memset(block_write_acc->data, 0, BLOCK_SIZE);
            dsa_copy_ex(block_write_acc->data + blk_offset, buf + bytes_written, copy_len, SHAOFS_DSA_WRITE_FROM_USER);
            atomic_write(&bh.get_entry()->valid, 1);
            bc_mark_block_dirty(bh);
            mark_inode_data_cache_dirty(inode, current_offset, current_offset + copy_len);
        }

        uint64_t new_end_pos = current_offset + copy_len;
        if (new_end_pos > inode->file_size)
        {
            inode->file_size = new_end_pos;
            mark_inode_metadata_dirty(inode);
        }
        bytes_written += copy_len;
    }

    return bytes_written;
}

static ssize_t file_write_batch_new_blocks_locked(MInode* inode, const char* buf, uint64_t offset, size_t len)
{
    SHAOFS_PROFILE_SCOPE(SHAOFS_PROF_FILE_WRITE_NEW_BLOCK_BATCH);
    shaofs_profile_record_bytes(SHAOFS_PROF_FILE_WRITE_NEW_BLOCK_BATCH, len);
    if (inode->type != REGULAR || offset != inode->file_size || (offset & (BLOCK_SIZE - 1)) != 0) return 0;

    size_t full_blocks = len / BLOCK_SIZE;
    if (full_blocks == 0) return 0;
    size_t batch_blocks = MIN(full_blocks, (size_t)dsa_batch_task_num);
    size_t batch_len = batch_blocks * BLOCK_SIZE;
    if (batch_len < kFileBatchCopyMin) return 0;

    BlockID blocks[dsa_batch_task_num];
    BlockHandle handles[dsa_batch_task_num];
    CacheEntry<BlockID, BlockData>* entries[dsa_batch_task_num];
    Segment vecs[dsa_batch_task_num];
    size_t handle_nr = 0;
    size_t lock_nr = 0;
    size_t vec_nr = 0;

    auto release_locked_blocks = [&]() {
        for (size_t i = lock_nr; i > 0; i--)
            rwmutex_unlock(&entries[i - 1]->rw_mtx);
        for (size_t i = 0; i < handle_nr; i++)
            handles[i] = BlockHandle();
        lock_nr = 0;
        handle_nr = 0;
    };

    int mapped_blocks = inode_append_run_locked(inode, offset / BLOCK_SIZE, (int)batch_blocks, blocks);
    if (mapped_blocks <= 0) return 0;
    batch_blocks = (size_t)mapped_blocks;
    batch_len = batch_blocks * BLOCK_SIZE;

    for (size_t i = 0; i < batch_blocks; i++)
    {
        BlockHandle bh = bc_get_handle(blocks[i], false);
        if (unlikely(!bh))
        {
            release_locked_blocks();
            return 0;
        }

        handles[handle_nr] = std::move(bh);
        entries[handle_nr] = handles[handle_nr].get_entry();
        rwmutex_wrlock(&entries[handle_nr]->rw_mtx);
        lock_nr++;
        vecs[vec_nr++] = {entries[handle_nr]->data.data, buf + i * BLOCK_SIZE, BLOCK_SIZE};
        handle_nr++;
    }

    dsa_copyv_ex(vecs, vec_nr, SHAOFS_DSA_WRITE_FROM_USER);

    for (size_t i = 0; i < handle_nr; i++)
    {
        atomic_write(&handles[i].get_entry()->valid, 1);
        bc_mark_data_block_dirty(handles[i]);
    }
    release_locked_blocks();
    mark_inode_data_cache_dirty(inode, offset, offset + batch_len);
    inode->file_size = offset + batch_len;
    mark_inode_metadata_dirty(inode);
    shaofs_profile_record_blocks(SHAOFS_PROF_FILE_WRITE_NEW_BLOCK_BATCH, batch_blocks);
    return batch_len;
}

ssize_t file_write_append(int inum, const char* buf, size_t len, off_t* new_off)
{
    SHAOFS_PROFILE_SCOPE(SHAOFS_PROF_FILE_WRITE_APPEND);
    shaofs_profile_record_bytes(SHAOFS_PROF_FILE_WRITE_APPEND, len);
    if (len == 0) return 0;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih))
    {
        log_err("[file_write_append] Failed to get inode %d from cache", inum);
        return -1;
    }

    auto write_acc = ih.write_access();
    if (!write_acc->used)
    {
        log_err("[file_write_append] Inode %d is not in use", inum);
        return -1;
    }

    uint64_t bytes_written = 0;
    uint64_t start = write_acc->file_size;
    while (bytes_written < len)
    {
        uint64_t current_offset = write_acc->file_size;
        ssize_t ret = 0;

        if (write_acc->type == REGULAR && (current_offset & (BLOCK_SIZE - 1)) == 0 && len - bytes_written >= kFileBatchCopyMin)
            ret = file_write_batch_new_blocks_locked(&*write_acc, buf + bytes_written, current_offset, len - bytes_written);

        if (ret == 0) ret = file_write_scalar_locked(&*write_acc, buf + bytes_written, current_offset, len - bytes_written);
        if (ret <= 0) break;
        bytes_written += ret;
        write_acc.mark_dirty();
    }

    if (new_off) *new_off = start + bytes_written;
    return (bytes_written == 0 && len > 0) ? -1 : bytes_written;
}

static ssize_t file_write_eof_extension(int inum, const char* buf, off_t offset, size_t len)
{
    SHAOFS_PROFILE_SCOPE(SHAOFS_PROF_FILE_WRITE_EOF_EXTENSION);
    shaofs_profile_record_bytes(SHAOFS_PROF_FILE_WRITE_EOF_EXTENSION, len);
    if (len == 0) return 0;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih))
    {
        log_err("[file_write] Failed to get inode %d from cache", inum);
        return -1;
    }

    auto write_acc = ih.write_access();
    if (!write_acc->used)
    {
        log_err("[file_write] Inode %d is not in use", inum);
        return -1;
    }
    if (write_acc->type != REGULAR) return 0;

    uint64_t bytes_written = 0;
    while (bytes_written < len)
    {
        uint64_t current_offset = static_cast<uint64_t>(offset) + bytes_written;
        if (current_offset != write_acc->file_size) break;

        ssize_t ret = 0;
        if ((current_offset & (BLOCK_SIZE - 1)) == 0 && len - bytes_written >= kFileBatchCopyMin)
            ret = file_write_batch_new_blocks_locked(&*write_acc, buf + bytes_written, current_offset, len - bytes_written);

        if (ret == 0) ret = file_write_scalar_locked(&*write_acc, buf + bytes_written, current_offset, len - bytes_written);
        if (ret <= 0) break;
        bytes_written += ret;
        write_acc.mark_dirty();
    }

    return bytes_written;
}

static ssize_t file_write_batch_existing(int inum, const char* buf, off_t offset, size_t len)
{
    SHAOFS_PROFILE_SCOPE(SHAOFS_PROF_FILE_WRITE_BATCH_EXISTING);
    shaofs_profile_record_bytes(SHAOFS_PROF_FILE_WRITE_BATCH_EXISTING, len);
    if (len == 0) return 0;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih))
    {
        log_err("[file_write] Failed to get inode %d from cache", inum);
        return -1;
    }

    const size_t max_blocks = dsa_batch_task_num;
    BlockHandle handles[dsa_batch_task_num];
    CacheEntry<BlockID, BlockData>* entries[dsa_batch_task_num];
    Segment vecs[dsa_batch_task_num];

    uint64_t bytes_written = 0;
    while (bytes_written < len)
    {
        uint64_t batch_bytes = 0;
        bool stop_batching = false;
        size_t handle_nr = 0;
        size_t lock_nr = 0;
        size_t vec_nr = 0;

        auto release_locked_blocks = [&]() {
            for (size_t i = lock_nr; i > 0; i--)
                rwmutex_unlock(&entries[i - 1]->rw_mtx);
            for (size_t i = 0; i < handle_nr; i++)
                handles[i] = BlockHandle();
            lock_nr = 0;
            handle_nr = 0;
        };

        {
            auto read_acc = ih.read_access();
            if (!read_acc->used)
            {
                log_err("[file_write] Inode %d is not in use", inum);
                return bytes_written > 0 ? bytes_written : -1;
            }

            for (size_t blocks = 0; blocks < max_blocks && bytes_written + batch_bytes < len; blocks++)
            {
                uint64_t current_offset = offset + bytes_written + batch_bytes;
                uint64_t logical_blk = current_offset / BLOCK_SIZE, blk_offset  = current_offset % BLOCK_SIZE;
                uint64_t copy_len = MIN(BLOCK_SIZE - blk_offset, len - bytes_written - batch_bytes);
                if (copy_len == 0) break;

                if (current_offset + copy_len > read_acc->file_size)
                {
                    stop_batching = true;
                    break;
                }

                BlockID phys_blk = inode_bmap_locked(const_cast<MInode*>(&(*read_acc)), logical_blk, false, nullptr);
                if (phys_blk == INVALID_BLOCK_ID)
                {
                    stop_batching = true;
                    break;
                }

                BlockHandle bh = bc_get_handle(phys_blk);
                if (unlikely(!bh))
                {
                    log_err("[file_write] Fast path failed to get cache handle");
                    release_locked_blocks();
                    return bytes_written;
                }

                handles[handle_nr] = std::move(bh);
                entries[handle_nr] = handles[handle_nr].get_entry();
                rwmutex_wrlock(&entries[handle_nr]->rw_mtx);
                lock_nr++;
                append_segment(vecs, &vec_nr, dsa_batch_task_num, {entries[handle_nr]->data.data + blk_offset, buf + bytes_written + batch_bytes, copy_len});
                handle_nr++;
                batch_bytes += copy_len;
            }

            if (vec_nr != 0)
            {
                dsa_copyv_ex(vecs, vec_nr, SHAOFS_DSA_WRITE_FROM_USER);
                for (size_t i = 0; i < handle_nr; i++)
                    bc_mark_data_block_dirty(handles[i]);
                mark_inode_data_cache_dirty(const_cast<MInode*>(&(*read_acc)), offset + bytes_written, offset + bytes_written + batch_bytes);
            }
            release_locked_blocks();
        }

        bytes_written += batch_bytes;
        if (batch_bytes == 0 || stop_batching) break;
    }

    return bytes_written;
}

ssize_t file_write(int inum, const char* buf, off_t offset, size_t len)
{
    if (len < kFileBatchCopyMin) return file_write_blockwise(inum, buf, offset, len);

    ssize_t batch_written = file_write_batch_existing(inum, buf, offset, len);
    if (batch_written < 0) return batch_written;
    if (static_cast<size_t>(batch_written) == len) return batch_written;

    ssize_t eof_written = file_write_eof_extension(inum, buf + batch_written, offset + batch_written, len - batch_written);
    if (eof_written < 0) return batch_written > 0 ? batch_written : eof_written;
    if (eof_written > 0)
    {
        batch_written += eof_written;
        if (static_cast<size_t>(batch_written) == len) return batch_written;
    }

    ssize_t rest = file_write_blockwise(inum, buf + batch_written, offset + batch_written, len - batch_written);
    if (rest < 0) return batch_written > 0 ? batch_written : rest;
    return batch_written + rest;
}


/* O_DIRECT I/O */
ssize_t file_read_direct(int inum, char* buf, off_t offset, size_t len)
{
    if (len == 0) return 0;
    if (!user_dma_request_ok(buf, offset, len)) return -EINVAL;   // O_DIRECT 使用严格的用户 buffer DMA 合约；不满足条件或注册失败时直接返回错误。

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) return -1;

    uint64_t bytes_read = 0;
    while (bytes_read < len)
    {
        uint64_t current_offset = offset + bytes_read;
        uint64_t logical_blk = current_offset / BLOCK_SIZE, blk_offset  = current_offset % BLOCK_SIZE;

        uint64_t copy_len = 0;
        BlockID phys_blk = INVALID_BLOCK_ID;

        {
            auto read_acc = ih.read_access();
            if (!read_acc->used || current_offset >= read_acc->file_size) break;

            uint64_t actual_remain = read_acc->file_size - current_offset;
            uint64_t request_remain = len - bytes_read;
            copy_len = MIN(BLOCK_SIZE - blk_offset, MIN(request_remain, actual_remain));
            if (copy_len == 0) break;
            if (blk_offset != 0 || (copy_len % BLOCK_SIZE) != 0) break;

            phys_blk = inode_bmap_locked(const_cast<MInode*>(&(*read_acc)), logical_blk, false, nullptr);
        }

        if (phys_blk == INVALID_BLOCK_ID)
        {
            memset(buf + bytes_read, 0, copy_len);  // 处理文件空洞 (Hole)：直接将对应的用户 Buffer 填 0，无需下发 I/O
        }
        else
        {
            char* dst = buf + bytes_read;
            if (atomic_read(&ih.get_entry()->data.has_dirty_data_cache) && !bc_flush_block(phys_blk)) break;  // 若该块在 cache 中且为脏，先刷盘保证 Direct I/O 能读到最新数据

            if (storage_read_aligned(dst, phys_blk, copy_len / BLOCK_SIZE) != 0) break;
        }

        bytes_read += copy_len;
    }

    return bytes_read;
}

bool file_prepare_direct_read_hint(int inum, DirectReadHint* hint)
{
    if (!hint) return false;
    RuntimeFSBaseGuard g;

    hint->valid = false;
    hint->extent_count = 0;
    hint->file_size = 0;
    hint->has_dirty_data_cache = nullptr;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) return false;

    auto read_acc = ih.read_access();
    if (!read_acc->used || read_acc->valid_extent_count == 0 || read_acc->valid_extent_count > DIRECT_READ_HINT_MAX_EXTENTS) return false;

    uint32_t hint_count = 0;
    uint32_t direct_count = direct_extent_count(&*read_acc);
    for (uint32_t i = 0; i < direct_count; i++)
        hint->extents[hint_count++] = read_acc->direct_extents[i];

    if (uses_extent_tree(&*read_acc)) return false;

    if (uses_indirect_block(&*read_acc))
    {
        BlockHandle ind_bh = bc_get_handle(read_acc->indirect_extent_block);
        if (!ind_bh) return false;

        auto ind_acc = ind_bh.read_access();
        const iExtent* ind_exts = reinterpret_cast<const iExtent*>(ind_acc->data);
        uint32_t indirect_count = legacy_indirect_extent_count(&*read_acc);
        for (uint32_t i = 0; i < indirect_count; i++)
            hint->extents[hint_count++] = ind_exts[i];
    }

    if (hint_count != read_acc->valid_extent_count) return false;

    hint->file_size = read_acc->file_size;
    hint->extent_count = hint_count;
    hint->has_dirty_data_cache = &ih.get_entry()->data.has_dirty_data_cache;
    hint->valid = true;
    return true;
}

ssize_t file_read_direct_hint(const DirectReadHint* hint, char* buf, off_t offset, size_t len)
{
    if (!hint || !hint->valid) return -1;
    if (len == 0) return 0;
    if (!user_dma_request_ok(buf, offset, len)) return -EINVAL;
    if (static_cast<uint64_t>(offset) >= hint->file_size) return 0;

    RuntimeFSBaseGuard g;

    uint64_t bytes_read = 0;
    while (bytes_read < len)
    {
        uint64_t current_offset = offset + bytes_read;
        uint64_t logical_blk = current_offset / BLOCK_SIZE, blk_offset = current_offset % BLOCK_SIZE;
        uint64_t actual_remain = hint->file_size - current_offset;
        uint64_t request_remain = len - bytes_read;
        if (blk_offset != 0) break;

        iExtent ext = {};
        for (uint32_t i = 0; i < hint->extent_count; i++)
        {
            if (block_in_extent(logical_blk, hint->extents[i]))
            {
                ext = hint->extents[i];
                break;
            }
        }

        uint64_t copy_len;
        BlockID phys_blk;
        if (ext.block_count == 0)
        {
            copy_len = MIN(BLOCK_SIZE, MIN(request_remain, actual_remain));
            memset(buf + bytes_read, 0, copy_len);
            bytes_read += copy_len;
            continue;
        }

        uint64_t extent_blocks = ext.block_count - (logical_blk - ext.logical_start);
        uint64_t max_blocks = MIN(extent_blocks, MIN(request_remain, actual_remain) / BLOCK_SIZE);
        if (max_blocks == 0) break;
        copy_len = max_blocks * BLOCK_SIZE;
        phys_blk = ext.physical_start + (logical_blk - ext.logical_start);

        char* dst = buf + bytes_read;
        if (atomic_read(hint->has_dirty_data_cache))
        {
            bool flush_ok = true;
            for (uint64_t i = 0; i < max_blocks; i++)
            {
                if (!bc_flush_block(phys_blk + i))
                {
                    flush_ok = false;
                    break;
                }
            }
            if (!flush_ok) break;
        }

        if (storage_read_aligned(dst, phys_blk, copy_len / BLOCK_SIZE) != 0) break;
        bytes_read += copy_len;
    }

    return bytes_read;
}

static ssize_t file_readv_direct_scalar(int inum, const struct iovec* iov, int iovcnt, off_t offset)
{
    uint64_t total_len = 0;
    off_t cursor = offset;

    for (int i = 0; i < iovcnt; i++)
    {
        if (iov[i].iov_len == 0) continue;

        ssize_t ret = file_read_direct(inum, static_cast<char*>(iov[i].iov_base), cursor, iov[i].iov_len);
        if (ret < 0) return total_len ? static_cast<ssize_t>(total_len) : ret;

        total_len += ret;
        cursor += ret;
        if (static_cast<size_t>(ret) < iov[i].iov_len) break;
    }

    return total_len;
}

ssize_t file_readv_direct(int inum, const struct iovec* iov, int iovcnt, off_t offset)
{
    if (!iov || iovcnt <= 0 || offset < 0) return -EINVAL;

    RuntimeFSBaseGuard g;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) return -1;

    static constexpr size_t kDirectReadvBatch = 64;
    storage_batch_read reqs[kDirectReadvBatch];
    size_t req_nr = 0;
    uint64_t total_len = 0;
    uint64_t cursor = offset;
    bool scalar_fallback = false;

    {
        auto read_acc = ih.read_access();
        if (!read_acc->used || static_cast<uint64_t>(offset) >= read_acc->file_size) return 0;

        if (atomic_read(&ih.get_entry()->data.has_dirty_data_cache))
        {
            scalar_fallback = true;
        }
        else
        {
            for (int i = 0; i < iovcnt; i++)
            {
                if (iov[i].iov_len == 0) continue;
                if (!user_dma_request_ok(iov[i].iov_base, cursor, iov[i].iov_len)) return total_len ? static_cast<ssize_t>(total_len) : -EINVAL;
                if (cursor >= read_acc->file_size) break;

                uint64_t req_len = MIN(static_cast<uint64_t>(iov[i].iov_len), read_acc->file_size - cursor);
                if ((req_len & (BLOCK_SIZE - 1)) != 0) break;

                uint64_t logical_blk = cursor / BLOCK_SIZE;
                BlockID phys_blk = inode_bmap_locked(const_cast<MInode*>(&(*read_acc)), logical_blk, false, nullptr);
                if (req_nr == kDirectReadvBatch) break;
                if (phys_blk == INVALID_BLOCK_ID)
                {
                    scalar_fallback = total_len == 0;
                    break;
                }

                reqs[req_nr++] = {iov[i].iov_base, phys_blk, static_cast<uint32_t>(req_len / BLOCK_SIZE)};
                total_len += req_len;
                cursor += req_len;
            }
        }
    }

    if (scalar_fallback) return file_readv_direct_scalar(inum, iov, iovcnt, offset);
    if (req_nr == 0) return file_readv_direct_scalar(inum, iov, iovcnt, offset);
    
    if (storage_read_aligned_batch(reqs, req_nr) != 0) return -EIO;
    return total_len;
}

ssize_t file_write_direct(int inum, const char* buf, off_t offset, size_t len)
{
    if (len == 0) return 0;
    if (!user_dma_request_ok(buf, offset, len)) return -EINVAL;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) 
    {
        log_err("[file_write_direct] Failed to get inode %d from cache", inum);
        return -1;
    }

    uint64_t bytes_written = 0;
    while (bytes_written < len)
    {
        uint64_t current_offset = offset + bytes_written;
        uint64_t logical_blk = current_offset / BLOCK_SIZE, blk_offset  = current_offset % BLOCK_SIZE;
        uint64_t copy_len = MIN(BLOCK_SIZE - blk_offset, len - bytes_written);
        if (blk_offset != 0 || (copy_len % BLOCK_SIZE) != 0) break;

        BlockID phys_blk = INVALID_BLOCK_ID;
        bool use_fast_path = false;

        // 判断能否走 Fast Path（直接写入已分配的物理块，无需持有 Inode 写锁）
        {
            auto read_acc = ih.read_access();
            if (!read_acc->used) 
            {
                log_err("[file_write_direct] Inode %d is not in use", inum);
                break;
            }

            if (current_offset + copy_len <= read_acc->file_size)
            {
                phys_blk = inode_bmap_locked(const_cast<MInode*>(&(*read_acc)), logical_blk, false, nullptr);
                if (phys_blk != INVALID_BLOCK_ID) 
                {
                    bc_flush_block(phys_blk);

                    if (storage_write_user_dma(buf + bytes_written, phys_blk, copy_len / BLOCK_SIZE) != 0) 
                    {
                        log_err("[file_write_direct] Failed to write to physical block %lu", phys_blk);
                        break;
                    }
                    bc_invalidate_block(phys_blk);
                    use_fast_path = true;
                }
            }
        }
        if (use_fast_path)
        {
            bytes_written += copy_len;
            continue;
        }

        // Slow Path: 需要分配新块和更新 Inode 元数据，持有 Inode 写锁
        bool is_new_block = false;

        {
            auto write_acc = ih.write_access();
            if (!write_acc->used) 
            {
                log_err("[file_write_direct] Inode %d is not in use", inum);
                break;
            }

            phys_blk = inode_bmap_locked(&*write_acc, logical_blk, true, &is_new_block);
            if (phys_blk == INVALID_BLOCK_ID) break;
            if (write_acc->type == DIRECTORY) journal_register_metadata_block(phys_blk);

            bc_flush_block(phys_blk);

            if (storage_write_user_dma(buf + bytes_written, phys_blk, copy_len / BLOCK_SIZE) != 0) break;
            bc_invalidate_block(phys_blk);

            uint64_t new_end_pos = current_offset + copy_len;
            if (new_end_pos > write_acc->file_size)
            {
                write_acc->file_size = new_end_pos;
                mark_inode_metadata_dirty(&*write_acc);
            }
            write_acc.mark_dirty();
        }

        bytes_written += copy_len;
    }

    return (bytes_written == 0 && len > 0) ? -1 : bytes_written;
}
