#include "group.h"
#include "fs.h"
#include "blockCache.h"

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
    if (old_gid >= 0 && old_gid < (int)sb.group_num) bitmap_atomic_clear(gmap, old_gid);
    core_to_group[coreid] = -1;
}

bool set_newgroup(unsigned int coreid)
{
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
            
			core_to_group[coreid] = gid;
			return true;
		}
	}   
		
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


BlockID alloc_block()
{
    while (true) 
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
                    log_err("[alloc_block] disk is full or no claimable group");
                    return INVALID_BLOCK_ID;
                }
                snapshot_gid = core_to_group[snapshot_coreid];
            }

            gdesc = &group_info[snapshot_gid];
        }


        // 获取这个 group 中的 bitmap 块
        BlockHandle handle = bc_get_handle(gdesc->bitmap_lba);
        if (unlikely(!handle)) 
        {
            log_err("[alloc_block] failed to get bitmap handle for gid=%d", snapshot_gid);

            {
                kguard k;
                if (k->curr_cpu == snapshot_coreid && core_to_group[snapshot_coreid] == snapshot_gid) release_group_claim(snapshot_coreid);
            }

            return INVALID_BLOCK_ID;
        }

        // 先获取 bitmap 块的写锁（这里可能会发生 uthread yield），再进入不可抢占区做校验和 bitmap 操作
        auto acc = handle.write_access();
        unsigned long* bmap = reinterpret_cast<unsigned long*>(acc->data);

        // 尝试在这个 group 中分配 1 块
        {
            kguard k;
            if (k->curr_cpu != snapshot_coreid || core_to_group[k->curr_cpu] != snapshot_gid) continue;   // 重试（acc 析构释放写锁）

            SpinGuard g(&gdesc->lock);
            if (unlikely(gdesc->free_blocks_count == 0)) continue;

            uint32_t hint = gdesc->next_free_hint;
            int allocated_offset = alloc_one_bit_from_bitmap_locked(bmap, DATABLOCKS_PERGROUP, &hint);
            if (likely(allocated_offset >= 0))
            {
                gdesc->next_free_hint = hint;
                gdesc->free_blocks_count--;
                acc.mark_dirty();

                BlockID allocated_blk = gdesc->data_start_lba + (uint32_t)allocated_offset;                
                // log_info("Core %u allocated block %llu in group %d", curr_coreid, (unsigned long long)allocated_blk, snapshot_gid);
                return allocated_blk;
            }

            // 理论上走到这里，说明 free_blocks_count 和 bitmap 不一致：统计说有空闲，但 bitmap 已满。将其修正为 0，然后重试。
            log_warn("[alloc_block] gid=%d bitmap full but free_blocks_count=%u, force fix to 0", snapshot_gid, gdesc->free_blocks_count);

            gdesc->free_blocks_count = 0;
            gdesc->next_free_hint = 0;
        }
    }
}

// 批量分配连续物理块，返回实际分配数量。
// 尽可能在同一个 Block Group 中分配 count 个物理连续的数据块；如果当前组空间不足或存在碎片，它会跨越多次循环（甚至跨越多个 Block Group），拼凑出总计 count 个块，并将它们的 BlockID 记录在 out 数组中。
int alloc_blocks(BlockID* out, int count)
{
    if (unlikely(count <= 0)) return 0;
    if (count == 1) 
    { 
        out[0] = alloc_block(); 
        return (out[0] != INVALID_BLOCK_ID) ? 1 : 0; 
    }

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

            SpinGuard g(&gdesc->lock);

            if (unlikely(gdesc->free_blocks_count == 0)) continue;

            int want = MIN(count - total_allocated, gdesc->free_blocks_count);
            uint32_t hint = gdesc->next_free_hint;
            got = alloc_consecutive_bits_locked(bmap, DATABLOCKS_PERGROUP, &hint, want, &start_offset);
            if (got > 0)
            {
                gdesc->next_free_hint = hint;
                gdesc->free_blocks_count -= got;
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

void free_block(BlockID blk)
{
    if (unlikely(!is_datablock(blk))) 
    {
        log_err("[free_block] attempt to free non-data block or out-of-range block %llu", (unsigned long long)blk);
        return;
    }

    int gid;
    BlockID bitmap_lba, data_start_lba;
    get_group_by_blkid(blk, &gid, &bitmap_lba, &data_start_lba);

    BlockHandle handle = bc_get_handle(bitmap_lba);
    if (unlikely(!handle)) 
    {
        log_err("[free_block] failed to get bitmap handle for group %u, blk=%llu", gid, (unsigned long long)blk);
        return;
    }

    auto acc = handle.write_access();
    unsigned long* bmap = reinterpret_cast<unsigned long*>(acc->data);

    uint32_t offset = (uint32_t)(blk - data_start_lba);
    if (unlikely(offset >= DATABLOCKS_PERGROUP)) 
    {
        log_err("[free_block] invalid offset=%u for blk=%llu in group=%u", offset, (unsigned long long)blk, gid);
        return;
    }

    GroupDescExt* gdesc = &group_info[gid];
    
    SpinGuard g(&gdesc->lock);

    if (unlikely(!bitmap_test(bmap, offset))) 
    {
        log_warn("[free_block] double free detected for blk=%llu (group=%u, offset=%u)", (unsigned long long)blk, gid, offset);
        return;
    }

    bitmap_clear(bmap, offset);  // 清掉 bitmap 中对应 bit

    // 更新空闲块计数；做一个上界保护，防止元数据继续漂坏
    if (likely(gdesc->free_blocks_count < DATABLOCKS_PERGROUP)) gdesc->free_blocks_count++;
    else log_warn("[free_block] group %u free_blocks_count already saturated (%u), bitmap cleared for blk=%llu", gid, gdesc->free_blocks_count, (unsigned long long)blk);

    if (offset < gdesc->next_free_hint) gdesc->next_free_hint = offset; // hint 尽量往前推进：若释放的位置比当前 hint 更靠前，则将 hint 移到这里，有利于后续尽快复用刚释放的块，减轻碎片。

    acc.mark_dirty();
    // log_info("Freed block %llu in group %u", (unsigned long long)blk, gid);
}

void sync_gdt(uint32_t gid)
{
    if (gid >= sb.group_num) 
    {
        log_err("[sync_one_group_desc_to_disk] invalid gid=%u", gid);
        return;
    }

    const uint32_t descs_per_block = BLOCK_SIZE / sizeof(GroupDescriptor);
    const uint32_t block_idx = gid / descs_per_block;
    const uint32_t idx_in_block = gid % descs_per_block;
    const BlockID lba = sb.gdt_blockstart + block_idx;

    GroupDescriptor block_buf[BLOCK_SIZE / sizeof(GroupDescriptor)];
    storage_read_obj(block_buf, BLOCK_SIZE, lba, 0);

    {
        SpinGuard g(&group_info[gid].lock);
        block_buf[idx_in_block].free_blocks_count = group_info[gid].free_blocks_count;
        block_buf[idx_in_block].next_free_hint    = group_info[gid].next_free_hint;
        block_buf[idx_in_block].flags             = group_info[gid].flags;
        block_buf[idx_in_block].pad1              = 0;
        block_buf[idx_in_block].pad2[0]           = 0;
        block_buf[idx_in_block].pad2[1]           = 0;
    }

    storage_write_obj(block_buf, BLOCK_SIZE, lba, 0);
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
        SpinGuard g(&group_info[i].lock);
        disk_gdt[i].free_blocks_count = group_info[i].free_blocks_count;
        disk_gdt[i].next_free_hint    = group_info[i].next_free_hint;
        disk_gdt[i].flags             = group_info[i].flags;
        disk_gdt[i].pad1              = 0;
        disk_gdt[i].pad2[0]           = 0;
        disk_gdt[i].pad2[1]           = 0;
    }

    storage_write_obj(disk_gdt, sb.group_num * sizeof(GroupDescriptor), sb.gdt_blockstart, 0);

    delete[] disk_gdt;

    log_info("[sync_all_gdt] synced %u group descriptors to disk", sb.group_num);
}