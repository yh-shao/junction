#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main()
{
    int fd, ret;
    char read_buf[64] = {0};

    // --- 测试 1：连续写入（测试 offset 自动累加） ---
    printf("=== Test 1: Sequential Write ===\n");
    // 假设你的系统支持类似 O_TRUNC 的标志位，或者你每次都删掉重建文件
    fd = open("FSHAO:/offset_test", O_RDWR | O_CREAT, 0644);
    if (fd < 0) 
    {
        printf("Failed to open file.\n");
        exit(1);
    }

    char str1[] = "Hello, ";
    char str2[] = "World!";
    
    ret = write(fd, str1, strlen(str1));
    printf("[WRITE 1] Expected 7, returned %d, Wrote: '%s'\n", ret, str1);
    
    // 如果 offset 机制正确，这里应该接着 "Hello, " 后面写，而不是覆盖开头
    ret = write(fd, str2, strlen(str2));
    printf("[WRITE 2] Expected 6, returned %d, Wrote: '%s'\n", ret, str2);

    // 为了测试读取，最稳妥的做法是先 close 再重新 open，这会让新 fd 的 offset 重置为 0
    close(fd);


    // --- 测试 2：连续读取（测试 offset 自动累加） ---
    printf("\n=== Test 2: Sequential Read ===\n");
    fd = open("FSHAO:/offset_test", O_RDWR, 0644);
    
    memset(read_buf, 0, sizeof(read_buf));
    ret = read(fd, read_buf, 7); // 读前 7 个字节
    printf("[READ 1] Expected 7, returned %d, Read: '%s'\n", ret, read_buf);

    memset(read_buf, 0, sizeof(read_buf));
    ret = read(fd, read_buf, 6); // 接着读 6 个字节
    printf("[READ 2] Expected 6, returned %d, Read: '%s'\n", ret, read_buf);


    // --- 测试 3：使用 lseek 测试随机访问（如果你实现了的话） ---
     printf("\n=== Test 3: Random Access with lseek ===\n");
    // 将 offset 移动到偏移量为 7 的位置（即 "World!" 的 'W' 处）
    off_t pos = lseek(fd, 7, SEEK_SET);
    printf("[LSEEK] Set position to %ld\n", (long)pos);

    memset(read_buf, 0, sizeof(read_buf));
    ret = read(fd, read_buf, 6);
    printf("[READ 3] Expected 6, returned %d, Read: '%s'\n", ret, read_buf);

    close(fd);
    printf("\nTest finished.\n");
    exit(0);
}