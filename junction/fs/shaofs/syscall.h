#include "fs.h"

int my_open(const char *pathname, int flags, mode_t mode);
ssize_t my_read (int inum,       void *buf, off_t* off, size_t len, bool direct);
ssize_t my_write(int inum, const void *buf, off_t* off, size_t len, bool direct);
int my_mkdir(const char *pathname, mode_t mode);
off_t my_lseek(int inum, off_t offset, int whence, off_t old_off);