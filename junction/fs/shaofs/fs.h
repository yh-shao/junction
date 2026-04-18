#pragma once
#include "utili.h"

#define SHAOFS                  517
#define MYPREFIX                "FSHAO:"
#define MYPREFIX_LEN            (sizeof(MYPREFIX) - 1)   // 不包括末尾的 \0，值为 6

#define BLOCK_SIZE              4096
typedef uint64_t BlockID;
#define INVALID_BLOCK_ID ((BlockID)-1)

#define INODENUM                32768               // 所能创建的文件数目
#define INODE_SIZE              sizeof(DInode)      // 每个 inode 占用的字节数（目前设置为 256B）
#define INODENUM_PER_BLOCK      (BLOCK_SIZE / INODE_SIZE)  // 每个块中可以包含的 inode 数目（目前是 16）

#define MAX_PATH_LEN            4096
#define NAMESIZ                 255                  // 单个文件名 token 的长度 （和 ext4 一致）

#define SUPERBLOCK_LOCATION     0
#define SUPERBLOCK_NUM          1

#define ROOT_INO                0                   // 根结点对应的 inode

#define DIRECT_EXTENT_NUM       6                   // 每个 inode 中的 direct extent 数目
#define DEFAULT_EXTENT_LENGTH   10                  // 为目录文件预分配的块数

#define BMAPNUM_PERGROUP        1                   // 每个 group 中 bitmap 占多少个块
#define DATABLOCKS_PERGROUP     (BMAPNUM_PERGROUP * BLOCK_SIZE * 8)  // 4096*8=32768
#define TOTALBLOCKS_PERGROUP    (BMAPNUM_PERGROUP + DATABLOCKS_PERGROUP) 

typedef enum {
    UNKNOWN = 0,
    REGULAR,    // 普通文件
    DIRECTORY,  // 目录
    SYMLINK,    // 符号链接
} file_type_t;

typedef struct {
    uint32_t       magic_number;                 // FSMAGIC=0x0517
    uint32_t       block_size;                   // 每块（LBA）的大小（B）
    uint64_t       total_blocknum;               // 总块数
    uint32_t       inode_size;                   // inode 大小
    uint32_t       inode_num;                    // inode 数目

    BlockID        imap_blockstart;              // inode bitmap 起始块
    uint64_t       imap_blocknum;                // inode bitmap 占用的块数

    BlockID        itable_blockstart;            // inode table 起始块
    uint64_t       itable_blocknum;              // inode table 占用的块数

    BlockID        indirect_block_start;         // 第一个 indirect extent block
    uint64_t       indirect_block_num;           // 等于 inode_num

    uint32_t       group_num;                    // group 数目
    BlockID        gdt_blockstart;               // group descriptor table 起始块
    uint64_t       gdt_blocknum;                 // group descriptor table 占用的块数

    BlockID        group_blockstart;             // 第一个 group 的起始块（它的 bitmap）

    uint32_t       root_inode;                   // root 目录对应的 inode 号
} SuperBlock;

typedef struct {
    uint32_t free_blocks_count; // 该组中当前空闲的数据块总数
    uint32_t next_free_hint;    // 下一个可用空闲块的相对索引（加速查找）
    uint32_t flags;             // 状态标志位
    uint32_t pad1;              
    uint64_t pad2[2];          
} GroupDescriptor;

typedef struct {
    BlockID physical_start;
    uint64_t block_count;
} Extent;     // disk extent
typedef struct {
	BlockID logical_start, physical_start;
    uint64_t block_count;
} iExtent;    // in-inode extent
static inline uint64_t extent_size(const iExtent* ext) { return ext->block_count * BLOCK_SIZE; }
#define EXTENTS_PER_BLOCK (BLOCK_SIZE / sizeof(iExtent))

typedef struct {
	int         idx;
	uint8_t     used;
	uint8_t     major;                              // Major device number
	uint8_t     minor;                              // Minor device number
	file_type_t type;                               // 文件类型
	uint32_t    nlink;                              // 硬链接数目
	uint64_t    file_size;                          // 文件大小
	iExtent     direct_extents[DIRECT_EXTENT_NUM];  // 直接使用的 extent
	uint64_t    indirect_extent_block;              // 存储间接 extents 的 block 号
	uint64_t    ctime;                              // 创建时间
	uint64_t    mtime;                              // 修改时间
	uint64_t    atime;                              // 访问时间
	char        pad[56];
} DInode;     // disk inode (256B)

extern SuperBlock sb;
extern bitmap_ptr_t imap;
extern bitmap_ptr_t gmap;

void init_meta();
static inline bool USE_SHAOFS(const char *pathname) { return (strncmp(pathname, MYPREFIX, MYPREFIX_LEN) == 0); }