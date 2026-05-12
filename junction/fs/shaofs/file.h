#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "fs.h"

ssize_t file_read(int inum, char* buf, off_t offset, size_t len);
ssize_t file_write(int inum, const char* buf, off_t offset, size_t len);
ssize_t file_read_direct(int inum, char* buf, off_t offset, size_t len);
ssize_t file_write_direct(int inum, const char* buf, off_t offset, size_t len);
void truncate_inode(int inum);
void final_flush();

#define DIRECT_READ_HINT_MAX_EXTENTS 64
struct DirectReadHint {
    bool valid;
    uint32_t extent_count;
    uint64_t file_size;
    iExtent extents[DIRECT_READ_HINT_MAX_EXTENTS];
    volatile int* has_dirty_data_cache;
};
bool file_prepare_direct_read_hint(int inum, DirectReadHint* hint);
ssize_t file_read_direct_hint(const DirectReadHint* hint, char* buf, off_t offset, size_t len);