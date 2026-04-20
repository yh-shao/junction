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