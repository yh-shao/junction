#pragma once
#include "fs.h"

struct DirIndex;

struct MInode : public DInode
{
    mutable rwmutex_t dir_mtx;     // 若这个 inode 是目录，读写锁保护目录的并发访问
    DirIndex* dir_index;           // 目录运行时索引，仅用于加速 lookup/add/delete，不写入磁盘
    mutable iExtent extent_hint;   // 上次访问的 extent，加速顺序访问
    spinlock_t hint_lock;          // 保护多线程对 extent_hint 的更新 （线程在持有 MInode 读锁的情况下，也可以更新 hint）
    spinlock_t dirty_lock;         // 保护下面的 dirty byte range
    volatile int has_dirty_data_cache;  // block cache 中是否有该 inode 数据块的脏数据。进行 buffered write 时会置位；Direct I/O 读只有置位后才检查 BlockCache 脏块
    uint64_t dirty_data_start;     // buffered write 脏数据区间：[start, end)
    uint64_t dirty_data_end;
    uint64_t dirty_data_seq;
    uint64_t inode_dirty_seq;      // inode 盘上元数据的变更序号
    uint64_t inode_fsync_seq;      // 最近一次 fsync 已经持久化的 inode_dirty_seq

    void init_runtime_state();
    void drop_dir_index();

    void clear_dirty_data_unlocked()
    {
        atomic_write(&has_dirty_data_cache, 0);
        dirty_data_start = 0;
        dirty_data_end = 0;
    }

    MInode();
    ~MInode();

    MInode& operator=(const DInode& disk_inode);   // 自定义拷贝赋值运算符，只拷贝盘上数据即可
};

static inline void mark_inode_metadata_dirty(MInode* inode)
{
    inode->inode_dirty_seq++;
}

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

static inline bool uses_indirect_block(const DInode* inode) 
{
    return inode->valid_extent_count > DIRECT_EXTENT_NUM;
}
static inline bool uses_extent_tree(const DInode* inode)
{
    return inode->valid_extent_count > LEGACY_MAX_EXTENT_NUM;
}
static inline uint32_t direct_extent_count(const DInode* inode)
{
    return MIN(inode->valid_extent_count, (uint32_t)DIRECT_EXTENT_NUM);
}
static inline uint32_t indirect_extent_count(const DInode* inode)
{
    return inode->valid_extent_count - direct_extent_count(inode);
}
static inline uint32_t legacy_indirect_extent_count(const DInode* inode)
{
    if (inode->valid_extent_count <= DIRECT_EXTENT_NUM) return 0;
    return MIN(inode->valid_extent_count - (uint32_t)DIRECT_EXTENT_NUM, (uint32_t)EXTENTS_PER_BLOCK);
}