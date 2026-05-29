#pragma once

#include "fs.h"
#include "inode.h"
#include <stdint.h>

BlockID lookup_extent(const iExtent* extents, int valid_count, BlockID logical_blk, iExtent* out_extent = nullptr);
BlockID inode_bmap_locked(MInode* inode_ptr, BlockID logical_blk, bool allocate, bool* is_new);
bool inode_lookup_extent_locked(MInode* inode_ptr, BlockID logical_blk, iExtent* out_extent);
int inode_append_run_locked(MInode* inode_ptr, BlockID logical_start, int max_blocks, BlockID* out_blocks);
bool inode_for_each_extent(const MInode* inode, bool include_direct, bool (*cb)(const iExtent&, void*), void* arg);
bool inode_for_each_extent_metadata_block(const MInode* inode, bool (*cb)(BlockID, void*), void* arg);
bool inode_flush_extent_metadata(const MInode* inode);
bool inode_free_extent_metadata(MInode* inode);
bool inode_drain_deferred_extent_frees();

static inline bool block_in_extent(BlockID logical_blk, const iExtent& ext)
{
    if (ext.block_count == 0) return false;
    return (logical_blk >= ext.logical_start && logical_blk < ext.logical_start + ext.block_count);
}
