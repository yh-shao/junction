#include "group.h"
#include "fs.h"
#include "blockCache.h"
#include "journal.h"

int core_to_group[NCPU];   // 每个 core 从哪个 group 中分配空闲块
GroupDescExt* group_info;

static inline BlockID group_bitmap_lba(uint32_t gid)    { return sb.group_blockstart + (uint64_t)gid * TOTALBLOCKS_PERGROUP; }
static inline BlockID group_data_startlba(uint32_t gid) { return group_bitmap_lba(gid) + BMAPNUM_PERGROUP; }
void get_group_by_gid(int groupid, BlockID* bitmap, BlockID* datablock_start)
{
	if (bitmap == nullptr || datablock_start == nullptr) { log_info("[Error] get_group_by_gid(): Invalid memory pointer"); exit(1); }
	if (groupid < 0 || groupid >= sb.group_num) { log_info("Error: Invalid groupid %d\n", groupid); exit(1); }

	*bitmap          = group_bitmap_lba(groupid);
	*datablock_start = group_data_startlba(groupid);
}
void get_group_by_blkid(BlockID blk, int* groupid, BlockID* bitmap, BlockID* datablock_start)  // 传入一个物理块号，获取该物理块所属的组的信息
{
	if (groupid == nullptr || bitmap == nullptr || datablock_start == nullptr) { log_info("[Error] get_group_by_blkid(): Invalid memory pointer"); exit(1); }
	if (blk < sb.group_blockstart) { log_info("[Error] get_group_by_blkid(): BlockID %llu is before the start of group managed area.\n", blk); exit(1); }
	if (blk >= sb.total_blocknum) { log_info("[Error] get_group_by_blkid(%lu): BlockID %lu is out of bounds.\n", blk, blk); exit(1); }

	uint64_t blk_off = blk - sb.group_blockstart;
	int gid = blk_off / TOTALBLOCKS_PERGROUP;
	if (gid >= sb.group_num) { log_info("[Error] get_group_by_blkid(): Calculated groupid %d for blk %llu is out of bounds.\n", gid, blk); exit(1); }

	*groupid = gid;
	get_group_by_gid(gid, bitmap, datablock_start);
}

void init_group()  
{
	memset(core_to_group, -1, NCPU * sizeof(int));

    GroupDescriptor* disk_gdt = new GroupDescriptor[sb.group_num]();
    storage_read_obj(disk_gdt, sb.group_num * sizeof(GroupDescriptor), sb.gdt_blockstart, 0);

	group_info = new GroupDescExt[sb.group_num];
	for (uint32_t i = 0; i < sb.group_num; i++) 
    {
        group_info[i].group_id          = i;
        group_info[i].free_blocks_count = disk_gdt[i].free_blocks_count;
        group_info[i].next_free_hint    = disk_gdt[i].next_free_hint;
        group_info[i].flags             = disk_gdt[i].flags;
        group_info[i].owner_count       = 0;
        group_info[i].bitmap_lba        = group_bitmap_lba(i);
        group_info[i].data_start_lba    = group_data_startlba(i);

        if (group_info[i].next_free_hint >= DATABLOCKS_PERGROUP) group_info[i].next_free_hint = 0;
        if (group_info[i].free_blocks_count > DATABLOCKS_PERGROUP) 
        {
            log_warn("warning: GDT[%u].free_blocks_count=%u out of range, clamp to %u\n", i, group_info[i].free_blocks_count, (uint32_t)DATABLOCKS_PERGROUP);
            group_info[i].free_blocks_count = DATABLOCKS_PERGROUP;
        }
    }

    delete[] disk_gdt;
}

static inline void release_group_claim(unsigned int coreid)
{
    if (unlikely(coreid >= NCPU)) { log_err("invalid core id"); return; }

    int old_gid = core_to_group[coreid];
    if (old_gid >= 0 && old_gid < (int)sb.group_num && __atomic_sub_fetch(&group_info[old_gid].owner_count, 1, __ATOMIC_SEQ_CST) == 0) bitmap_atomic_clear(gmap, old_gid);
    core_to_group[coreid] = -1;
}

bool set_newgroup(unsigned int coreid)
{
    if (unlikely(coreid >= NCPU)) 
    {
        log_err("invalid core id");
        return false;
    }

    release_group_claim(coreid);

	static volatile unsigned int group_cursor = 0;
    unsigned int start_idx = __atomic_fetch_add(&group_cursor, 1, __ATOMIC_RELAXED);
    
	for (unsigned int i = 0; i < sb.group_num; i++)
	{
		unsigned int gid = (start_idx + i) % sb.group_num;
        if (__atomic_load_n(&group_info[gid].free_blocks_count, __ATOMIC_RELAXED) == 0) continue;     // free_blocks_count 这里只作为 hint 使用，不加锁

		if (!bitmap_atomic_test_and_set(gmap, gid))       // claim 成功
		{
            if (unlikely(__atomic_load_n(&group_info[gid].free_blocks_count, __ATOMIC_RELAXED) == 0))   // 再快速确认一次，避免 claim 到已经耗尽的组
            {
                bitmap_atomic_clear(gmap, gid);
                continue;
            }
            
            __atomic_fetch_add(&group_info[gid].owner_count, 1, __ATOMIC_SEQ_CST);
			core_to_group[coreid] = gid;
			return true;
		}
	}   

    // 当前所有有空间的 group 都被别人独占了，fallback 为“共享”模式：只要有空间的组就凑合用，不抢 group 独占权了
    for (unsigned int i = 0; i < sb.group_num; i++)
    {
        unsigned int gid = (start_idx + i) % sb.group_num;
        if (__atomic_load_n(&group_info[gid].free_blocks_count, __ATOMIC_RELAXED) > 0) 
        {
            __atomic_fetch_add(&group_info[gid].owner_count, 1, __ATOMIC_SEQ_CST);
            core_to_group[coreid] = gid;
            return true;
        }
    }

    // 硬盘满了
    log_err("[set_newgroup()] Fail to alloc a group to use: The disk is full.");
	return false;   
}

bool is_datablock(BlockID block_id)
{
    if (block_id >= sb.total_blocknum)  return false;  // 越界检查（超出当前磁盘/分区总块数）
    if (block_id < sb.group_blockstart) return false;  // 如果块号小于第一个 Group 的起始块号，说明它属于前面的全局元数据区或保留区

    BlockID offset_in_group = (block_id - sb.group_blockstart) % TOTALBLOCKS_PERGROUP;
    if (offset_in_group < BMAPNUM_PERGROUP) return false; // 落在了该 Group 的 bitmap 块上

    return true;
}

static inline uint32_t bitmap_clear_range_count_locked(unsigned long* bmap, uint32_t start, uint32_t count, uint32_t* first_unset)
{
    uint32_t cleared = 0;
    uint32_t pos = start;
    uint32_t remaining = count;

    while (remaining > 0 && BITMAP_POS_SHIFT(pos) != 0)
    {
        if (bitmap_test(bmap, pos))
        {
            bitmap_clear(bmap, pos);
            cleared++;
        }
        else if (*first_unset == DATABLOCKS_PERGROUP) *first_unset = pos;
        pos++;
        remaining--;
    }

    while (remaining >= BITS_PER_LONG)
    {
        unsigned long* word = &bmap[BITMAP_POS_IDX(pos)];
        unsigned long old = *word;

        cleared += __builtin_popcountl(old);
        if (old != ~0ul && *first_unset == DATABLOCKS_PERGROUP) *first_unset = pos + (uint32_t)__builtin_ctzl(~old);
        *word = 0;

        pos += BITS_PER_LONG;
        remaining -= BITS_PER_LONG;
    }

    while (remaining > 0)
    {
        if (bitmap_test(bmap, pos))
        {
            bitmap_clear(bmap, pos);
            cleared++;
        }
        else if (*first_unset == DATABLOCKS_PERGROUP) *first_unset = pos;
        pos++;
        remaining--;
    }

    return cleared;
}


// 批量分配连续物理块，返回实际分配数量。
// 尽可能在同一个 Block Group 中分配 count 个物理连续的数据块；如果当前组空间不足或存在碎片，它会跨越多次循环（甚至跨越多个 Block Group），拼凑出总计 count 个块，并将它们的 BlockID 记录在 out 数组中。
int alloc_blocks(BlockID* out, int count)
{
    if (unlikely(count <= 0)) return 0;

    int total_allocated = 0;
    while (total_allocated < count)
    {
        unsigned int snapshot_coreid;
        int          snapshot_gid;
        GroupDescExt* gdesc;

        // 获取当前核所持有的 group （如果当前 group 不可用，会为该核再重新分配一个 group）
        {
            kguard k;
            snapshot_coreid = k->curr_cpu;
            snapshot_gid    = core_to_group[snapshot_coreid];

            if (snapshot_gid < 0 || snapshot_gid >= (int)sb.group_num || __atomic_load_n(&group_info[snapshot_gid].free_blocks_count, __ATOMIC_RELAXED) == 0)
            {
                if (!set_newgroup(snapshot_coreid)) 
                {
                    log_err("[alloc_blocks] disk is full or no claimable group");
                    break;
                }
                snapshot_gid = core_to_group[snapshot_coreid];
            }

            gdesc = &group_info[snapshot_gid];
        }

        // 获取这个 group 中的 bitmap 块
        BlockHandle handle = bc_get_handle(gdesc->bitmap_lba);
        if (unlikely(!handle)) 
        {
            log_err("[alloc_blocks] failed to get bitmap handle for gid=%d", snapshot_gid);
            
            {
                kguard k;
                if (k->curr_cpu == snapshot_coreid && core_to_group[snapshot_coreid] == snapshot_gid) release_group_claim(snapshot_coreid);
            }

            break;
        }

        // 先获取 bitmap 块的写锁
        auto acc = handle.write_access();
        unsigned long* bmap = reinterpret_cast<unsigned long*>(acc->data);

        int got = 0;
        int start_offset = 0;

        {
            kguard k;
            if (unlikely(k->curr_cpu != snapshot_coreid || core_to_group[k->curr_cpu] != snapshot_gid)) continue;

            SpinGuardNP g(&gdesc->lock);

            if (unlikely(gdesc->free_blocks_count == 0)) continue;

            int want = MIN(count - total_allocated, gdesc->free_blocks_count);
            uint32_t hint = gdesc->next_free_hint;
            got = alloc_consecutive_bits_locked(bmap, DATABLOCKS_PERGROUP, &hint, want, &start_offset);
            if (got > 0)
            {
                gdesc->next_free_hint = hint;
                __atomic_sub_fetch(&gdesc->free_blocks_count, got, __ATOMIC_RELAXED);
                acc.mark_dirty();
            }
            else
            {
                log_warn("[alloc_blocks] gid=%d bitmap full but free_blocks_count=%u, force fix to 0", snapshot_gid, gdesc->free_blocks_count);
                gdesc->free_blocks_count = 0;
                gdesc->next_free_hint = 0;
            }
        }

        if (got > 0)
        {
            for (int i = 0; i < got; i++)
                out[total_allocated++] = gdesc->data_start_lba + (uint32_t)(start_offset + i);
        }
    }

    return total_allocated;
}

void free_extent(const iExtent* ext)
{
    if (unlikely(ext == nullptr || ext->block_count == 0)) return;

    BlockID  current_lba     = ext->physical_start;
    uint64_t remaining_count = ext->block_count;

    // 虽然 Extent 理论上代表物理连续的块，但实际上不应该跨越 Group 的元数据区，
    while (remaining_count > 0)
    {
        if (unlikely(!is_datablock(current_lba))) 
        {
            log_err("[free_extent] attempt to free non-data block %llu in extent", (unsigned long long)current_lba);
            current_lba++;
            remaining_count--;
            continue;
        }

        int gid;
        BlockID bitmap_lba, data_start_lba;
        get_group_by_blkid(current_lba, &gid, &bitmap_lba, &data_start_lba);

        BlockHandle handle = bc_get_handle(bitmap_lba);
        if (unlikely(!handle)) 
        {
            log_err("[free_extent] failed to get bitmap handle for group %u, blk=%llu", gid, (unsigned long long)current_lba);
            return;
        }

        // 计算在当前这一个 Group 中，最多能释放多少个连续的块
        uint32_t start_offset = (uint32_t)(current_lba - data_start_lba);
        uint32_t max_blocks_in_group = DATABLOCKS_PERGROUP - start_offset;
        uint32_t blocks_to_free_this_round = MIN((uint64_t)max_blocks_in_group, remaining_count);

        GroupDescExt* gdesc = &group_info[gid];
        uint32_t actually_freed = 0;
        uint32_t first_unset = DATABLOCKS_PERGROUP;
        bool free_count_saturated = false;

        {
            auto acc = handle.write_access();
            unsigned long* bmap = reinterpret_cast<unsigned long*>(acc->data);

            {
                SpinGuardNP g(&gdesc->lock);

                actually_freed = bitmap_clear_range_count_locked(bmap, start_offset, blocks_to_free_this_round, &first_unset);

                // 批量更新空闲计数器
                if (likely(gdesc->free_blocks_count + actually_freed <= DATABLOCKS_PERGROUP)) __atomic_add_fetch(&gdesc->free_blocks_count, actually_freed, __ATOMIC_RELAXED);
                else 
                {
                    free_count_saturated = true;
                    __atomic_store_n(&gdesc->free_blocks_count, DATABLOCKS_PERGROUP, __ATOMIC_RELAXED);
                }

                if (start_offset < gdesc->next_free_hint) gdesc->next_free_hint = start_offset;   // hint 尽量往前推
            }

            acc.mark_dirty();
        }

        if (unlikely(first_unset != DATABLOCKS_PERGROUP)) log_warn("[free_extent] double free detected for blk=%llu (group=%u, offset=%u)", (unsigned long long)(data_start_lba + first_unset), gid, first_unset);
        if (unlikely(free_count_saturated)) log_warn("[free_extent] group %u free_blocks_count saturated, clamping to MAX", gid);

        // 推进到下一批（如果有跨组的情况）
        current_lba     += blocks_to_free_this_round;
        remaining_count -= blocks_to_free_this_round;
    }
}

BlockID alloc_block()
{
    BlockID blk;
    if (alloc_blocks(&blk, 1) == 1) return blk;
    return INVALID_BLOCK_ID;
}
void free_block(BlockID blk)
{
    iExtent ext = {.logical_start = 0, .physical_start = blk, .block_count = 1};
    free_extent(&ext);
}

void sync_all_gdt()
{
    if (group_info == nullptr) 
    {
        log_warn("[sync_all_gdt] group_info is null");
        return;
    }

    GroupDescriptor* disk_gdt = new GroupDescriptor[sb.group_num]();

    for (uint32_t i = 0; i < sb.group_num; ++i)   // 如果系统终止前已经停止了所有并发 alloc/free，这里其实不加锁也可以。
    {
        SpinGuardNP g(&group_info[i].lock);
        disk_gdt[i].free_blocks_count = group_info[i].free_blocks_count;
        disk_gdt[i].next_free_hint    = group_info[i].next_free_hint;
        disk_gdt[i].flags             = group_info[i].flags;
        disk_gdt[i].pad1              = 0;
        disk_gdt[i].pad2[0]           = 0;
        disk_gdt[i].pad2[1]           = 0;
    }

    journal_write_metadata(disk_gdt, sb.group_num * sizeof(GroupDescriptor), sb.gdt_blockstart, 0);

    delete[] disk_gdt;

    log_info("[sync_all_gdt] synced %u group descriptors to disk", sb.group_num);
}