#pragma once
#include "fs.h"

struct MInode : public DInode
{
    mutable rwmutex_t dir_mtx;     // 若这个 inode 是目录，读写锁保护目录的并发访问
    mutable iExtent extent_hint;   // 上次 extent 查找的缓存，加速顺序访问 O(1) 命中

    MInode() { rwmutex_init(&dir_mtx); memset(&extent_hint, 0, sizeof(extent_hint)); }
    MInode& operator=(const DInode& disk_inode)   // 自定义拷贝赋值运算符，只拷贝盘上数据即可
    {
        DInode::operator=(disk_inode);
        return *this;
    }
};

int alloc_inum();
void free_inum(int inum);