#pragma once

#include "fs.h"
#include "inode.h"

int get_valid_extent_count(const iExtent* extents, int max_count);
BlockID lookup_extent(const iExtent* extents, int valid_count, BlockID logical_blk);
BlockID inode_bmap_locked(DInode* inode_ptr, int inum, BlockID logical_blk, bool allocate, bool* is_new = nullptr);
BlockID inode_bmap(int inum, BlockID logical_blk, bool allocate, bool* is_new = nullptr);