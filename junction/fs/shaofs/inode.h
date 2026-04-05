#pragma once
#include "fs.h"

struct MInode : public DInode
{
    mutable mutex_t dir_mtx;   // 若这个 inode 是目录，保护这个目录能否被增删

    MInode() { mutex_init(&dir_mtx); }
    MInode& operator=(const DInode& disk_inode)   // 自定义拷贝赋值运算符，只拷贝盘上数据即可
    {
        DInode::operator=(disk_inode);
        return *this;
    }
};

int alloc_inum();
void free_inum(int inum);