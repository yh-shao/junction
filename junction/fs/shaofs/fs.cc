#include "fs.h"
#include "utili.h"
#include <cstring>

extern "C" {
    #include "runtime/storage.h"
}

SuperBlock sb;
bitmap_ptr_t imap;
bitmap_ptr_t gmap;    // gmap[i] 表示第 i 个 group 是否被某个核占用 或 其中的块是否已分配完毕（总之如果是 1，就不用它）

static void read_obj(void* obj, size_t siz, uint64_t lba, uint32_t lba_count)   // spdk_buffer -> buf -> obj  (2 memcpy)
{
    char* buf = new char[BLOCK_SIZE * lba_count];
    storage_read(buf, lba, lba_count);
    memcpy(obj, buf, siz);
    delete[] buf;
}


void init_meta()
{
    read_obj(&sb, sizeof(SuperBlock), SUPERBLOCK_LOCATION, SUPERBLOCK_NUM);
    log_info("SuperBlock info:\nmagic_number: 0X%x\nblock_size: %u\ntotal_blocknum: %lu\ninode_size: %d\ninode_num: %d\nimap_blockstart: %lu\nimap_blocknum: %lu\nitable_blockstart: %lu\nitable_blocknum: %lu\nindirect_block_start: %lu\nindirect_block_num: %lu\ngroup_num: %d\ngmap_blockstart: %lu\ngmap_blocknum: %lu\nroot_inode: %d", sb.magic_number, sb.block_size, sb.total_blocknum, sb.inode_size, sb.inode_num, sb.imap_blockstart, sb.imap_blocknum, sb.itable_blockstart, sb.itable_blocknum, sb.indirect_block_start, sb.indirect_block_num, sb.group_num, sb.gmap_blockstart, sb.gmap_blocknum, sb.root_inode);

    imap = new unsigned long[BITMAP_LONG_SIZE(sb.inode_num)];
    read_obj(imap, BITMAP_LONG_SIZE(sb.inode_num) * sizeof(unsigned long), sb.imap_blockstart, sb.imap_blocknum);
    for (int i = 0; i < sb.inode_num; i++)
        if (bitmap_test(imap, i)) log_info("inode [%d] is used", i);

    gmap = new unsigned long[BITMAP_LONG_SIZE(sb.group_num)];
    read_obj(gmap, BITMAP_LONG_SIZE(sb.group_num) * sizeof(unsigned long), sb.gmap_blockstart, sb.gmap_blocknum);
    for (int i = 0; i < sb.group_num; i++)
        if (bitmap_test(gmap, i)) log_info("group [%d] is used", i);

    // barrier();
    // atomic64_write(&runtime_info->spdk_uipi, 1);   // 之后让 IOKernel 检查 SPDK 完成情况
}