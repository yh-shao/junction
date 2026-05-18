#include "fs.h"
#include "inode.h"
#include "dir.h"

MInode::MInode() : dir_index(nullptr) { init_runtime_state(); }
MInode::~MInode() { drop_dir_index(); }

void MInode::drop_dir_index()
{
    if (dir_index)
    {
        dir_index_destroy(dir_index);
        dir_index = nullptr;
    }
}

void MInode::init_runtime_state()
{
    drop_dir_index();
    rwmutex_init(&dir_mtx);
    memset(&extent_hint, 0, sizeof(extent_hint));
    spin_lock_init(&hint_lock);
    spin_lock_init(&dirty_lock);
    atomic_write(&has_dirty_data_cache, 0);
    dirty_data_start = 0;
    dirty_data_end = 0;
    dirty_data_seq = 0;
}

MInode& MInode::operator=(const DInode& disk_inode)
{
    DInode::operator=(disk_inode);
    init_runtime_state();
    return *this;
}

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