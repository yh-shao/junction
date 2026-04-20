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

#define MAX_EXTENTS  (DIRECT_EXTENT_NUM + EXTENTS_PER_BLOCK)

BlockID lookup_extent(const iExtent* extents, int valid_count, BlockID logical_blk, iExtent* out_extent)   // 在有序的 Extent 数组中进行二分查找，若找到，则直接返回物理块号
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
        {
            if (out_extent) *out_extent = ext;
            return ext.physical_start + (logical_blk - ext.logical_start);  
        }
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

static BlockID bmap_lookup(MInode* inode, BlockID logical_blk)   // 在 inode 中查找这个 logical_blk，若能找到，则返回对应的实际物理块号
{
    if (inode->valid_extent_count == 0) return INVALID_BLOCK_ID;

    // Fast Path: Hint 缓存
    if (spin_try_lock(&inode->hint_lock)) 
    {
        iExtent hint = inode->extent_hint;
        spin_unlock(&inode->hint_lock);
        if (block_in_extent(logical_blk, hint)) return hint.physical_start + (logical_blk - hint.logical_start);
    }

    uint32_t total_exts = inode->valid_extent_count, direct_count = direct_extent_count(inode), indirect_count = indirect_extent_count(inode);

    // Fast Path: 单 extent
    if (total_exts == 1)
    {
        const iExtent& ext = inode->direct_extents[0];
        if (block_in_extent(logical_blk, ext)) 
        {
            update_extent_hint(inode, ext);
            return ext.physical_start + (logical_blk - ext.logical_start);
        }
    }

    // 在 direct extents 中进行查找
    iExtent found;
    BlockID phys_blk = lookup_extent(inode->direct_extents, direct_count, logical_blk, &found);
    if (phys_blk != INVALID_BLOCK_ID)
    {
        update_extent_hint(inode, found);
        return phys_blk;
    }

    // 在 indirect extents 中进行查找
    if (uses_indirect_block(inode))
    {
        BlockHandle ind_bh = bc_get_handle(inode->indirect_extent_block);   // TODO：此时如果发生加载，则会阻塞所有需要读取这个 inode 的线程，可以优化为采用 prefetch 策略
        if (unlikely(!ind_bh)) 
        {
            log_err("[bmap_lookup_locked()] Failed to get indirect block [%lu] for inode %d", inode->indirect_extent_block, inode->idx);
            return INVALID_BLOCK_ID;
        }

        {
            auto acc = ind_bh.read_access();
            const iExtent* ind_exts = reinterpret_cast<const iExtent*>(acc->data);
            phys_blk = lookup_extent(ind_exts, indirect_count, logical_blk, &found);
            if (phys_blk != INVALID_BLOCK_ID)
            {
                update_extent_hint(inode, found);
                return phys_blk;
            }
        }
    }

    return INVALID_BLOCK_ID;
}

static bool bmap_try_append(MInode* inode, BlockID logical_blk, BlockID phys_blk)  // 尝试追加到末尾 extent 中
{
    if (inode->valid_extent_count == 0)    // 当前这个 inode 还没有任何分配的块
    {
        iExtent new_ext = { .logical_start = logical_blk, .physical_start = phys_blk, .block_count = 1 };
        inode->direct_extents[0] = new_ext;
        inode->valid_extent_count = 1;
        update_extent_hint(inode, new_ext);
        return true;
    }

    if (uses_indirect_block(inode))   // 追加到 indirect extent block 中
    {
        BlockHandle ind_bh = bc_get_handle(inode->indirect_extent_block);
        if (unlikely(!ind_bh)) 
        {
            log_err("[bmap_try_append()] Failed to get indirect block [%lu] for inode %d", inode->indirect_extent_block, inode->idx);
            return false;
        }

        {
            auto acc = ind_bh.write_access();
            iExtent* exts = reinterpret_cast<iExtent*>(acc->data);
            iExtent& last = exts[indirect_extent_count(inode) - 1];

            if (last.logical_start + last.block_count == logical_blk && last.physical_start + last.block_count == phys_blk)
            {
                last.block_count++;
                acc.mark_dirty();
                update_extent_hint(inode, last);
                return true;
            }
        }
    }
    else   // 追加到 direct extents 中
    {
        iExtent& last = inode->direct_extents[direct_extent_count(inode) - 1];
        if (last.logical_start + last.block_count == logical_blk && last.physical_start + last.block_count == phys_blk)
        {
            last.block_count++;
            update_extent_hint(inode, last);
            return true;
        }
    }

    return false; // 无法简单追加，交由 Slow Path 处理
}

static bool bmap_insert_compact(MInode* inode, BlockID logical_blk, BlockID phys_blk)
{
    iExtent all_extents[MAX_EXTENTS + 1];
    int ext_count = 0;

    // 收集所有的 Extent
    uint32_t direct_cnt = direct_extent_count(inode);
    for (uint32_t i = 0; i < direct_cnt; ++i) all_extents[ext_count++] = inode->direct_extents[i];

    BlockHandle ind_bh;
    if (uses_indirect_block(inode))
    {
        ind_bh = bc_get_handle(inode->indirect_extent_block);
        if (unlikely(!ind_bh)) 
        {
            log_err("[inode_bmap_locked()] Failed to get indirect block [%lu] for inode %d", inode->indirect_extent_block, inode->idx);
            return false;
        }
        
        {
            auto acc = ind_bh.read_access();
            const iExtent* exts = reinterpret_cast<const iExtent*>(acc->data);
            uint32_t ind_cnt = indirect_extent_count(inode);
            for (uint32_t i = 0; i < ind_cnt; ++i) all_extents[ext_count++] = exts[i];
        }
    }

    // 加入新 Extent 并就地合并
    all_extents[ext_count++] = { .logical_start = logical_blk, .physical_start = phys_blk, .block_count = 1 };
    ext_count = compact_extents_inplace(all_extents, ext_count);
    if (ext_count > MAX_EXTENTS)
    {
        log_err("[extent] Extent array overflow for inode %d!", inode->idx);
        return false;
    }

    if (ext_count > DIRECT_EXTENT_NUM && !ind_bh)    // 需要将 extent 写入到 indirect extent block 中
    {
        ind_bh = bc_get_handle(inode->indirect_extent_block);
        if (unlikely(!ind_bh)) 
        {
            log_err("[bmap_insert_compact] Failed to fetch pre-allocated indirect block %lu for inode %d", inode->indirect_extent_block, inode->idx);
            return false;
        }
    }

    // 写回元数据
    inode->valid_extent_count = ext_count;
    int global_idx = 0;
    for (int i = 0; i < DIRECT_EXTENT_NUM; ++i)  // 写回 Direct 区域
    {
        if (global_idx < ext_count) inode->direct_extents[i] = all_extents[global_idx++];
        else { memset(&inode->direct_extents[i], 0, sizeof(iExtent) * (DIRECT_EXTENT_NUM - i)); break; }
    }

    if (ext_count > DIRECT_EXTENT_NUM)           // 写回 Indirect 区域
    {
        auto acc = ind_bh.write_access();
        iExtent* exts = reinterpret_cast<iExtent*>(acc->data);
        for (int i = 0; i < EXTENTS_PER_BLOCK; ++i)
        {
            if (global_idx < ext_count) exts[i] = all_extents[global_idx++];
            else { memset(&exts[i], 0, sizeof(iExtent) * (EXTENTS_PER_BLOCK - i)); break; }
        }
        acc.mark_dirty();
    }

    update_extent_hint(inode, { .logical_start = logical_blk, .physical_start = phys_blk, .block_count = 1 });
    return true;
}

// 约定：调用此函数前，caller 必须持有该 inode 的读锁（无需allocate）或写锁（需要allocate）
BlockID inode_bmap_locked(MInode* inode_ptr, BlockID logical_blk, bool allocate, bool* is_new)
{
    if (is_new) *is_new = false;

    BlockID phys_blk = bmap_lookup(inode_ptr, logical_blk);  // 查找 inode 中是否有该逻辑块号，若有则返回相应的物理块号
    if (phys_blk != INVALID_BLOCK_ID) return phys_blk;

    if (!allocate) return INVALID_BLOCK_ID;    // 该 inode 中没有该逻辑块号

    // 新分配一个块
    BlockID new_phys_blk = alloc_block();
    if (new_phys_blk == INVALID_BLOCK_ID) 
    {
        log_err("[inode_bmap_locked()] Fail to alloc a new block");
        return INVALID_BLOCK_ID;
    }
    if (is_new) *is_new = true;

    // 将新分配的块加入到 inode 中，更新相关 extent 的信息
    if (bmap_try_append(inode_ptr, logical_blk, new_phys_blk)) return new_phys_blk;     // Fast Path: 尝试快速追加到最后一个 extent 中
    if (bmap_insert_compact(inode_ptr, logical_blk, new_phys_blk)) return new_phys_blk; // Slow Path: 触发 Extent 数组重排合并

    // 异常恢复 (Rollback)
    free_block(new_phys_blk);
    if (is_new) *is_new = false;
    return INVALID_BLOCK_ID;
}