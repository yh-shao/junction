#include "inodeCache.h"
#include "blockCache.h"
#include "fs.h"
#include "file.h"
#include "extent.h"
#include <vector>
#include "group.h"

void final_flush()
{
	// atomic64_write(&runtime_info->spdk_uipi, 0);  // 停止让 IOKernel 检查 SPDK 完成情况
	// barrier();

	RuntimeFSBaseGuard g;
	// uint64_t before_flush = rdtsc();
	storage_write_obj(imap, BITMAP_LONG_SIZE(sb.inode_num) * sizeof(unsigned long), sb.imap_blockstart, sb.imap_blocknum);
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
        BlockID phys_blk = INVALID_BLOCK_ID;

        {
            auto read_acc = ih.read_access();     // 获取 Inode【共享读锁】，多线程可并发读！
            if (!read_acc->used || current_offset >= read_acc->file_size) break;   // 文件是否已被逻辑删除，或读取起点是否已超越实时文件大小，直接跳出，copy_len 保持为 0

            // 动态计算本次循环真正安全的读取长度（防御并发写导致的文件大小抖动）
            uint64_t actual_remain = read_acc->file_size - current_offset, request_remain = len - bytes_read;
            copy_len = MIN(BLOCK_SIZE - blk_offset, MIN(request_remain, actual_remain));

            phys_blk = inode_bmap_locked(const_cast<MInode*>(&(*read_acc)), inum, logical_blk, false, nullptr);  // 直接调用 bmap，内部可能会需要读取间接块，发生 IO 阻塞，当前线程会带锁休眠（不影响其他 reader）
        } // 读锁释放

        if (copy_len == 0) break;   // 正常抵达 EOF 或文件被并发删除，跳出主循环

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
                memcpy(buf + bytes_read, block_read_acc->data + blk_offset, copy_len);
            }  // 自动释放物理块读锁
        }

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
        uint64_t logical_blk = current_offset / BLOCK_SIZE;
        uint64_t blk_offset = current_offset % BLOCK_SIZE;        
        uint64_t copy_len = MIN(BLOCK_SIZE - blk_offset, len - bytes_written);

        BlockID phys_blk = INVALID_BLOCK_ID;
        bool is_new_block = false;

        {
            auto write_acc = ih.write_access();   // 获取 Inode 排他写锁
            if (!write_acc->used)
            {
                log_info("inode %d is not valid (deleted)", inum);
                break;  // POSIX 语义：失败时，不返回 -1，而是返回目前已成功写入的字节数
            }

            phys_blk = inode_bmap_locked(&*write_acc, inum, logical_blk, true, &is_new_block);
            if (phys_blk == INVALID_BLOCK_ID) 
            {
                log_err("[file_write] Disk full or bmap failed at logical block %lu", logical_blk);
                break; // POSIX 语义：分配失败（磁盘满）时，不返回 -1，而是返回目前已成功写入的字节数
            }

            // 将数据写入 BlockCache （注意：此时我们仍然持有 inode 的排他写锁！）
            BlockHandle bh = bc_get_handle(phys_blk); 
            if (unlikely(!bh)) 
            {
                log_err("[file_write] Failed to get cache handle for physical block %lu", phys_blk);
                break; 
            }

            {
                auto block_write_acc = bh.write_access(); // 获取这一个物理块的排他写锁
                if (is_new_block && copy_len < BLOCK_SIZE) memset(block_write_acc->data, 0, BLOCK_SIZE);   // 防止旧磁盘垃圾数据泄漏
                memcpy(block_write_acc->data + blk_offset, buf + bytes_written, copy_len);  // 写入用户真实数据
                block_write_acc.mark_dirty();                                               // 标记底层数据块为脏页
            } // 物理块写锁释放

            // 在同一把 Inode 写锁的保护下更新 file_size，保证任何等待 Inode 锁的读线程一旦被唤醒，立刻就能看到刚写入的新数据和正确的文件长度。
            uint64_t new_end_pos = current_offset + copy_len;
            if (new_end_pos > write_acc->file_size) 
            {
                write_acc->file_size = new_end_pos;
                write_acc.mark_dirty(); // 标记 Inode 脏
            }
            
        } // 离开作用域，Inode 排他写锁被释放
        
        bytes_written += copy_len;
    }

    return bytes_written;
}