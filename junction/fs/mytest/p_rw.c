#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main() 
{
    int fd = open("FSHAO:/test_file.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);  // 打开或创建一个测试文件
    if (fd < 0) 
    {
        perror("open failed");
        return 1;
    }

    const char *write_data = "Hello, pread and pwrite!";
    off_t offset = 20; // 我们故意从文件的第 20 个字节开始写
    ssize_t w_ret = pwrite(fd, write_data, strlen(write_data), offset);
    if (w_ret < 0) 
    {
        perror("pwrite failed");
        close(fd);
        return 1;
    }
    printf(">> 成功在偏移量 %ld 处写入了 %zd 个字节。\n", offset, w_ret);

    char read_buf[50] = {0};
    ssize_t r_ret = pread(fd, read_buf, strlen(write_data), offset);
    if (r_ret < 0) 
    {
        perror("pread failed");
        close(fd);
        return 1;
    }
    printf(">> 成功从偏移量 %ld 处读取了 %zd 个字节，内容是: \"%s\"\n", offset, r_ret, read_buf);

    close(fd);
    return 0;
}