#pragma once

ssize_t file_read(int inum, char* buf, off_t offset, size_t len);
ssize_t file_write(int inum, const char* buf, off_t offset, size_t len);
void final_flush();