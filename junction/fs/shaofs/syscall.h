#include "fs.h"
#include <sys/stat.h>

int my_open(const char *pathname, int flags, mode_t mode);
void shaofs_reset_inode_lifecycle(int inum);
void shaofs_pin_open_inode(int inum);
void my_close(int inum);
ssize_t my_read (int inum,       void *buf, off_t* off, size_t len, bool direct);
ssize_t my_write(int inum, const void *buf, off_t* off, size_t len, bool direct, bool append);
int my_mkdir(const char *pathname, mode_t mode);
int my_unlink(const char *pathname);
off_t my_lseek(int inum, off_t offset, int whence, off_t old_off);
int my_fstat(int inum, struct stat *statbuf);
int my_newfstatat(const char *pathname, struct stat *statbuf);
int my_fsync(int inum);