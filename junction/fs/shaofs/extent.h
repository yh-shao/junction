#pragma once

#include "fs.h"
#include "inode.h"

BlockID lookup_extent(const iExtent* extents, int valid_count, BlockID logical_blk, iExtent* out_extent = nullptr);
BlockID inode_bmap_locked(MInode* inode_ptr, BlockID logical_blk, bool allocate, bool* is_new);

static inline bool block_in_extent(BlockID logical_blk, const iExtent& ext)
{
    if (ext.block_count == 0) return false;
    return (logical_blk >= ext.logical_start && logical_blk < ext.logical_start + ext.block_count);
}