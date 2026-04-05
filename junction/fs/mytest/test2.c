#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main()
{
    // 打开文件
    int fd = open("FSHAO:/file", O_RDWR | O_CREAT, 0644);
    printf("[OPEN] ret val = %d\n", fd);
    if (fd < 0) {
        printf("Failed to open file.\n");
        exit(1);
    }

    size_t n = 2000;
    char *read_buf = (char*)malloc(n);
    char *write_buf = (char*)malloc(n);
    
    // 初始化写缓冲区为空字符串
    write_buf[0] = '\0'; 

    // 第一次初始读取
    int ret = read(fd, read_buf, n);
    printf("[READ INIT] ret val = %d\n", ret);

    // 循环测试，每次写入更长且内容不同的数据
    for (int i = 0; i < 5; i++) {
        printf("\n--- Iteration %d ---\n", i);

        // 1. 构建本次要写入的内容 (不断累加)
        char temp_str[64];
        snprintf(temp_str, sizeof(temp_str), "Msg %d | ", i);
        strcat(write_buf, temp_str); // 将新内容追加到写缓冲区末尾

        size_t write_len = strlen(write_buf);

        // 2. 写入文件（因为没有 offset，会从头覆盖并因为长度增加而扩展文件）
        ret = write(fd, write_buf, write_len);
        printf("[WRITE] ret val = %d, expected length = %zu\n", ret, write_len);

        // 清空读缓冲区，防止上一次读取的残留数据干扰判断
        memset(read_buf, 0, n);

        // 3. 读取文件
        ret = read(fd, read_buf, n);
        printf("[READ] ret val = %d\n", ret);
        
        // 4. 打印读取到的内容
        printf("Read Content: ");
        for (int j = 0; j < ret; j++) {
            printf("%c", read_buf[j]);
        }
        printf("\n");
    }

    // 释放资源
    free(read_buf);
    free(write_buf);
    close(fd);

    exit(0);
}