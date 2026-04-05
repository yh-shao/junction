#include "fs.h"

int alloc_inum()
{
	// for (int idx = 0; idx < INODENUM; idx++)
	// 	if (!bitmap_atomic_test_and_set(imap, idx)) return idx;

	static volatile int cursor = 0;
  	int start = atomic_read(&cursor);
  	for (int i = 0; i < INODENUM; i++)
  	{
    	int idx = (start + i) % INODENUM;
    	if (!bitmap_atomic_test_and_set(imap, idx))
    	{
      		atomic_write(&cursor, (idx + 1) % INODENUM);
      		return idx;
    	}
  	}
	
	log_info("[ERROR] alloc_inum(): No free inode!");
	return -1;
}
void free_inum(int inum)    // 释放 inode，好像很少有场景需要 free，除非是删除文件
{
    if (inum < 0 || inum >= INODENUM) return;
    bitmap_atomic_clear(imap, inum);
}