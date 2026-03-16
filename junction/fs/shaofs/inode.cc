#include "fs.h"
#include "disk.h"
#include "blockCache2.h"
#include "blockpool.h"

int alloc_inum()
{
	for (int idx = 0; idx < INODENUM; idx++)
		if (!bitmap_atomic_test_and_set(imap, idx)) return idx;
	
	log_info("[ERROR] alloc_inum(): No free inode!");
	return -1;
}
void free_inum(int inum)    // 释放 inode，好像很少有场景需要 free，除非是删除文件
{
    if (inum < 0 || inum >= INODENUM) return;
    bitmap_atomic_clear(imap, inum);
}


void read_dinode_from_blockcache(int idx, DInode* dinode)    // 读取盘上第 idx 个 dinode 的数据 （直接返回 block cache 中的数据）
{
	if (idx < 0 || idx >= sb.inode_num) 
	{
    	log_info("Error: inode index %d out of bounds (max = %d)\n", idx, sb.inode_num - 1);
    	exit(1);
	}

	// log_info("read_dinode_from_blockcache(%d)", idx);

	// uint64_t before_readinode_tsc = rdtsc();
	// thread_t *th = thread_self();
	// uint64_t before_readinode = thread_get_total_cycles(th) / cycles_per_us;

	BlockID blockidx = sb.itable_blockstart + idx / INODENUM_PER_BLOCK;
	int idx2 = idx % INODENUM_PER_BLOCK;

	BlockHandle handle = bc_get_handle(blockidx);
    if (!handle) 
    {
        log_err("Error: Failed to get handle for inode block %lu", blockidx);
        exit(1); // 发生严重 IO 错误或内存耗尽，保持原有的 fail-fast 语义
    }

	auto acc = handle.access();
	*dinode = reinterpret_cast<DInode*>(acc->data)[idx2];         // 考虑给 block cache 使用引用计数策略，防止在使用期间被 evict

	// log_info("read_dinode_from_blockcache(%d) done", idx);
	// uint64_t after_readinode_tsc = rdtsc();
	// uint64_t after_readinode = thread_get_total_cycles(th) / cycles_per_us;
	// log_info("[read_inode(%d)] duration: %lu us, actual time: %lu us", idx, (after_readinode_tsc - before_readinode_tsc) / cycles_per_us, after_readinode - before_readinode);
}
void write_dinode_to_blockcache(int idx, DInode* dinode)    // 将 dinode 的数据写到盘上第 idx 个 dinode 中
{
    if (idx < 0 || idx >= sb.inode_num) 
	{
    	log_info("Error: inode index %d out of bounds (max = %d)\n", idx, sb.inode_num - 1);
    	exit(1);
	}

	// uint64_t before_writeinode = rdtsc();
	u_int64_t blockidx = sb.itable_blockstart + idx / INODENUM_PER_BLOCK;
    int idx2 = idx % INODENUM_PER_BLOCK;
	
	BlockHandle handle = bc_get_handle(blockidx);
    if (!handle) 
    {
        log_err("Error: Failed to get handle for inode block %lu", blockidx);
        return;
    }

	auto acc = handle.access();
    DInode* inode_tbl = reinterpret_cast<DInode*>(acc->data);
    if (inode_tbl[idx2].idx != idx) 
    {
        log_info("Warning: overwriting mismatched inode (expected %d, got %d)", idx, inode_tbl[idx2].idx);
        return;
    }

	inode_tbl[idx2] = *dinode;
	acc.mark_dirty();
}
