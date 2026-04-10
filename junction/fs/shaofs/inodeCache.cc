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

    size_t total_mem = GlobalInodeCache::calculate_total_memory(capacity, shard_num);
    size_t align     = GlobalInodeCache::calculate_alignment();
    
    char* buffer = (char*)malloc(total_mem);
    if (unlikely(!buffer)) 
    { 
        log_err("[inode_cache_init] ERROR: malloc failed!");
        buffer = nullptr;
    }

    // g_inode_cache_ptr = new GlobalInodeCache(
    //     capacity, shard_num, 
    //     &InodeBackend::getInstance(), 
    //     LRUPolicy<int>::create_policy_instance, 
    //     buffer
    // );
    g_inode_cache_ptr = new GlobalInodeCache(
        capacity, shard_num, 
        &InodeBackend::getInstance(), 
        []() { return new LRUPolicy<int, MInode>(); }, 
        buffer
    );
}

InodeHandle ic_get_inode(int inum) { return get_inode_cache().getHandle(inum); }

InodeHandle ic_alloc_inode(file_type_t type, int inum)    // 如果 inum != -1 则尝试分配指定的 inum，否则分配一个新的 inum
{
    if (inum == -1) inum = alloc_inum();
    if (inum == -1) return InodeHandle(); // 返回空 Handle

    // // 构造一个新的盘上 Inode 数据
    // MInode2 new_inode;
    // memset(static_cast<DInode*>(&new_inode), 0, sizeof(DInode));

    // new_inode.idx = inum;
    // new_inode.used = true;
    // new_inode.type = type;
    // new_inode.nlink = 1;
    // new_inode.file_size = 0;
    // new_inode.indirect_extent_block = sb.indirect_block_start + inum;

    // auto& cache = get_inode_cache();
    // cache.put(inum, new_inode);   // put 会自动获取 Lock，将新数据写入 Cache，并将其标记为 Dirty 和 Valid
    
    // return cache.get(inum);        // 重新 get 一次以返回带有引用计数的 Handle 给调用者

    auto& cache = get_inode_cache();
    InodeHandle handle = cache.getHandle(inum);  // 直接通过 get() 获取 Handle（ref_count >= 1，不会被驱逐），然后就地初始化
    if (!handle) return InodeHandle();

    {
        auto write_acc = handle.write_access();   // 获取排他写锁
        // 就地初始化 inode 数据（覆盖可能从盘上加载的旧数据）
        memset(static_cast<DInode*>(&(*write_acc)), 0, sizeof(DInode));
        write_acc->idx = inum;
        write_acc->used = true;
        write_acc->type = type;
        write_acc->nlink = 1;
        write_acc->file_size = 0;
        write_acc->indirect_extent_block = sb.indirect_block_start + inum;
        write_acc.mark_dirty();
    }

    return handle;  // Handle 持有 ref_count，全程不会降为 0
}

bool ic_free_inode(int inum)   // 释放该 inode 持有的所有资源，inum 可被再次使用
{
    InodeHandle ih = ic_get_inode(inum);
    if (!ih) return false;

    {
        auto write_acc = ih.write_access();  // 获取排他写锁，防止在销毁时有其他线程试图读取
        if (!write_acc->used) return false;  // 防止被重复删除
        write_acc->used = false;        // 逻辑删除（新的读写请求拿到锁后看到 used == false 会直接退出）
        write_acc.mark_dirty();
    }

    {
        auto write_acc = ih.write_access();

        // 释放 direct extents 中引用的所有物理块
        int direct_count = get_valid_extent_count(write_acc->direct_extents, DIRECT_EXTENT_NUM);
        for (int i = 0; i < direct_count; i++)
        {
            const iExtent& ext = write_acc->direct_extents[i];
            for (uint64_t j = 0; j < ext.block_count; j++)
                free_block(ext.physical_start + j);
        }

        // 释放 indirect extents 中引用的所有物理块
        if (direct_count == DIRECT_EXTENT_NUM && write_acc->indirect_extent_block != 0)
        {
            BlockHandle ind_bh = bc_get_handle(write_acc->indirect_extent_block);
            if (ind_bh)
            {
                auto ind_acc = ind_bh.read_access();
                const iExtent* ind_exts = reinterpret_cast<const iExtent*>(ind_acc->data);
                int indirect_count = get_valid_extent_count(ind_exts, EXTENTS_PER_BLOCK);
                for (int i = 0; i < indirect_count; i++)
                {
                    const iExtent& ext = ind_exts[i];
                    for (uint64_t j = 0; j < ext.block_count; j++)
                        free_block(ext.physical_start + j);
                }
            }
        }

        write_acc->type      = UNKNOWN;
        write_acc->nlink     = 0;
        write_acc->file_size = 0;
        memset(write_acc->direct_extents, 0, sizeof(write_acc->direct_extents));
        memset(&write_acc->extent_hint, 0, sizeof(write_acc->extent_hint));

        write_acc.mark_dirty();
    }

    free_inum(inum);

    return true;
}

void ic_flush_all()
{
    get_inode_cache().flush();
}

bool ic_flush_inode(int inum)
{
    // 刷写 inode cache entry 自身（将 MInode 数据写回 inode table block）
    if (!get_inode_cache().flush_entry(inum)) return false;

    // inode 写回后，inode table block 在 block cache 中也变脏了，需要一并刷写
    BlockID itable_blk = sb.itable_blockstart + inum / INODENUM_PER_BLOCK;
    return bc_flush_block(itable_blk);
}

void print_inode_info(int inum) 
{
    InodeHandle handle = ic_get_inode(inum);   // 获取 Inode 的 Handle
    if (!handle) 
    {
        log_err("Failed to read inode %d", inum);
        return;
    }

    {
        // auto acc = handle.access();   // 调用 access() 获取 Accessor 对象
        auto acc = handle.read_access();

        if (!acc->used) 
        {
            log_info("Inode [%d] is strictly not in use.", inum);
            return;
        }

        log_info("---- DInode [%d] ----\nType:      %d\nFile Size: %lu\nHard Link: %u\nIndirect:  %u\n--------------------", acc->idx, acc->type, acc->file_size, acc->nlink, acc->indirect_extent_block);

    } // 离开此作用域时，acc 被析构，底层自动执行 spin_unlock(&entry->mtx)
} // 离开此函数作用域时，handle 被析构，底层自动执行 atomic_dec(&entry->ref_count)