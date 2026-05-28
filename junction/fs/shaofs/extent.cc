#include "extent.h"
#include "blockCache.h"
#include "group.h"
#include "inode.h"
#include <algorithm>
#include <cstring>
#include "inodeCache.h"
#include "journal.h"

extern "C" {
#include "../runtime/defs.h"
}

static constexpr int kLegacyMaxExtents = static_cast<int>(LEGACY_MAX_EXTENT_NUM);

static constexpr int kAppendPreallocSmallBlocks = 16;
static constexpr int kAppendPreallocMediumBlocks = 64;
static constexpr int kAppendPreallocMaxBlocks = 128;
static constexpr int kMaxInodeExtents = DIRECT_EXTENT_NUM + EXTENT_TREE_ROOT_REFS * EXTENT_TREE_LEAF_EXTENTS;
static constexpr int kExtentScratchSlots = 8;

struct ExtentScratch {
    volatile int busy;
    iExtent extents[kMaxInodeExtents + kAppendPreallocMaxBlocks];
};

static ExtentScratch extent_scratch[kExtentScratchSlots];
static bool extent_scratch_ready;

static void init_extent_scratch_once()
{
    if (likely(extent_scratch_ready)) return;

    static spinlock_t init_lock = SPINLOCK_INITIALIZER;
    SpinGuardNP g(&init_lock);
    if (extent_scratch_ready) return;

    for (int i = 0; i < kExtentScratchSlots; i++)
        extent_scratch[i].busy = 0;
    extent_scratch_ready = true;
}

class ExtentScratchGuard {
    ExtentScratch* scratch_;

public:
    ExtentScratchGuard()
    {
        init_extent_scratch_once();
        unsigned int cpu;
        {
            kguard k;
            cpu = k->curr_cpu;
        }
        unsigned int start = cpu % kExtentScratchSlots;
        while (true)
        {
            for (int i = 0; i < kExtentScratchSlots; i++)
            {
                ExtentScratch* candidate = &extent_scratch[(start + i) % kExtentScratchSlots];
                int expected = 0;
                if (__atomic_compare_exchange_n(&candidate->busy, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                {
                    scratch_ = candidate;
                    return;
                }
            }
            thread_yield();
        }
    }

    ~ExtentScratchGuard() { __atomic_store_n(&scratch_->busy, 0, __ATOMIC_RELEASE); }

    iExtent* data() { return scratch_->extents; }

    ExtentScratchGuard(const ExtentScratchGuard&) = delete;
    ExtentScratchGuard& operator=(const ExtentScratchGuard&) = delete;
};

static inline BlockID first_block_after_eof(const MInode* inode)
{
    return (inode->file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

static inline bool should_prealloc_append(const MInode* inode, BlockID logical_blk)
{
    (void)inode;
    (void)logical_blk;
    return false;
}

static inline int append_prealloc_blocks(const MInode* inode)
{
    if (inode->file_size < 64ull * 1024)   return kAppendPreallocSmallBlocks;
    if (inode->file_size < 1024ull * 1024) return kAppendPreallocMediumBlocks;
    return kAppendPreallocMaxBlocks;
}

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

static inline ExtentTreeHeader* tree_header(void* data)
{
    return reinterpret_cast<ExtentTreeHeader*>(data);
}

static inline const ExtentTreeHeader* tree_header(const void* data)
{
    return reinterpret_cast<const ExtentTreeHeader*>(data);
}

static inline ExtentLeafRef* tree_refs(void* data)
{
    return reinterpret_cast<ExtentLeafRef*>(static_cast<char*>(data) + sizeof(ExtentTreeHeader));
}

static inline const ExtentLeafRef* tree_refs(const void* data)
{
    return reinterpret_cast<const ExtentLeafRef*>(static_cast<const char*>(data) + sizeof(ExtentTreeHeader));
}

static inline ExtentLeafHeader* leaf_header(void* data)
{
    return reinterpret_cast<ExtentLeafHeader*>(data);
}

static inline const ExtentLeafHeader* leaf_header(const void* data)
{
    return reinterpret_cast<const ExtentLeafHeader*>(data);
}

static inline iExtent* leaf_extents(void* data)
{
    return reinterpret_cast<iExtent*>(static_cast<char*>(data) + sizeof(ExtentLeafHeader));
}

static inline const iExtent* leaf_extents(const void* data)
{
    return reinterpret_cast<const iExtent*>(static_cast<const char*>(data) + sizeof(ExtentLeafHeader));
}

static inline bool root_valid(const ExtentTreeHeader* hdr)
{
    return hdr->magic == EXTENT_TREE_ROOT_MAGIC && hdr->version == EXTENT_TREE_VERSION && hdr->leaf_count <= EXTENT_TREE_ROOT_REFS && hdr->indirect_extent_count <= (uint32_t)EXTENT_TREE_ROOT_REFS * (uint32_t)EXTENT_TREE_LEAF_EXTENTS;
}

static inline bool leaf_valid(const ExtentLeafHeader* hdr)
{
    return hdr->magic == EXTENT_TREE_LEAF_MAGIC && hdr->version == EXTENT_TREE_VERSION && hdr->extent_count <= EXTENT_TREE_LEAF_EXTENTS;
}

static int find_leaf_ref(const ExtentLeafRef* refs, uint32_t leaf_count, BlockID logical_blk)
{
    int left = 0, right = (int)leaf_count - 1;
    int best = -1;
    while (left <= right)
    {
        int mid = left + (right - left) / 2;
        if (refs[mid].logical_start <= logical_blk)
        {
            best = mid;
            left = mid + 1;
        }
        else right = mid - 1;
    }
    return best;
}

static bool collect_all_extents(const MInode* inode, iExtent* out, int capacity, int* out_count)
{
    if (!out || !out_count || capacity <= 0) return false;
    *out_count = 0;

    auto append_extent = [&] (const iExtent& ext) -> bool {
        if (*out_count >= capacity) return false;
        out[(*out_count)++] = ext;
        return true;
    };

    uint32_t direct_cnt = direct_extent_count(inode);
    for (uint32_t i = 0; i < direct_cnt; ++i)
        if (!append_extent(inode->direct_extents[i])) return false;

    if (!uses_indirect_block(inode)) return true;

    BlockHandle root_bh = bc_get_handle(inode->indirect_extent_block);
    if (unlikely(!root_bh))
    {
        log_err("[extent] failed to get extent metadata block %lu for inode %d", inode->indirect_extent_block, inode->idx);
        return false;
    }

    auto root_acc = root_bh.read_access();
    if (!uses_extent_tree(inode))
    {
        const iExtent* exts = reinterpret_cast<const iExtent*>(root_acc->data);
        uint32_t ind_cnt = legacy_indirect_extent_count(inode);
        for (uint32_t i = 0; i < ind_cnt; ++i)
            if (!append_extent(exts[i])) return false;
        return true;
    }

    const ExtentTreeHeader* hdr = tree_header(root_acc->data);
    if (!root_valid(hdr))
    {
        log_err("[extent] invalid extent tree root for inode %d", inode->idx);
        return false;
    }

    const ExtentLeafRef* refs = tree_refs(root_acc->data);
    for (uint32_t r = 0; r < hdr->leaf_count; r++)
    {
        BlockHandle leaf_bh = bc_get_handle(refs[r].leaf_block);
        if (unlikely(!leaf_bh))
        {
            log_err("[extent] failed to read extent leaf %lu for inode %d", refs[r].leaf_block, inode->idx);
            return false;
        }

        auto leaf_acc = leaf_bh.read_access();
        const ExtentLeafHeader* leaf = leaf_header(leaf_acc->data);
        if (!leaf_valid(leaf) || leaf->extent_count != refs[r].extent_count)
        {
            log_err("[extent] invalid extent leaf %lu for inode %d", refs[r].leaf_block, inode->idx);
            return false;
        }

        const iExtent* exts = leaf_extents(leaf_acc->data);
        for (uint32_t i = 0; i < leaf->extent_count; i++)
            if (!append_extent(exts[i])) return false;
    }

    return *out_count == static_cast<int>(inode->valid_extent_count);
}

static bool write_legacy_extents(MInode* inode, const iExtent* all_extents, int ext_count)
{
    if (ext_count > kLegacyMaxExtents) return false;

    BlockHandle ind_bh;
    if (ext_count > DIRECT_EXTENT_NUM)
    {
        ind_bh = bc_get_handle(inode->indirect_extent_block);
        if (unlikely(!ind_bh))
        {
            log_err("[extent] failed to fetch indirect block %lu for inode %d", inode->indirect_extent_block, inode->idx);
            return false;
        }
    }

    int global_idx = 0;
    for (int i = 0; i < DIRECT_EXTENT_NUM; ++i)
    {
        if (global_idx < ext_count) inode->direct_extents[i] = all_extents[global_idx++];
        else { memset(&inode->direct_extents[i], 0, sizeof(iExtent) * (DIRECT_EXTENT_NUM - i)); break; }
    }

    if (ext_count > DIRECT_EXTENT_NUM)
    {
        auto acc = ind_bh.write_access();
        iExtent* exts = reinterpret_cast<iExtent*>(acc->data);
        for (int i = 0; i < static_cast<int>(EXTENTS_PER_BLOCK); ++i)
        {
            if (global_idx < ext_count) exts[i] = all_extents[global_idx++];
            else { memset(&exts[i], 0, sizeof(iExtent) * (EXTENTS_PER_BLOCK - i)); break; }
        }
        acc.mark_dirty();
    }

    inode->valid_extent_count = ext_count;
    mark_inode_metadata_dirty(inode);
    return true;
}

static bool write_tree_leaf(BlockID leaf_block, const iExtent* exts, uint32_t count)
{
    journal_register_metadata_block(leaf_block);

    BlockHandle leaf_bh = bc_get_handle(leaf_block, false);
    if (unlikely(!leaf_bh)) return false;

    auto acc = leaf_bh.write_access();
    memset(acc->data, 0, BLOCK_SIZE);
    ExtentLeafHeader* hdr = leaf_header(acc->data);
    hdr->magic = EXTENT_TREE_LEAF_MAGIC;
    hdr->version = EXTENT_TREE_VERSION;
    hdr->extent_count = count;
    memcpy(leaf_extents(acc->data), exts, sizeof(iExtent) * count);
    acc.mark_dirty();
    atomic_write(&leaf_bh.get_entry()->valid, 1);
    return true;
}

static bool write_tree_from_sorted(MInode* inode, const iExtent* all_extents, int ext_count)
{
    if (ext_count <= kLegacyMaxExtents) return write_legacy_extents(inode, all_extents, ext_count);

    uint32_t indirect_count = ext_count - DIRECT_EXTENT_NUM;
    uint32_t leaf_count = (indirect_count + EXTENT_TREE_LEAF_EXTENTS - 1) / EXTENT_TREE_LEAF_EXTENTS;
    if (leaf_count > EXTENT_TREE_ROOT_REFS)
    {
        log_err("[extent] extent tree root overflow for inode %d: extents=%d", inode->idx, ext_count);
        return false;
    }

    BlockID leaf_blocks[EXTENT_TREE_ROOT_REFS];
    int allocated = alloc_blocks(leaf_blocks, leaf_count);
    if (allocated != (int)leaf_count)
    {
        for (int i = 0; i < allocated; i++) free_block(leaf_blocks[i]);
        log_err("[extent] failed to allocate %u extent tree leaves for inode %d", leaf_count, inode->idx);
        return false;
    }

    BlockHandle root_bh = bc_get_handle(inode->indirect_extent_block);
    if (unlikely(!root_bh))
    {
        for (uint32_t i = 0; i < leaf_count; i++) free_block(leaf_blocks[i]);
        return false;
    }

    uint32_t pos = DIRECT_EXTENT_NUM;
    for (uint32_t i = 0; i < leaf_count; i++)
    {
        uint32_t cnt = MIN((uint32_t)EXTENT_TREE_LEAF_EXTENTS, indirect_count - (pos - DIRECT_EXTENT_NUM));
        if (!write_tree_leaf(leaf_blocks[i], all_extents + pos, cnt))
        {
            for (uint32_t j = 0; j < leaf_count; j++)
            {
                bc_invalidate_block(leaf_blocks[j]);
                free_block(leaf_blocks[j]);
            }
            return false;
        }
        pos += cnt;
    }

    {
        auto root_acc = root_bh.write_access();
        memset(root_acc->data, 0, BLOCK_SIZE);
        ExtentTreeHeader* hdr = tree_header(root_acc->data);
        ExtentLeafRef* refs = tree_refs(root_acc->data);
        hdr->magic = EXTENT_TREE_ROOT_MAGIC;
        hdr->version = EXTENT_TREE_VERSION;
        hdr->leaf_count = leaf_count;
        hdr->indirect_extent_count = indirect_count;

        pos = DIRECT_EXTENT_NUM;
        for (uint32_t i = 0; i < leaf_count; i++)
        {
            uint32_t cnt = MIN((uint32_t)EXTENT_TREE_LEAF_EXTENTS, indirect_count - (pos - DIRECT_EXTENT_NUM));
            refs[i].logical_start = all_extents[pos].logical_start;
            refs[i].leaf_block = leaf_blocks[i];
            refs[i].extent_count = cnt;
            pos += cnt;
        }
        root_acc.mark_dirty();
    }

    for (int i = 0; i < DIRECT_EXTENT_NUM; i++) inode->direct_extents[i] = all_extents[i];
    inode->valid_extent_count = ext_count;
    mark_inode_metadata_dirty(inode);
    return true;
}

static bool bmap_lookup_tree(MInode* inode, BlockID logical_blk, iExtent* found)
{
    BlockHandle root_bh = bc_get_handle(inode->indirect_extent_block);
    if (unlikely(!root_bh))
    {
        log_err("[extent] failed to get extent tree root %lu for inode %d", inode->indirect_extent_block, inode->idx);
        return false;
    }

    auto root_acc = root_bh.read_access();
    const ExtentTreeHeader* hdr = tree_header(root_acc->data);
    if (!root_valid(hdr))
    {
        log_err("[extent] invalid extent tree root for inode %d", inode->idx);
        return false;
    }

    const ExtentLeafRef* refs = tree_refs(root_acc->data);
    int ref_idx = find_leaf_ref(refs, hdr->leaf_count, logical_blk);
    if (ref_idx < 0) return false;

    ExtentLeafRef ref = refs[ref_idx];
    BlockHandle leaf_bh = bc_get_handle(ref.leaf_block);
    if (unlikely(!leaf_bh)) return false;

    auto leaf_acc = leaf_bh.read_access();
    const ExtentLeafHeader* leaf = leaf_header(leaf_acc->data);
    if (!leaf_valid(leaf)) return false;

    iExtent ext;
    BlockID phys = lookup_extent(leaf_extents(leaf_acc->data), leaf->extent_count, logical_blk, &ext);
    if (phys == INVALID_BLOCK_ID) return false;

    if (found) *found = ext;
    return true;
}

static bool bmap_lookup_extent(MInode* inode, BlockID logical_blk, iExtent* found)
{
    if (!found || inode->valid_extent_count == 0) return false;

    if (spin_try_lock_np(&inode->hint_lock))
    {
        iExtent hint = inode->extent_hint;
        spin_unlock_np(&inode->hint_lock);
        if (block_in_extent(logical_blk, hint))
        {
            *found = hint;
            return true;
        }
    }

    uint32_t total_exts = inode->valid_extent_count, direct_count = direct_extent_count(inode);
    if (total_exts == 1)
    {
        const iExtent& ext = inode->direct_extents[0];
        if (block_in_extent(logical_blk, ext))
        {
            *found = ext;
            update_extent_hint(inode, ext);
            return true;
        }
    }

    iExtent ext;
    if (lookup_extent(inode->direct_extents, direct_count, logical_blk, &ext) != INVALID_BLOCK_ID)
    {
        *found = ext;
        update_extent_hint(inode, ext);
        return true;
    }

    if (!uses_indirect_block(inode)) return false;

    if (uses_extent_tree(inode))
    {
        if (!bmap_lookup_tree(inode, logical_blk, &ext)) return false;
        *found = ext;
        update_extent_hint(inode, ext);
        return true;
    }

    BlockHandle ind_bh = bc_get_handle(inode->indirect_extent_block);
    if (unlikely(!ind_bh))
    {
        log_err("[bmap_lookup_extent()] Failed to get indirect block [%lu] for inode %d", inode->indirect_extent_block, inode->idx);
        return false;
    }

    auto acc = ind_bh.read_access();
    const iExtent* ind_exts = reinterpret_cast<const iExtent*>(acc->data);
    if (lookup_extent(ind_exts, legacy_indirect_extent_count(inode), logical_blk, &ext) == INVALID_BLOCK_ID) return false;

    *found = ext;
    update_extent_hint(inode, ext);
    return true;
}

static bool tree_load_last_extent(MInode* inode, iExtent* out)
{
    if (!out) return false;

    BlockHandle root_bh = bc_get_handle(inode->indirect_extent_block);
    if (unlikely(!root_bh)) return false;

    auto root_acc = root_bh.read_access();
    const ExtentTreeHeader* hdr = tree_header(root_acc->data);
    if (!root_valid(hdr) || hdr->leaf_count == 0) return false;

    const ExtentLeafRef* refs = tree_refs(root_acc->data);
    ExtentLeafRef ref = refs[hdr->leaf_count - 1];
    if (ref.extent_count == 0) return false;

    BlockHandle leaf_bh = bc_get_handle(ref.leaf_block);
    if (unlikely(!leaf_bh)) return false;

    auto leaf_acc = leaf_bh.read_access();
    const ExtentLeafHeader* leaf = leaf_header(leaf_acc->data);
    if (!leaf_valid(leaf) || leaf->extent_count == 0) return false;

    *out = leaf_extents(leaf_acc->data)[leaf->extent_count - 1];
    return true;
}

static bool tree_append_new_extent(MInode* inode, const iExtent& new_ext)
{
    BlockHandle root_bh = bc_get_handle(inode->indirect_extent_block);
    if (unlikely(!root_bh)) return false;

    auto root_acc = root_bh.write_access();
    ExtentTreeHeader* hdr = tree_header(root_acc->data);
    if (!root_valid(hdr) || hdr->leaf_count == 0) return false;

    ExtentLeafRef* refs = tree_refs(root_acc->data);
    ExtentLeafRef& ref = refs[hdr->leaf_count - 1];
    if (ref.extent_count < EXTENT_TREE_LEAF_EXTENTS)
    {
        BlockHandle leaf_bh = bc_get_handle(ref.leaf_block);
        if (unlikely(!leaf_bh)) return false;

        auto leaf_acc = leaf_bh.write_access();
        ExtentLeafHeader* leaf = leaf_header(leaf_acc->data);
        if (!leaf_valid(leaf) || leaf->extent_count != ref.extent_count) return false;

        leaf_extents(leaf_acc->data)[leaf->extent_count++] = new_ext;
        ref.extent_count++;
        hdr->indirect_extent_count++;
        inode->valid_extent_count++;
        leaf_acc.mark_dirty();
        root_acc.mark_dirty();
        mark_inode_metadata_dirty(inode);
        update_extent_hint(inode, new_ext);
        return true;
    }

    if (hdr->leaf_count >= EXTENT_TREE_ROOT_REFS) return false;

    BlockID leaf_block = alloc_block();
    if (leaf_block == INVALID_BLOCK_ID) return false;
    if (!write_tree_leaf(leaf_block, &new_ext, 1))
    {
        bc_invalidate_block(leaf_block);
        free_block(leaf_block);
        return false;
    }

    refs[hdr->leaf_count] = {
        .logical_start = new_ext.logical_start,
        .leaf_block = leaf_block,
        .extent_count = 1,
        .reserved = 0,
    };
    hdr->leaf_count++;
    hdr->indirect_extent_count++;
    inode->valid_extent_count++;
    root_acc.mark_dirty();
    mark_inode_metadata_dirty(inode);
    update_extent_hint(inode, new_ext);
    return true;
}

static BlockID bmap_lookup(MInode* inode, BlockID logical_blk)   // 在 inode 中查找这个 logical_blk，若能找到，则返回对应的实际物理块号
{
    iExtent found;
    if (!bmap_lookup_extent(inode, logical_blk, &found)) return INVALID_BLOCK_ID;
    return found.physical_start + (logical_blk - found.logical_start);
}

bool inode_lookup_extent_locked(MInode* inode_ptr, BlockID logical_blk, iExtent* out_extent)
{
    return bmap_lookup_extent(inode_ptr, logical_blk, out_extent);
}

static bool bmap_try_append_extent(MInode* inode, const iExtent& new_ext)  // 尝试追加到末尾 extent 中
{
    if (new_ext.block_count == 0) return false;
    if (inode->valid_extent_count == 0)    // 当前这个 inode 还没有任何分配的块
    {
        inode->direct_extents[0] = new_ext;
        inode->valid_extent_count = 1;
        mark_inode_metadata_dirty(inode);
        update_extent_hint(inode, new_ext);
        return true;
    }

    if (uses_extent_tree(inode))
    {
        iExtent last;
        if (!tree_load_last_extent(inode, &last)) return false;
        if (last.logical_start + last.block_count == new_ext.logical_start && last.physical_start + last.block_count == new_ext.physical_start)
        {
            BlockHandle root_bh = bc_get_handle(inode->indirect_extent_block);
            if (unlikely(!root_bh)) return false;

            auto root_acc = root_bh.write_access();
            ExtentTreeHeader* hdr = tree_header(root_acc->data);
            if (!root_valid(hdr) || hdr->leaf_count == 0) return false;
            ExtentLeafRef* refs = tree_refs(root_acc->data);
            ExtentLeafRef& ref = refs[hdr->leaf_count - 1];

            BlockHandle leaf_bh = bc_get_handle(ref.leaf_block);
            if (unlikely(!leaf_bh)) return false;

            auto leaf_acc = leaf_bh.write_access();
            ExtentLeafHeader* leaf = leaf_header(leaf_acc->data);
            if (!leaf_valid(leaf) || leaf->extent_count == 0) return false;

            iExtent& leaf_last = leaf_extents(leaf_acc->data)[leaf->extent_count - 1];
            if (leaf_last.logical_start + leaf_last.block_count != new_ext.logical_start || leaf_last.physical_start + leaf_last.block_count != new_ext.physical_start) return false;

            leaf_last.block_count += new_ext.block_count;
            leaf_acc.mark_dirty();
            mark_inode_metadata_dirty(inode);
            update_extent_hint(inode, leaf_last);
            return true;
        }

        if (new_ext.logical_start > last.logical_start) return tree_append_new_extent(inode, new_ext);
        return false;
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
            iExtent& last = exts[legacy_indirect_extent_count(inode) - 1];

            if (last.logical_start + last.block_count == new_ext.logical_start && last.physical_start + last.block_count == new_ext.physical_start)
            {
                last.block_count += new_ext.block_count;
                acc.mark_dirty();
                mark_inode_metadata_dirty(inode);
                update_extent_hint(inode, last);
                return true;
            }

            if (new_ext.logical_start > last.logical_start && inode->valid_extent_count < LEGACY_MAX_EXTENT_NUM)
            {
                exts[legacy_indirect_extent_count(inode)] = new_ext;
                inode->valid_extent_count++;
                acc.mark_dirty();
                mark_inode_metadata_dirty(inode);
                update_extent_hint(inode, new_ext);
                return true;
            }
        }
    }
    else   // 追加到 direct extents 中
    {
        iExtent& last = inode->direct_extents[direct_extent_count(inode) - 1];
        if (last.logical_start + last.block_count == new_ext.logical_start && last.physical_start + last.block_count == new_ext.physical_start)
        {
            last.block_count += new_ext.block_count;
            mark_inode_metadata_dirty(inode);
            update_extent_hint(inode, last);
            return true;
        }
        if (new_ext.logical_start > last.logical_start && inode->valid_extent_count < DIRECT_EXTENT_NUM)
        {
            inode->direct_extents[inode->valid_extent_count++] = new_ext;
            mark_inode_metadata_dirty(inode);
            update_extent_hint(inode, new_ext);
            return true;
        }
    }

    return false; // 无法简单追加，交由 Slow Path 处理
}

static bool bmap_try_append(MInode* inode, BlockID logical_blk, BlockID phys_blk)
{
    return bmap_try_append_extent(inode, { .logical_start = logical_blk, .physical_start = phys_blk, .block_count = 1 });
}

static bool bmap_insert_compact_extents(MInode* inode, const iExtent* new_exts, int new_count, BlockID hint_logical_blk, bool report_overflow)
{
    ExtentScratchGuard scratch;
    iExtent* all = scratch.data();
    int ext_count = 0;
    if (!collect_all_extents(inode, all, kMaxInodeExtents + kAppendPreallocMaxBlocks, &ext_count)) return false;

    for (int i = 0; i < new_count; i++)
    {
        if (new_exts[i].block_count == 0) continue;
        if (ext_count >= kMaxInodeExtents + kAppendPreallocMaxBlocks) return false;
        all[ext_count++] = new_exts[i];
    }

    ext_count = compact_extents_inplace(all, ext_count);

    bool was_tree = uses_extent_tree(inode);
    if (!write_tree_from_sorted(inode, all, ext_count))
    {
        if (report_overflow) log_err("[extent] Extent tree overflow for inode %d!", inode->idx);
        return false;
    }
    if (!was_tree && uses_extent_tree(inode)) log_info("[extent] inode %d converted to extent tree (%d extents)", inode->idx, ext_count);

    for (int i = 0; i < ext_count; i++)
    {
        if (block_in_extent(hint_logical_blk, all[i]))
        {
            update_extent_hint(inode, all[i]);
            break;
        }
    }
    return true;
}

static bool bmap_insert_compact(MInode* inode, BlockID logical_blk, BlockID phys_blk)
{
    iExtent new_ext = { .logical_start = logical_blk, .physical_start = phys_blk, .block_count = 1 };
    return bmap_insert_compact_extents(inode, &new_ext, 1, logical_blk, true);
}

static int build_physical_runs(BlockID logical_start, const BlockID* blocks, int count, iExtent* runs)
{
    int run_count = 0;
    int pos = 0;
    while (pos < count)
    {
        int len = 1;
        while (pos + len < count && blocks[pos + len] == blocks[pos] + len) len++;
        runs[run_count++] = { .logical_start = logical_start + (BlockID)pos, .physical_start = blocks[pos], .block_count = (uint64_t)len, };
        pos += len;
    }
    return run_count;
}

static void free_physical_runs(const iExtent* runs, int run_count)
{
    for (int i = 0; i < run_count; i++) free_extent(&runs[i]);
}

static void discard_preallocated_blocks(const BlockID* blocks, int block_count, const iExtent* runs, int run_count)
{
    for (int i = 0; i < block_count; i++) bc_invalidate_block(blocks[i]);
    free_physical_runs(runs, run_count);
}

int inode_append_run_locked(MInode* inode, BlockID logical_start, int max_blocks, BlockID* out_blocks)
{
    if (!inode || !out_blocks || max_blocks <= 0) return 0;
    max_blocks = MIN(max_blocks, kAppendPreallocMaxBlocks);

    int mapped = 0;
    while (mapped < max_blocks)
    {
        BlockID phys = bmap_lookup(inode, logical_start + (BlockID)mapped);
        if (phys == INVALID_BLOCK_ID) break;
        out_blocks[mapped++] = phys;
    }
    if (mapped == max_blocks) return mapped;

    BlockID new_blocks[kAppendPreallocMaxBlocks];
    iExtent runs[kAppendPreallocMaxBlocks];
    int want = max_blocks - mapped;
    int got = alloc_blocks(new_blocks, want);
    if (got <= 0) return mapped;

    int run_count = build_physical_runs(logical_start + (BlockID)mapped, new_blocks, got, runs);
    bool inserted = false;
    if (run_count == 1 && bmap_try_append_extent(inode, runs[0]))
        inserted = true;
    else
        inserted = bmap_insert_compact_extents(inode, runs, run_count, logical_start + (BlockID)mapped, true);

    if (!inserted)
    {
        discard_preallocated_blocks(new_blocks, got, runs, run_count);
        return mapped;
    }

    for (int i = 0; i < got; i++)
        out_blocks[mapped + i] = new_blocks[i];
    return mapped + got;
}

static BlockID bmap_prealloc_append(MInode* inode, BlockID logical_blk)
{
    BlockID blocks[kAppendPreallocMaxBlocks];
    iExtent runs[kAppendPreallocMaxBlocks];

    int want = append_prealloc_blocks(inode);
    int got = alloc_blocks(blocks, want);
    if (got <= 0) return INVALID_BLOCK_ID;

    int run_count = build_physical_runs(logical_blk, blocks, got, runs);
    if (run_count != 1 || !bmap_try_append_extent(inode, runs[0]))
    {
        if (!bmap_insert_compact_extents(inode, runs, run_count, logical_blk, false))
        {
            discard_preallocated_blocks(blocks, got, runs, run_count);
            return INVALID_BLOCK_ID;
        }
    }

    return blocks[0];
}

bool inode_for_each_extent(const MInode* inode, bool include_direct, bool (*cb)(const iExtent&, void*), void* arg)
{
    if (!inode || !cb) return false;

    uint32_t direct_cnt = direct_extent_count(inode);
    if (include_direct)
    {
        for (uint32_t i = 0; i < direct_cnt; i++)
            if (!cb(inode->direct_extents[i], arg)) return false;
    }

    if (!uses_indirect_block(inode)) return true;

    BlockHandle root_bh = bc_get_handle(inode->indirect_extent_block);
    if (!root_bh) return false;

    auto root_acc = root_bh.read_access();
    if (!uses_extent_tree(inode))
    {
        const iExtent* exts = reinterpret_cast<const iExtent*>(root_acc->data);
        uint32_t cnt = legacy_indirect_extent_count(inode);
        for (uint32_t i = 0; i < cnt; i++)
            if (!cb(exts[i], arg)) return false;
        return true;
    }

    const ExtentTreeHeader* hdr = tree_header(root_acc->data);
    if (!root_valid(hdr)) return false;
    const ExtentLeafRef* refs = tree_refs(root_acc->data);
    for (uint32_t r = 0; r < hdr->leaf_count; r++)
    {
        BlockHandle leaf_bh = bc_get_handle(refs[r].leaf_block);
        if (!leaf_bh) return false;

        auto leaf_acc = leaf_bh.read_access();
        const ExtentLeafHeader* leaf = leaf_header(leaf_acc->data);
        if (!leaf_valid(leaf) || leaf->extent_count != refs[r].extent_count) return false;
        const iExtent* exts = leaf_extents(leaf_acc->data);
        for (uint32_t i = 0; i < leaf->extent_count; i++)
            if (!cb(exts[i], arg)) return false;
    }

    return true;
}

bool inode_for_each_extent_metadata_block(const MInode* inode, bool (*cb)(BlockID, void*), void* arg)
{
    if (!inode || !cb || !uses_indirect_block(inode)) return true;
    if (!cb(inode->indirect_extent_block, arg)) return false;
    if (!uses_extent_tree(inode)) return true;

    BlockHandle root_bh = bc_get_handle(inode->indirect_extent_block);
    if (!root_bh) return false;

    auto root_acc = root_bh.read_access();
    const ExtentTreeHeader* hdr = tree_header(root_acc->data);
    if (!root_valid(hdr)) return false;
    const ExtentLeafRef* refs = tree_refs(root_acc->data);
    for (uint32_t i = 0; i < hdr->leaf_count; i++)
        if (!cb(refs[i].leaf_block, arg)) return false;
    return true;
}

static bool flush_metadata_block_cb(BlockID block, void*)
{
    return bc_flush_block_batched(block);
}

bool inode_flush_extent_metadata(const MInode* inode)
{
    return inode_for_each_extent_metadata_block(inode, flush_metadata_block_cb, nullptr);
}

bool inode_free_extent_metadata(MInode* inode)
{
    if (!inode || !uses_extent_tree(inode)) return true;

    BlockHandle root_bh = bc_get_handle(inode->indirect_extent_block);
    if (!root_bh) return false;

    BlockID leaves[EXTENT_TREE_ROOT_REFS];
    uint32_t leaf_count = 0;
    {
        auto root_acc = root_bh.read_access();
        const ExtentTreeHeader* hdr = tree_header(root_acc->data);
        if (!root_valid(hdr)) return false;
        const ExtentLeafRef* refs = tree_refs(root_acc->data);
        leaf_count = hdr->leaf_count;
        for (uint32_t i = 0; i < leaf_count; i++) leaves[i] = refs[i].leaf_block;
    }

    for (uint32_t i = 0; i < leaf_count; i++)
    {
        bc_invalidate_block(leaves[i]);
        free_block(leaves[i]);
    }

    {
        auto root_acc = root_bh.write_access();
        memset(root_acc->data, 0, BLOCK_SIZE);
        root_acc.mark_dirty();
    }

    return true;
}

// 约定：调用此函数前，caller 必须持有该 inode 的读锁（无需allocate）或写锁（需要allocate）
BlockID inode_bmap_locked(MInode* inode_ptr, BlockID logical_blk, bool allocate, bool* is_new)
{
    if (is_new) *is_new = false;

    BlockID phys_blk = bmap_lookup(inode_ptr, logical_blk);  // 查找 inode 中是否有该逻辑块号，若有则返回相应的物理块号
    if (phys_blk != INVALID_BLOCK_ID)
    {
        if (allocate && is_new && logical_blk >= first_block_after_eof(inode_ptr)) *is_new = true;
        return phys_blk;
    }

    if (!allocate) return INVALID_BLOCK_ID;    // 该 inode 中没有该逻辑块号

    if (should_prealloc_append(inode_ptr, logical_blk))
    {
        BlockID new_phys_blk = bmap_prealloc_append(inode_ptr, logical_blk);
        if (new_phys_blk != INVALID_BLOCK_ID)
        {
            if (is_new) *is_new = true;
            return new_phys_blk;
        }
    }

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
