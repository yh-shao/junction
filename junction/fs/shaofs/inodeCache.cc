#include "inodeCache.h"
#include "generic_cache/backend.h"
#include "utili.h"
#include "inode.h"
#include "extent.h"
#include "group.h"
#include <cstring>

static GlobalInodeCache* g_inode_cache_ptr = nullptr;

GlobalInodeCache& get_inode_cache()
{
    if (unlikely(g_inode_cache_ptr == nullptr)) 
    {
        log_err("FATAL: get_inode_cache() called BEFORE init_inode_cache()!");
        init_inode_cache();
    }
    return *g_inode_cache_ptr;
}

void init_inode_cache(size_t capacity, size_t shard_num)
{
    if (g_inode_cache_ptr != nullptr) return;

    log_info("init inode cache ...");

    size_t total_mem = GlobalInodeCache::CacheEntry_footprint(capacity, shard_num);
    char* buffer = (char*)malloc(total_mem);
    if (unlikely(!buffer)) 
    { 
        log_err("[inode_cache_init] ERROR: malloc failed!");
        buffer = nullptr;
    }

    g_inode_cache_ptr = new GlobalInodeCache(capacity, shard_num, &InodeBackend::getInstance(), []() { return new LRUPolicy<int, MInode>(); }, buffer);
}

InodeHandle ic_get_inode(int inum) { return get_inode_cache().getHandle(inum); }

InodeHandle ic_alloc_inode(file_type_t type, int inum)    // 如果 inum != -1 则尝试分配指定的 inum，否则分配一个新的 inum
{
    bool new_alloc_inum = false;
    if (inum == -1) 
    {
        inum = alloc_inum();
        new_alloc_inum = true;
    }
    else
    {
        if (inum < 0 || inum >= INODENUM || bitmap_atomic_test_and_set(imap, inum))
        {
            log_err("[ic_alloc_inode()] Fail to allocate requested inode %d", inum);
            return InodeHandle();
        }
        new_alloc_inum = true;
    }
    if (inum == -1) 
    {
        log_err("[ic_alloc_inode()] Fail to allocate a new inode: invalid inum");
        return InodeHandle(); // 返回空 Handle
    }

    InodeHandle handle = get_inode_cache().getHandle(inum, false);  // 直接通过 get() 获取 Handle（ref_count >= 1，不会被驱逐），然后就地初始化
    if (!handle) 
    {
        log_err("[ic_alloc_inode()] Fail to allocate a new inode CacheEntry");
        if (new_alloc_inum) free_inum(inum);
        return InodeHandle();
    }

    {
        auto write_acc = handle.write_access();   // 获取排他写锁，对 inode 的内容进行初始化
        memset(static_cast<DInode*>(&(*write_acc)), 0, sizeof(DInode));
        write_acc->init_runtime_state();
        write_acc->idx = inum;
        write_acc->used = true;
        write_acc->type = type;
        write_acc->nlink = 1;    // 肯定是因为创建了文件所以才创建这个 inode，因此有文件名，硬链接数初始时为 1
        write_acc->file_size = 0;
        write_acc->indirect_extent_block = sb.indirect_block_start + inum;
        mark_inode_metadata_dirty(&*write_acc);
        write_acc.mark_dirty();
        atomic_write(&handle.get_entry()->valid, 1);
    }

    return handle;  // Handle 持有 ref_count，全程不会降为 0
}

bool ic_free_inode(int inum)   // 释放该 inode 持有的所有资源，inum 可被再次使用
{
    InodeHandle ih = ic_get_inode(inum);
    if (!ih) 
    {
        log_err("Fail to get inode %d", inum);
        return false;
    }

    {
        auto write_acc = ih.write_access();  // 获取排他写锁，防止在销毁时有其他线程试图读取
        if (!write_acc->used) return false;  // 防止被重复删除
        write_acc->drop_dir_index();
        write_acc->used = false;             // 逻辑删除（新的读写请求拿到锁后看到 used == false 会直接退出）

        // 释放 direct extents 中引用的所有物理块
        uint32_t direct_count = direct_extent_count(&(*write_acc));
        for (int i = 0; i < direct_count; i++) free_extent(&write_acc->direct_extents[i]);

        // 释放 indirect extents 中引用的所有物理块
        if (uses_indirect_block(&(*write_acc)))
        {
            BlockHandle ind_bh = bc_get_handle(write_acc->indirect_extent_block);
            if (ind_bh)
            {
                auto ind_acc = ind_bh.write_access();
                const iExtent* ind_exts = reinterpret_cast<const iExtent*>(ind_acc->data);
                uint32_t indirect_count = indirect_extent_count(&(*write_acc));
                for (int i = 0; i < indirect_count; i++) free_extent(&ind_exts[i]);

                memset(ind_acc->data, 0, BLOCK_SIZE);
                ind_acc.mark_dirty();
            }
        }

        write_acc->type      = UNKNOWN;
        write_acc->nlink     = 0;
        write_acc->file_size = 0;
        write_acc->valid_extent_count = 0;
        memset(write_acc->direct_extents, 0, sizeof(write_acc->direct_extents));
        memset(&write_acc->extent_hint, 0, sizeof(write_acc->extent_hint));
        mark_inode_metadata_dirty(&*write_acc);
        
        {
            SpinGuardNP g(&write_acc->dirty_lock);
            write_acc->clear_dirty_data_unlocked();
            write_acc->dirty_data_seq++;
        }

        write_acc.mark_dirty();
    }

    free_inum(inum);

    return true;
}

bool ic_flush_inode(int inum)
{
    // 刷写 inode cache entry 自身（将 MInode 数据写回 inode table block）
    if (!get_inode_cache().flush_entry(inum)) return false;

    // inode 写回后，inode table block 在 block cache 中也变脏了，需要一并刷写
    BlockID itable_blk = sb.itable_blockstart + inum / INODENUM_PER_BLOCK;
    return bc_flush_block(itable_blk);
}

void ic_flush_all() { get_inode_cache().flush_all(); }