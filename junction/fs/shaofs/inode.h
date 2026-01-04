#pragma once

#include "disk.h"

typedef struct MInode {
    uint32_t    dev;
    int         inum;               // inode 号（与 idx 相同）
    DInode      disk_inode;         // 盘上结构
    int         refcnt;             // 使用计数
    bool        dirty;              // 是否需要写回盘上（新创建的 或 修改过）
    bool        valid;              // 是否有效（新创建的 或 从盘上加载过）
    spinlock_t  lock;

    MInode(int inum) : inum(inum), valid(false), dirty(false), refcnt(0)
    {
        spin_lock_init(&lock);
        // log_info("MInode created for inum %d", inum);
    }
} MInode;

int alloc_inum();
void free_inum(int inum);

void read_dinode_from_blockcache(int idx, DInode* dinode);
void write_dinode_to_blockcache(int idx, DInode* dinode);