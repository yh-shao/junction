#pragma once
#include "fs.h"
#include "generic_cache/sharded_cache.h"
#include "generic_cache/backend.h"
#include "inode.h"
#include "blockCache.h"

#define DEFAULT_INODECACHE_CAPACITY 8192
#define DEFAULT_INODE_SHARD_NUM     256

class InodeBackend : public Backend<int, MInode> {
public:
    bool read(const int& inum, MInode& value)
    {
        BlockID blockidx = sb.itable_blockstart + inum / INODENUM_PER_BLOCK;
        int offset = inum % INODENUM_PER_BLOCK;

        BlockHandle handle = bc_get_handle(blockidx);
        if (!handle) return false;

        auto acc = handle.read_access();
        const DInode* inode_tbl = reinterpret_cast<const DInode*>(acc->data);
        value = inode_tbl[offset];
        return true;
    }

    bool write(const int& inum, const MInode& value)
    {
        BlockID blockidx = sb.itable_blockstart + inum / INODENUM_PER_BLOCK;
        int offset = inum % INODENUM_PER_BLOCK;

        BlockHandle handle = bc_get_handle(blockidx);
        if (!handle) return false;

        auto acc = handle.write_access();
        DInode* inode_tbl = reinterpret_cast<DInode*>(acc->data);
        inode_tbl[offset] = value;
        acc.mark_dirty();  
        // log_info("written Inode %d to Block %lu", inum, blockidx);
        return true;
    }

    static InodeBackend& getInstance() 
    {
        static InodeBackend instance;
        return instance;
    }

    InodeBackend(const InodeBackend&) = delete;
    void operator=(const InodeBackend&) = delete;
private:
    InodeBackend() = default;
};


using GlobalInodeCache = ShardedCache<int, MInode>;
GlobalInodeCache& get_inode_cache();
void init_inode_cache(size_t capacity = DEFAULT_INODECACHE_CAPACITY, size_t shard_num = DEFAULT_INODE_SHARD_NUM);

using InodeHandle = GlobalInodeCache::Handle;
InodeHandle ic_alloc_inode(file_type_t type, int inum = -1);
InodeHandle ic_get_inode(int inum);
void ic_flush_all();
bool ic_flush_inode(int inum);     // 将指定 inode 刷写到后端（如果在缓存中且为脏）
bool ic_free_inode(int inum);