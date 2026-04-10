#include "fs.h"
#include "utili.h"
#include <cstring>
#include "group.h"

SuperBlock sb;
bitmap_ptr_t imap;
bitmap_ptr_t gmap;    // gmap[i] 表示第 i 个 group 是否被某个核占用

void init_meta()
{
    storage_read_obj(&sb, sizeof(SuperBlock), SUPERBLOCK_LOCATION, 0);

    imap = new unsigned long[BITMAP_LONG_SIZE(sb.inode_num)]();
    storage_read_obj(imap, BITMAP_LONG_SIZE(sb.inode_num) * sizeof(unsigned long), sb.imap_blockstart, 0);

    gmap = new unsigned long[BITMAP_LONG_SIZE(sb.group_num)]();

    init_group();

    // barrier();
    // atomic64_write(&runtime_info->spdk_uipi, 1);   // 之后让 IOKernel 检查 SPDK 完成情况
}