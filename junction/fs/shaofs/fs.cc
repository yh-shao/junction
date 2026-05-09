#include "fs.h"
#include "utili.h"
#include <cstring>
#include "group.h"
#include "journal.h"
extern "C" {
#include "runtime/runtime.h"
}

SuperBlock sb;
bitmap_ptr_t imap;
bitmap_ptr_t gmap;    // gmap[i] 表示第 i 个 group 是否被某个核占用

void print_SuperBlock(const SuperBlock& sb)
{
    log_info("SuperBlock:\n"
         "  magic_number: 0x%08x\n"
         "  block_size: %u\n"
         "  total_blocknum: %lu\n"
         "  inode_size: %u\n"
         "  inode_num: %u\n"
         "  imap_blockstart: %lu\n"
         "  imap_blocknum: %lu\n"
         "  itable_blockstart: %lu\n"
         "  itable_blocknum: %lu\n"
         "  indirect_block_start: %lu\n"
         "  indirect_block_num: %lu\n"
         "  group_num: %u\n"
         "  gdt_blockstart: %lu\n"
         "  gdt_blocknum: %lu\n"
         "  group_blockstart: %lu\n"
         "  journal_blockstart: %lu\n"
         "  journal_blocknum: %lu\n"
         "  root_inode: %u",
         sb.magic_number, sb.block_size, sb.total_blocknum, sb.inode_size, sb.inode_num, sb.imap_blockstart, sb.imap_blocknum, sb.itable_blockstart, sb.itable_blocknum, sb.indirect_block_start, sb.indirect_block_num, sb.group_num, sb.gdt_blockstart, sb.gdt_blocknum, sb.group_blockstart, sb.journal_blockstart, sb.journal_blocknum, sb.root_inode);
}

void init_meta()
{
    storage_read_obj(&sb, sizeof(SuperBlock), SUPERBLOCK_LOCATION, 0);
    print_SuperBlock(sb);

    journal_init();
    if (!journal_recover())
    {
        log_err("[init_meta] journal recovery failed");
        exit(1);
    }
    journal_mark_dirty();

    if (sb.group_num == 0) 
    {
        log_err("invalid group number!");
        exit(1);
    }
    if (sb.group_num > 2979) sb.group_num = 2979;    // group 太多时会导致 group_info 内存占用过大

    imap = new unsigned long[BITMAP_LONG_SIZE(sb.inode_num)]();
    storage_read_obj(imap, BITMAP_LONG_SIZE(sb.inode_num) * sizeof(unsigned long), sb.imap_blockstart, 0);

    gmap = new unsigned long[BITMAP_LONG_SIZE(sb.group_num)]();

    journal_build_metadata_map();

    init_group();

#if IO_PREEMPT
    barrier();
    atomic64_write(&runtime_info->spdk_uipi, 1);   // 之后让 IOKernel 检查 SPDK 完成情况
#endif
}