#include "extent.h"
#include "blockCache.h"
#include "group.h"
#include "inode.h"
#include <vector>
#include <algorithm>
#include "inodeCache.h"

extern "C" {
#include "../runtime/defs.h"
}

#define MAX_EXTENTS  (DIRECT_EXTENT_NUM + EXTENTS_PER_BLOCK + 1)

int get_valid_extent_count(const iExtent* extents, int max_count) 
{
    int count = 0;
    while (count < max_count && extents[count].block_count > 0) count++;
    return count;
}
BlockID lookup_extent(const iExtent* extents, int valid_count, BlockID logical_blk)   // 在有序的 Extent 数组中进行二分查找，若找到，则直接返回物理块号
{
    int left = 0, right = valid_count - 1;
    while (left <= right) 
    {
        int mid = left + (right - left) / 2;
        const auto& ext = extents[mid];

        if (logical_blk < ext.logical_start) 
            right = mid - 1;
        else if (logical_blk >= ext.logical_start + ext.block_count) 
            left = mid + 1;
        else   // 命中
            return ext.physical_start + (logical_blk - ext.logical_start);  
    }
    return INVALID_BLOCK_ID;
}

int compact_extents_inplace(iExtent* exts, int count)   // 就地排序 + 合并相邻 extent，返回合并后的数量（无需堆分配）
{
    if (count <= 0) return 0;

    std::sort(exts, exts + count, [](const iExtent& a, const iExtent& b) { return a.logical_start < b.logical_start; });   // 强制按逻辑起始块号排序

    int write_pos = 0;  // 合并后写入的位置
    for (int i = 1; i < count; i++)
    {
        iExtent& last = exts[write_pos];
        const iExtent& cur = exts[i];
        if (last.logical_start + last.block_count == cur.logical_start && last.physical_start + last.block_count == cur.physical_start)
            last.block_count += cur.block_count;   // merge
        else
            exts[++write_pos] = cur;               // 无法 merge，保留为独立 extent
    }
    return write_pos + 1;
}

// 约定：调用此函数前，caller 必须持有该 inode 的读锁（无需allocate）或写锁（需要allocate）
BlockID inode_bmap_locked(DInode* inode_ptr, int inum, BlockID logical_blk, bool allocate, bool* is_new) 
{
    if (is_new) *is_new = false; // 默认初始化为老块（并非新分配的块）

    // Fast Path: 尝试在 Direct Extents 中直接命中
    int direct_count = get_valid_extent_count(inode_ptr->direct_extents, DIRECT_EXTENT_NUM);
    BlockID phys_blk = lookup_extent(inode_ptr->direct_extents, direct_count, logical_blk);
    if (phys_blk != INVALID_BLOCK_ID) return phys_blk;

    BlockHandle ind_bh;
    int indirect_count = 0;
    if (direct_count == DIRECT_EXTENT_NUM)  // 尝试在 Indirect Extents 中命中
    {
        ind_bh = bc_get_handle(inode_ptr->indirect_extent_block);
        if (unlikely(!ind_bh)) 
        {
            log_err("[inode_bmap()] Failed to get indirect block [%lu] for inode %d", inode_ptr->indirect_extent_block, inum);
            return INVALID_BLOCK_ID;
        }

        {
            // auto ind_acc = ind_bh.access();
            // ind_exts = reinterpret_cast<iExtent*>(ind_acc->data);
            auto ind_read_acc = ind_bh.read_access();
            const iExtent* ind_exts = reinterpret_cast<const iExtent*>(ind_read_acc->data);
            indirect_count = get_valid_extent_count(ind_exts, EXTENTS_PER_BLOCK);
            phys_blk = lookup_extent(ind_exts, indirect_count, logical_blk);
        }
        
        if (phys_blk != INVALID_BLOCK_ID) return phys_blk;
    }// 离开作用域，ind_read_acc 析构，自动释放间接块的读锁

    if (allocate == false) return INVALID_BLOCK_ID; // 读取模式遇到空洞，返回

    // 新分配一个块，并加入到现有 extent 数组中
    BlockID new_phys_blk = alloc_block();
    if (new_phys_blk == INVALID_BLOCK_ID) return INVALID_BLOCK_ID; // 磁盘已满

    // std::vector<iExtent> all_extents;      // 收集所有 Extents
    // all_extents.reserve(DIRECT_EXTENT_NUM + EXTENTS_PER_BLOCK + 1);
    // for (int i = 0; i < direct_count; ++i) all_extents.push_back(inode_ptr->direct_extents[i]);
    iExtent all_extents[MAX_EXTENTS];   // 栈分配，零堆开销
    int ext_count = 0;
    for (int i = 0; i < direct_count; ++i) all_extents[ext_count++] = inode_ptr->direct_extents[i];

    if (ind_bh) // 如果间接块有效（说明该 inode 有用到 indirect extent block），重新获取读锁拷贝其内容
    {
        auto ind_read_acc = ind_bh.read_access();
        const iExtent* ind_exts = reinterpret_cast<const iExtent*>(ind_read_acc->data);
        indirect_count = get_valid_extent_count(ind_exts, EXTENTS_PER_BLOCK);
        // for (int i = 0; i < indirect_count; ++i) all_extents.push_back(ind_exts[i]); 
        for (int i = 0; i < indirect_count; ++i) all_extents[ext_count++] = ind_exts[i];
    }
    // all_extents.push_back({.logical_start = logical_blk, .physical_start = new_phys_blk, .block_count = 1});
    // compact_extents(all_extents);  // 压缩
    all_extents[ext_count++] = {.logical_start = logical_blk, .physical_start = new_phys_blk, .block_count = 1};
    ext_count = compact_extents_inplace(all_extents, ext_count);  // 就地压缩，无堆分配

    // if (all_extents.size() > DIRECT_EXTENT_NUM + EXTENTS_PER_BLOCK) 
    if (ext_count > DIRECT_EXTENT_NUM + EXTENTS_PER_BLOCK)
    {
        log_err("[extent] Extent array overflow for inode %d!", inum);
        free_block(new_phys_blk); // 事务回滚
        return INVALID_BLOCK_ID;
    }

    // 写回 Direct 区域 (直接修改 inode_ptr，因为调用者已经加了写锁)（inode 一定会被修改）
    int global_idx = 0;
    for (int i = 0; i < DIRECT_EXTENT_NUM; ++i) 
    {
        // if (global_idx < all_extents.size()) inode_ptr->direct_extents[i] = all_extents[global_idx++];
        if (global_idx < ext_count) inode_ptr->direct_extents[i] = all_extents[global_idx++];
        else 
        {
            memset(&inode_ptr->direct_extents[i], 0, sizeof(iExtent) * (DIRECT_EXTENT_NUM - i));
            break;
        }
    }

    if (ind_bh)
    {
        // auto ind_acc = ind_bh.access();
        auto ind_write_acc = ind_bh.write_access();    // 此时必须获取写锁！
        iExtent* ind_exts = reinterpret_cast<iExtent*>(ind_write_acc->data);
        
        for (int i = 0; i < EXTENTS_PER_BLOCK; ++i) 
        {
            // if (global_idx < all_extents.size()) ind_exts[i] = all_extents[global_idx++];
            if (global_idx < ext_count) ind_exts[i] = all_extents[global_idx++];
            else 
            {
                memset(&ind_exts[i], 0, sizeof(iExtent) * (EXTENTS_PER_BLOCK - i));
                break;
            }
        }
        ind_write_acc.mark_dirty(); // 标记间接块为脏
    }

    if (is_new) *is_new = true;
    return new_phys_blk;
}
BlockID inode_bmap(int inum, BlockID logical_blk, bool allocate, bool* is_new)
{
    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) 
    {
        log_err("[inode_bmap()] Failed to get inode %d from cache", inum);
        return INVALID_BLOCK_ID;
    }

    BlockID phys_blk = INVALID_BLOCK_ID;
    bool newly_allocated = false;

    if (allocate)
    {
        auto write_acc = ih.write_access();
        DInode* inode_ptr = &(*write_acc);
        phys_blk = inode_bmap_locked(inode_ptr, inum, logical_blk, true, &newly_allocated);
        if (newly_allocated) write_acc.mark_dirty();
    }
    else
    {
        auto read_acc = ih.read_access();
        DInode* inode_ptr = const_cast<MInode*>(&(*read_acc));
        phys_blk = inode_bmap_locked(inode_ptr, inum, logical_blk, false, is_new);
    }

    if (is_new) *is_new = newly_allocated;

    return phys_blk;
}