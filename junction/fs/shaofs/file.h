#pragma once

ssize_t file_read(int inum, char* buf, off_t offset, size_t len);
ssize_t file_write(int inum, const char* buf, off_t offset, size_t len);
ssize_t file_read_direct(int inum, char* buf, off_t offset, size_t len);
ssize_t file_write_direct(int inum, const char* buf, off_t offset, size_t len);
void truncate_inode(int inum);
void final_flush();