int my_open(const char *pathname, int flags, mode_t mode);
ssize_t my_read(int inum, void *buf, size_t len);
ssize_t my_write(int inum, const void *buf, size_t len);
long my_mkdir(const char *pathname, mode_t mode);