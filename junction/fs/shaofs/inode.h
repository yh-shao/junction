#pragma once
#include "fs.h"

struct MInode : public DInode
{
    mutable rwmutex_t dir_mtx;     // 若这个 inode 是目录，读写锁保护目录的并发访问
    mutable iExtent extent_hint;   // 上次访问的 extent，加速顺序访问
    spinlock_t hint_lock;          // 保护多线程对 extent_hint 的更新 （线程在持有 MInode 读锁的情况下，也可以更新 hint）
    volatile int has_dirty_data_cache;  // block cache 中是否有该 inode 数据块的脏数据。进行 buffered write 时会置位；Direct I/O 读只有置位后才检查 BlockCache 脏块

    MInode() 
    { 
        rwmutex_init(&dir_mtx); 
        memset(&extent_hint, 0, sizeof(extent_hint)); 
        spin_lock_init(&hint_lock);
        atomic_write(&has_dirty_data_cache, 0);
    }
    MInode& operator=(const DInode& disk_inode)   // 自定义拷贝赋值运算符，只拷贝盘上数据即可
    {
        DInode::operator=(disk_inode);
        memset(&extent_hint, 0, sizeof(extent_hint));
        spin_lock_init(&hint_lock);
        atomic_write(&has_dirty_data_cache, 0);
        return *this;
    }
};

int alloc_inum();
void free_inum(int inum);

static inline void update_extent_hint(MInode* inode, const iExtent& ext) 
{
    if (spin_try_lock(&inode->hint_lock)) 
    {
        inode->extent_hint = ext;
        spin_unlock(&inode->hint_lock);
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