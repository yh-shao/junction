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


static inline void release_group_claim(unsigned int coreid)
{
    int old_gid = core_to_group[coreid];
    if (old_gid >= 0 && old_gid < (int)sb.group_num) bitmap_atomic_clear(gmap, old_gid);
    core_to_group[coreid] = -1;
}

void init_group()  
{
	memset(core_to_group, -1, sizeof(core_to_group));

    GroupDescriptor* disk_gdt = new GroupDescriptor[sb.group_num]();
    storage_read_obj(disk_gdt, sb.group_num * sizeof(GroupDescriptor), sb.gdt_blockstart, sb.gdt_blocknum);

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

static int alloc_one_bit_from_bitmap_locked(bitmap_t bmap, uint32_t nr_bits, uint32_t* hint_io)  // 从 hint 开始环形寻找空闲 bit，并置 1
{
    uint32_t hint = *hint_io;
    if (hint >= nr_bits) hint = 0;

    for (uint32_t i = 0; i < nr_bits; ++i) 
    {
        uint32_t off = hint + i;
        if (off >= nr_bits) off -= nr_bits;

        if (!bitmap_test(bmap, off)) 
        {
            bitmap_set(bmap, off);
            *hint_io = (off + 1) % nr_bits;
            return (int)off;
        }
    }

    *hint_io = 0;
    return -1;
}

bool set_newgroup(unsigned int coreid)
{
    release_group_claim(coreid);

	static int group_cursor = 0;
	int start_idx = __sync_fetch_and_add(&group_cursor, 1);
    if (sb.group_num != 0) start_idx %= sb.group_num;
    
	for (int i = 0; i < sb.group_num; i++)
	{
		int gid = (start_idx + i) % sb.group_num;
        if (__atomic_load_n(&group_info[gid].free_blocks_count, __ATOMIC_RELAXED) == 0) continue;     // free_blocks_count 这里只作为 hint 使用，不加锁

		if (!bitmap_atomic_test_and_set(gmap, gid))       // claim 成功
		{
            if (__atomic_load_n(&group_info[gid].free_blocks_count, __ATOMIC_RELAXED) == 0)   // 再快速确认一次，避免 claim 到已经耗尽的组
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
        BlockID      bitmap_lba;
        BlockID      data_startlba; 

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

            bitmap_lba    = group_info[snapshot_gid].bitmap_lba;
            data_startlba = group_info[snapshot_gid].data_start_lba;
        }


        BlockHandle handle = bc_get_handle(bitmap_lba);
        if (unlikely(!handle)) 
        {
            log_err("[alloc_block] failed to get bitmap handle for gid=%d", snapshot_gid);

            {
                kguard k;
                if (k->curr_cpu == snapshot_coreid && core_to_group[snapshot_coreid] == snapshot_gid) release_group_claim(snapshot_coreid);
            }

            return INVALID_BLOCK_ID;
        }

        // 重新进入不可抢占区，确认：①线程仍在原来的 core 上；②这个 core 当前仍绑定同一个 gid。 若不成立，说明前面的快照已经失效，必须重试。
        {
            kguard k;
            unsigned int curr_coreid = k->curr_cpu;
            int curr_gid = core_to_group[curr_coreid];

            if (curr_coreid != snapshot_coreid || curr_gid != snapshot_gid) continue;   // 重试

            // 到这里，说明：当前线程还在原来那个 core 上 且 当前 core 仍然使用这个 gid
            auto acc = handle.write_access();
            unsigned long* bmap = reinterpret_cast<unsigned long*>(acc->data);

            spin_lock(&group_info[snapshot_gid].lock);
            if (group_info[snapshot_gid].free_blocks_count == 0)   // 在锁内再确认一次 group 的状态，避免并发 free/alloc 导致快照过期
            {
                spin_unlock(&group_info[snapshot_gid].lock);
                continue;
            }

            uint32_t hint = group_info[snapshot_gid].next_free_hint;
            int allocated_offset = alloc_one_bit_from_bitmap_locked(bmap, DATABLOCKS_PERGROUP, &hint);
            if (allocated_offset >= 0) 
            {
                group_info[snapshot_gid].next_free_hint = hint;
                group_info[snapshot_gid].free_blocks_count--;

                BlockID allocated_blk = data_startlba + (uint32_t)allocated_offset;

                acc.mark_dirty();
                spin_unlock(&group_info[snapshot_gid].lock);

                log_info("Core %u allocated block %llu in group %d", curr_coreid, (unsigned long long)allocated_blk, snapshot_gid);
                return allocated_blk;
            }

            // 理论上走到这里，说明 free_blocks_count 和 bitmap 不一致：统计说有空闲，但 bitmap 已满。将其修正为 0，然后重试。
            log_warn("[alloc_block] gid=%d bitmap full but free_blocks_count=%u, force fix to 0", snapshot_gid, group_info[snapshot_gid].free_blocks_count);

            group_info[snapshot_gid].free_blocks_count = 0;
            group_info[snapshot_gid].next_free_hint = 0;

            spin_unlock(&group_info[snapshot_gid].lock);
        }
    }
}

void free_block(BlockID blk)
{
    if (!is_datablock(blk)) 
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
    if (offset >= DATABLOCKS_PERGROUP) 
    {
        log_err("[free_block] invalid offset=%u for blk=%llu in group=%u", offset, (unsigned long long)blk, gid);
        return;
    }

    spin_lock(&group_info[gid].lock);

    if (!bitmap_test(bmap, offset)) 
    {
        spin_unlock(&group_info[gid].lock);
        log_warn("[free_block] double free detected for blk=%llu (group=%u, offset=%u)", (unsigned long long)blk, gid, offset);
        return;
    }

    bitmap_clear(bmap, offset);  // 清掉 bitmap 中对应 bit

    // 更新空闲块计数；做一个上界保护，防止元数据继续漂坏
    if (group_info[gid].free_blocks_count < DATABLOCKS_PERGROUP) 
    {
        group_info[gid].free_blocks_count++;
    } 
    else 
    {
        log_warn("[free_block] group %u free_blocks_count already saturated (%u), bitmap cleared for blk=%llu", gid, group_info[gid].free_blocks_count, (unsigned long long)blk);
    }

    if (offset < group_info[gid].next_free_hint) group_info[gid].next_free_hint = offset; // hint 尽量往前推进：若释放的位置比当前 hint 更靠前，则将 hint 移到这里，有利于后续尽快复用刚释放的块，减轻碎片。

    acc.mark_dirty();
    spin_unlock(&group_info[gid].lock);

    log_info("Freed block %llu in group %u", (unsigned long long)blk, gid);
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
    storage_read_obj(block_buf, BLOCK_SIZE, lba, 1);

    spin_lock(&group_info[gid].lock);
    block_buf[idx_in_block].free_blocks_count = group_info[gid].free_blocks_count;
    block_buf[idx_in_block].next_free_hint    = group_info[gid].next_free_hint;
    block_buf[idx_in_block].flags             = group_info[gid].flags;
    block_buf[idx_in_block].pad1              = 0;
    block_buf[idx_in_block].pad2[0]           = 0;
    block_buf[idx_in_block].pad2[1]           = 0;
    spin_unlock(&group_info[gid].lock);

    storage_write_obj(block_buf, BLOCK_SIZE, lba, 1);
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
        spin_lock(&group_info[i].lock);

        disk_gdt[i].free_blocks_count = group_info[i].free_blocks_count;
        disk_gdt[i].next_free_hint    = group_info[i].next_free_hint;
        disk_gdt[i].flags             = group_info[i].flags;
        disk_gdt[i].pad1              = 0;
        disk_gdt[i].pad2[0]           = 0;
        disk_gdt[i].pad2[1]           = 0;

        spin_unlock(&group_info[i].lock);
    }

    storage_write_obj(disk_gdt, sb.group_num * sizeof(GroupDescriptor), sb.gdt_blockstart, sb.gdt_blocknum);

    delete[] disk_gdt;

    log_info("[sync_all_gdt] synced %u group descriptors to disk", sb.group_num);
}