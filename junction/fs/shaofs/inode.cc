#include "fs.h"
#include "inode.h"

int alloc_inum()
{
	static volatile unsigned int cursor = 0;
	unsigned int start = __atomic_fetch_add(&cursor, 1, __ATOMIC_RELAXED);
  	for (int i = 0; i < INODENUM; i++)
  	{
    	unsigned int idx = (start + i) % INODENUM;
    	if (!bitmap_atomic_test_and_set(imap, idx)) return idx;
  	}
	
	log_info("[ERROR] alloc_inum(): No free inode!");
	return -1;
}
void free_inum(int inum)    // 释放 inode，好像很少有场景需要 free，除非是删除文件
{
    if (inum < 0 || inum >= INODENUM) return;
    bitmap_atomic_clear(imap, inum);
}

bool uses_indirect_block(const DInode* ino) 
{
    if (ino->direct_extents[DIRECT_EXTENT_NUM - 1].block_count == 0) return false;  // 如果第 6 个槽位都是空的，连直接 Extent 都没用完，绝对不可能用间接块
    
	const iExtent& last_direct = ino->direct_extents[DIRECT_EXTENT_NUM - 1];
    uint64_t max_direct_lblk = last_direct.logical_start + last_direct.block_count;   // 获取第 6 个 Extent 的覆盖范围
    uint64_t required_lblks = (ino->file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;  // 计算文件真实需要的逻辑块数量 (file_size 是精确维护的，即使有文件空洞，file_size 也会撑大)
    return required_lblks > max_direct_lblk;   // 如果文件需要的逻辑块 超出了 直接 Extent 所能覆盖的最大逻辑块，即说明数据必然溢出到了间接块中。
}