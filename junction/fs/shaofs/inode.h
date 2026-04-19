#pragma once
#include "fs.h"

struct MInode : public DInode
{
    mutable rwmutex_t dir_mtx;     // 若这个 inode 是目录，读写锁保护目录的并发访问
    mutable iExtent extent_hint;   // 上次访问的 extent，加速顺序访问
    spinlock_t hint_lock;          // 保护多线程对 extent_hint 的更新 （线程在持有 MInode 读锁的情况下，也可以更新 hint）

    MInode() 
    { 
        rwmutex_init(&dir_mtx); 
        memset(&extent_hint, 0, sizeof(extent_hint)); 
        spin_lock_init(&hint_lock);
    }
    MInode& operator=(const DInode& disk_inode)   // 自定义拷贝赋值运算符，只拷贝盘上数据即可
    {
        DInode::operator=(disk_inode);
        memset(&extent_hint, 0, sizeof(extent_hint));
        spin_lock_init(&hint_lock);
        return *this;
    }
};

int alloc_inum();
void free_inum(int inum);
bool uses_indirect_block(const DInode* ino);

static inline void update_extent_hint(MInode* inode, const iExtent& ext) 
{
    if (spin_try_lock(&inode->hint_lock)) 
    {
        inode->extent_hint = ext;
        spin_unlock(&inode->hint_lock);
    }
}