#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main()
{
    int fd, ret;
    char read_buf[128] = {0};

    printf("=== Test: SEEK_END and Append ===\n");

    // 1. 创建并写入初始数据
    fd = open("FSHAO:/append_test", O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        printf("Failed to open file.\n");
        exit(1);
    }

    char str1[] = "Hello, ";
    ret = write(fd, str1, strlen(str1));
    printf("[WRITE 1] Wrote %d bytes: '%s'\n", ret, str1);

    // 2. 将文件指针移动到文件末尾（偏移量为 0，基准为 SEEK_END）
    off_t end_pos = lseek(fd, 0, SEEK_END);
    printf("[LSEEK] SEEK_END returned position: %ld\n", (long)end_pos);

    // 3. 在末尾追加写入新数据
    char str2[] = "SHAOFS!";
    ret = write(fd, str2, strlen(str2));
    printf("[WRITE 2] Wrote %d bytes: '%s'\n", ret, str2);

    // 4. 读取验证：将指针移回文件开头，读取所有内容
    lseek(fd, 0, SEEK_SET); // 直接用 lseek 回到开头，省去 close 再 open
    ret = read(fd, read_buf, sizeof(read_buf) - 1);
    printf("[READ] Read %d bytes total.\n", ret);
    printf("[RESULT] File content: '%s'\n", read_buf);

    close(fd);
    printf("\nTest finished.\n");
    exit(0);
}