#include "disk.h"
#include "fs.h"
#include "blockCache2.h"
#include "blockpool.h"
// #include "dml_utili.h"
#include "dsa.h"

// SuperBlock sb;

// DEFINE_BITMAP(imap, INODENUM);
// int imap_size = BITMAP_LONG_SIZE(INODENUM);

// DEFINE_BITMAP(gmap, BLOCK_SIZE * 8);


void read_bm(unsigned long* bm, u_int64_t nbits, BlockID blockstart, u_int64_t blockcount)  // 将盘上的 bitmap 存储到 bm 中（空间需提前申请）
{
	size_t bm_size = CEIL(nbits, 64) * sizeof(uint64_t);
	// readObj(bm, bm_size, blockstart, blockcount);
    readObj_sync(bm, bm_size, blockstart, blockcount);
}
void write_bm(unsigned long* bm, u_int64_t nbits, BlockID blockstart, u_int64_t blockcount) // 将 bm 中的数据写入到盘上
{
	size_t bm_size = CEIL(nbits, 64) * sizeof(uint64_t);
	storage_write_obj(bm, bm_size, blockstart, blockcount);
}

// TODO：将这些初始时读取的块放一起用 readv() 来读取，以减少 IO 命令的数目  （这些块目前是全局变量，不能 SPDK DMA，后面看咋办吧）
// void read_meta()
// {
//     log_info("Reading SuperBlock ...");
// 	// readObj(&sb, sizeof(sb), 0, 1);
//     readObj_sync(&sb, sizeof(sb), 0, 1);
//     log_info("SuperBlock info:\nmagic_number: 0X%x\nblock_size: %u\ntotal_blocknum: %lu\ninode_size: %d\ninode_num: %d\nimap_blockstart: %lu\nimap_blocknum: %lu\nitable_blockstart: %lu\nitable_blocknum: %lu\nindirect_block_start: %lu\nindirect_block_num: %lu\ngroup_num: %d\ngmap_blockstart: %lu\ngmap_blocknum: %lu\nroot_inode: %d", sb.magic_number, sb.block_size, sb.total_blocknum, sb.inode_size, sb.inode_num, sb.imap_blockstart, sb.imap_blocknum, sb.itable_blockstart, sb.itable_blocknum, sb.indirect_block_start, sb.indirect_block_num, sb.group_num, sb.gmap_blockstart, sb.gmap_blocknum, sb.root_inode);
    
//     log_info("Reading inode bitmap ...");
//     read_bm(imap, INODENUM, sb.imap_blockstart, sb.imap_blocknum);
//     for (int i = 0; i < INODENUM; i++)
//         if (bitmap_test(imap, i)) log_info("inode [%d] is used", i);
    
//     log_info("Reading group bitmap ...");
//     read_bm(gmap, sb.group_num, sb.gmap_blockstart, sb.gmap_blocknum);
//     for (int i = 0; i < sb.group_num; i++)
//         if (bitmap_test(gmap, i)) log_info("group [%d] is used", i);

//     barrier();
//     atomic64_write(&runtime_info->spdk_uipi, 1);   // 之后让 IOKernel 检查 SPDK 完成情况
// }

uint64_t extent_size(const iExtent* ext) { return ext->block_count * BLOCK_SIZE; }


// 直接读取一个 extent 可以减少 IO 命令的数目，是否考虑优化下？（现在这个写法其实是逐块读取盘）
// void read_extent(const iExtent *ext, uint64_t offset, char* buf, size_t size)   // 从磁盘上读取某个 extent 中从 offset 处长度为 size 的内容
// {
//     if (ext->block_count == 0 || size == 0 || buf == NULL) return;

// 	log_info("[read_extent()] START");

// 	thread_t *th = thread_self();
// 	uint64_t before_read_extent_tsc = rdtsc();
// 	uint64_t before_read_extent = thread_get_total_cycles(th) / cycles_per_us;

// 	char* tmp = tmp_block_pool->alloc_block();
// 	size_t taken_size = 0;

// 	uint64_t start_block_idx =  offset             / BLOCK_SIZE;
// 	uint64_t end_block_idx   = (offset + size - 1) / BLOCK_SIZE;
//     for (uint64_t i = start_block_idx; i <= end_block_idx; i++)        // 逐个读取 block
// 		if (i == start_block_idx)
// 		{
// 			read_block(ext->physical_start + i, tmp);

// 			size_t block_offset = offset % BLOCK_SIZE;
// 			size_t block_size = MIN(BLOCK_SIZE - block_offset, size);   // 考虑到只涉及 1 块的情况
// 			memcpy(buf + taken_size, tmp + block_offset, block_size);
// 			taken_size += block_size;
// 		}
// 		else if (i == end_block_idx)
// 		{
// 			read_block(ext->physical_start + i, tmp);

// 			size_t tail_size = (offset + size) % BLOCK_SIZE; 
// 			size_t block_size = tail_size > 0 ? tail_size : BLOCK_SIZE;
// 			memcpy(buf + taken_size, tmp, block_size);
// 			taken_size += block_size;
// 		}
// 		else
// 		{
// 			read_block(ext->physical_start + i, buf + taken_size);  // 从 cache 中读取 block
// 			taken_size += BLOCK_SIZE;
// 		}

// 	tmp_block_pool->free_block(tmp);

// 	uint64_t after_read_extent = thread_get_total_cycles(th) / cycles_per_us;
// 	uint64_t after_read_extent_tsc = rdtsc();
// 	log_info("[read_extent()] duration: %lu us, actual time: %lu us", (after_read_extent_tsc - before_read_extent_tsc) / cycles_per_us, after_read_extent - before_read_extent);
//     // readObj(buf, size, ext->physical_start, ext->block_count);
// }

void whattoread(uint64_t current_idx, uint64_t start_block_idx, uint64_t end_block_idx, uint64_t offset, size_t size, size_t& block_offset, size_t& block_size)  // 本次读写聚焦到单个块上时，要在该块的 offset 处读/写 size 长度的数据
{
	block_offset = 0;
	block_size   = BLOCK_SIZE;

    if (current_idx == start_block_idx)    // 首块
    {   
        block_offset = offset % BLOCK_SIZE;
        block_size = MIN(BLOCK_SIZE - block_offset, size);   // 考虑到只涉及 1 块的情况
    }
    else if (current_idx == end_block_idx) // 尾块
    {
        size_t tail_size = (offset + size) % BLOCK_SIZE;
        if (tail_size > 0) block_size = tail_size;
    }
}

void read_extent(const iExtent *ext, uint64_t offset, char* buf, size_t size)
{
    if (ext == nullptr || ext->block_count == 0 || size == 0 || buf == NULL) return;

    size_t taken_size = 0;       // 已经复制到 buf 中的字节数

    uint64_t start_block_idx = offset / BLOCK_SIZE;
    uint64_t   end_block_idx = (offset + size - 1) / BLOCK_SIZE;

    for (uint64_t current_idx = start_block_idx; current_idx <= end_block_idx; current_idx++)
    {
        uint64_t current_lba = ext->physical_start + current_idx;

        BlockHandle handle = bc_get_handle(current_lba);
        if (!handle)
        {
            log_err("[read_extent] ERROR: Failed to get handle for LBA %lu. IO error or Cache Pool exhausted.", current_lba);
            return; 
        }

        size_t of = 0, sz = 0;
        whattoread(current_idx, start_block_idx, end_block_idx, offset, size, of, sz);
        if (sz > 0) 
        {
            auto acc = handle.access(); 
            memcpy(buf + taken_size, (char*)acc->data + of, sz);
            // dsa_memcpy 优化可以在这里继续使用，替代 memcpy
        }
        taken_size += sz;
    }
}

void write_extent(const iExtent *ext, uint64_t offset, const void *data, size_t size)   // 从该 extent 的 offset（B）处起，写入 size 长度数据
{
	if (ext == nullptr || extent_size(ext) < offset + size) 
    {
        log_info("[write_extent()] invalid extent");
        return;
    }

    size_t written = 0;

	uint64_t start_block_idx =  offset             / BLOCK_SIZE;     // 第一个字节所属的块
	uint64_t end_block_idx   = (offset + size - 1) / BLOCK_SIZE;     // 最后一个字节所属的块
	for (size_t i = start_block_idx; i <= end_block_idx; i++)
	{
        uint64_t lba = ext->physical_start + i;

        size_t of = 0, sz = BLOCK_SIZE;
		whattoread(i, start_block_idx, end_block_idx, offset, size, of, sz);
        bool is_partial = !(of == 0 && sz == BLOCK_SIZE);   // 如果是部分写入，则需先读再写
        if (!is_partial)  // 如果是一整块的覆盖写，根本不需要从盘上把老数据读上来，直接调用 bc_write 将新数据扔进 Cache 并自动标脏
        {
            if (data == NULL)   // 若 data 为 NULL，则置 0    （考虑写盘时用 spdk_nvme_ns_cmd_write_zeroes 来优化，如果是写内存cache，好像不行）
            {
                char zero_buf[BLOCK_SIZE] = {0};
                bc_write(lba, zero_buf);
            } 
            else 
            {
                bc_write(lba, (const char*)data + written);
            }
        }
        else
        {
            BlockHandle handle = bc_get_handle(lba);
            if (!handle) 
            {
                log_err("[write_extent] ERROR: Failed to get handle for LBA %lu", lba);
                return;
            }

            auto acc = handle.access();
            if (data == NULL)   
                memset((char*)acc->data + of, 0, sz);
            else
                memcpy((char*)acc->data + of, (const char*)data + written, sz);    
            acc.mark_dirty();
        }
        written += sz;
	}
}


void test_write_disk()
{
	char str[] = "abcdefg";
	uint64_t before_write = rdtsc();
	storage_write_obj(str, 8, 1000010, 1);
	uint64_t after_write = rdtsc();
	log_info("[storage_write_obj] duration: %lu us", (after_write - before_write) / cycles_per_us);
}
void test_read_disk()
{
	char str[10];
	uint64_t before_read = rdtsc();
	storage_read_obj(str, 8, 1000010, 1);
	uint64_t after_read = rdtsc();
	log_info("[storage_read_obj] duration: %lu us", (after_read - before_read) / cycles_per_us);
}