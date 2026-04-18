#pragma once
#include "fs.h"

extern int core_to_group[];
struct alignas(64) GroupDescExt {
    spinlock_t lock;                // 多个核可能会访问到同一个 group（一个核在进行分配，另一个核在进行释放）
    uint32_t   free_blocks_count;   // 当前空闲数据块数
    uint32_t   next_free_hint;      // 记录上次分配到的位置
    uint32_t   flags;               // 状态标志位
    uint32_t   owner_count;         // 记录当前有多少个 core 在使用此 group

    BlockID    bitmap_lba;                   // 该 group 的 bitmap 起始块
    BlockID    data_start_lba;               // 该 group 的第一个数据块
    uint32_t   group_id;                     // group 编号，便于调试
    GroupDescExt() : free_blocks_count(0), next_free_hint(0), flags(0), owner_count(0), bitmap_lba(0), data_start_lba(0), group_id(0) { spin_lock_init(&lock); }  // 初始值
};
extern GroupDescExt* group_info;

void init_group();
bool set_newgroup(unsigned int coreid);
void get_group_by_gid(int groupid, BlockID* bitmap, BlockID* datablock_start);
void get_group_by_blkid(BlockID blk, int* groupid, BlockID* bitmap, BlockID* datablock_start);
bool is_datablock(BlockID block_id);

BlockID alloc_block();
int alloc_blocks(BlockID* out, int count);  // 批量分配连续块，返回实际分配数量
void free_block(BlockID blk);
void free_extent(const iExtent* ext);
void sync_all_gdt();