#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>
#include "fs.h"

ssize_t file_read(int inum, char* buf, off_t offset, size_t len);
ssize_t file_write(int inum, const char* buf, off_t offset, size_t len);
ssize_t file_write_append(int inum, const char* buf, size_t len, off_t* new_off);
ssize_t file_read_direct(int inum, char* buf, off_t offset, size_t len);
ssize_t file_write_direct(int inum, const char* buf, off_t offset, size_t len);
ssize_t file_write_direct_append(int inum, const char* buf, size_t len, off_t* new_off);
ssize_t file_readv_direct(int inum, const struct iovec* iov, int iovcnt, off_t offset);
void truncate_inode(int inum);
void shaofs_sync_all();
void final_flush();

#define DIRECT_READ_HINT_MAX_EXTENTS 64
struct DirectReadHint {
    bool valid;
    int inum;
    uint32_t extent_count;
    uint64_t file_size;
    uint64_t inode_dirty_seq;
    iExtent extents[DIRECT_READ_HINT_MAX_EXTENTS];
};
bool file_prepare_direct_read_hint(int inum, DirectReadHint* hint);
ssize_t file_read_direct_hint(DirectReadHint* hint, char* buf, off_t offset, size_t len);
void init_file_io();
