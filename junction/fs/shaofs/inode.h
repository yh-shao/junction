#pragma once
#include "fs.h"

struct MInode : public DInode
{
    mutable rwmutex_t dir_mtx;     // 若这个 inode 是目录，读写锁保护目录的并发访问
    mutable iExtent extent_hint;   // 上次访问的 extent，加速顺序访问
    spinlock_t hint_lock;          // 保护多线程对 extent_hint 的更新 （线程在持有 MInode 读锁的情况下，也可以更新 hint）
    spinlock_t dirty_lock;         // 保护下面的 dirty byte range
    volatile int has_dirty_data_cache;  // block cache 中是否有该 inode 数据块的脏数据。进行 buffered write 时会置位；Direct I/O 读只有置位后才检查 BlockCache 脏块
    uint64_t dirty_data_start;     // buffered write 脏数据区间：[start, end)
    uint64_t dirty_data_end;
    uint64_t dirty_data_seq;

    void init_runtime_state()
    {
        rwmutex_init(&dir_mtx);
        memset(&extent_hint, 0, sizeof(extent_hint));
        spin_lock_init(&hint_lock);
        spin_lock_init(&dirty_lock);
        atomic_write(&has_dirty_data_cache, 0);
        dirty_data_start = 0;
        dirty_data_end = 0;
        dirty_data_seq = 0;
    }

    void clear_dirty_data_unlocked()
    {
        atomic_write(&has_dirty_data_cache, 0);
        dirty_data_start = 0;
        dirty_data_end = 0;
    }

    MInode() { init_runtime_state(); }

    MInode& operator=(const DInode& disk_inode)   // 自定义拷贝赋值运算符，只拷贝盘上数据即可
    {
        DInode::operator=(disk_inode);
        init_runtime_state();
        return *this;
    }
};

int alloc_inum();
void free_inum(int inum);

static inline void update_extent_hint(MInode* inode, const iExtent& ext) 
{
    if (spin_try_lock_np(&inode->hint_lock))
    {
        inode->extent_hint = ext;
        spin_unlock_np(&inode->hint_lock);
    }
}

static inline bool uses_indirect_block(const MInode* inode) 
{
    return inode->valid_extent_count > DIRECT_EXTENT_NUM;
}
static inline uint32_t direct_extent_count(const MInode* inode)
{
    return MIN(inode->valid_extent_count, (uint32_t)DIRECT_EXTENT_NUM);
}
static inline uint32_t indirect_extent_count(const MInode* inode)
{
    return inode->valid_extent_count - direct_extent_count(inode);
}