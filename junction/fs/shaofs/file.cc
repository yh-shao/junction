#include "inodeCache.h"
#include "blockCache.h"
#include "fs.h"
#include "file.h"
#include "extent.h"
#include <vector>
#include "group.h"
#include "dsa.h"

// 释放 inode 持有的所有数据块（direct + indirect extents）。调用前必须持有 inode 写锁。
static void free_inode_data_blocks(MInode* inode_ptr)
{
    // 释放 direct extents 中引用的所有物理块
    int direct_count = direct_extent_count(inode_ptr);
    for (int i = 0; i < direct_count; i++)
    {
        const iExtent& ext = inode_ptr->direct_extents[i];
        for (uint64_t j = 0; j < ext.block_count; j++)
            free_block(ext.physical_start + j);
    }

    // 释放 indirect extents 中引用的所有物理块
    if (uses_indirect_block(inode_ptr))
    {
        BlockHandle ind_bh = bc_get_handle(inode_ptr->indirect_extent_block);
        if (!ind_bh) 
        {
            log_err("[free_inode_data_blocks] Failed to get indirect block [%lu] for inode %d", inode_ptr->indirect_extent_block, inode_ptr->idx);
            return;
        }

        {
            auto ind_acc = ind_bh.read_access();
            const iExtent* ind_exts = reinterpret_cast<const iExtent*>(ind_acc->data);
            int indirect_count = indirect_extent_count(inode_ptr);
            for (int i = 0; i < indirect_count; i++)
            {
                const iExtent& ext = ind_exts[i];
                for (uint64_t j = 0; j < ext.block_count; j++)
                    free_block(ext.physical_start + j);
            }
        }
    }

    // 清空 inode 的 extent 元数据
    memset(inode_ptr->direct_extents, 0, sizeof(inode_ptr->direct_extents));
    inode_ptr->file_size = 0;
    inode_ptr->valid_extent_count = 0;
    memset(&inode_ptr->extent_hint, 0, sizeof(inode_ptr->extent_hint));  // 清空 extent hint
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

void final_flush()
{
	// atomic64_write(&runtime_info->spdk_uipi, 0);  // 停止让 IOKernel 检查 SPDK 完成情况
	// barrier();

	RuntimeFSBaseGuard g;
	// uint64_t before_flush = rdtsc();
	storage_write_obj(imap, BITMAP_LONG_SIZE(sb.inode_num) * sizeof(unsigned long), sb.imap_blockstart, 0);
	sync_all_gdt();
	ic_flush_all();
	bc_flush_all();
	// uint64_t after_flush = rdtsc();
	// log_info("[flush] duration: %lu us", (after_flush - before_flush) / cycles_per_us);
}

ssize_t file_read(int inum, char* buf, off_t offset, size_t len)
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
                    // memcpy(buf + bytes_read, block_read_acc->data + blk_offset, copy_len);
                    dsa_copy(buf + bytes_read, block_read_acc->data + blk_offset, copy_len);  // 使用 DSA 加速内存复制，释放 CPU 资源
                }  // 自动释放物理块读锁
            }
        }

        if (copy_len == 0) break;
        bytes_read += copy_len;
    }
    
    return bytes_read;
}

ssize_t file_write(int inum, const char* buf, off_t offset, size_t len)
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
                    memcpy(block_write_acc->data + blk_offset, buf + bytes_written, copy_len);
                    block_write_acc.mark_dirty();

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

            BlockHandle bh = bc_get_handle(phys_blk);
            if (unlikely(!bh))
            {
                log_err("[file_write] Failed to get cache handle for physical block %lu", phys_blk);
                break;
            }

            {
                auto block_write_acc = bh.write_access();
                if (is_new_block && copy_len < BLOCK_SIZE) memset(block_write_acc->data, 0, BLOCK_SIZE);   // 新分配的块若未写满，必须填 0
                memcpy(block_write_acc->data + blk_offset, buf + bytes_written, copy_len);
                block_write_acc.mark_dirty();
            }

            uint64_t new_end_pos = current_offset + copy_len;
            if (new_end_pos > write_acc->file_size) write_acc->file_size = new_end_pos;
            write_acc.mark_dirty();
        } // inode 写锁释放

        bytes_written += copy_len;
    }

    return (bytes_written == 0 && len > 0) ? -1 : bytes_written;
}


/* O_DIRECT I/O */
// 目前是实现是绕过 Block Cache，使用 storage_read_obj/storage_write_obj 直接访问磁盘，但是内部还是会分配临时 SPDK DMA buffer 并 memcpy 到/从用户 buffer，并不是真正的零拷贝（有待修改）
ssize_t file_read_direct(int inum, char* buf, off_t offset, size_t len)
{
    if (len == 0) return 0;

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

            phys_blk = inode_bmap_locked(const_cast<MInode*>(&(*read_acc)), logical_blk, false, nullptr);

            if (phys_blk == INVALID_BLOCK_ID) memset(buf + bytes_read, 0, copy_len);  // 处理文件空洞 (Hole)：直接将对应的用户 Buffer 填 0，无需下发 I/O
            else
            {
                // bc_flush_block(phys_blk);  // 若该块在 cache 中且为脏，先刷盘保证一致性，从而 Direct I/O 能读到 Cache 中尚未落盘的脏数据 （为什么不直接从 Cache 中读取呢？这里还是直接点，简化逻辑）
                if (storage_read_obj(buf + bytes_read, copy_len, phys_blk, blk_offset) != 0) break;
            }
        }

        bytes_read += copy_len;
    }

    return bytes_read;
}

ssize_t file_write_direct(int inum, const char* buf, off_t offset, size_t len)
{
    if (len == 0) return 0;

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
                    // bc_invalidate_block(phys_blk);  // 关键：Direct I/O 写入磁盘前，如果该物理块在 Block Cache 中，必须将其失效（或者刷盘后失效），否则后续的 Buffered Read 会读到 Cache 中的旧数据

                    if (storage_write_obj(buf + bytes_written, copy_len, phys_blk, blk_offset) != 0) 
                    {
                        log_err("[file_write_direct] Failed to write to physical block %lu", phys_blk);
                        break;
                    }
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

            // bc_invalidate_block(phys_blk);   // 新分配的块大概率不在 Cache 中，但如果是复用的被释放的块，为了安全依然 invalidate 一下

            int ret = 0;
            if (is_new_block) 
                ret = storage_write_obj_no_rmw(buf + bytes_written, copy_len, phys_blk, blk_offset);
            else 
                ret = storage_write_obj(buf + bytes_written, copy_len, phys_blk, blk_offset);
            if (unlikely(ret != 0)) break;

            uint64_t new_end_pos = current_offset + copy_len;
            if (new_end_pos > write_acc->file_size) write_acc->file_size = new_end_pos;
            write_acc.mark_dirty();
        }

        bytes_written += copy_len;
    }

    return (bytes_written == 0 && len > 0) ? -1 : bytes_written;
}