#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main()
{
    int fd, ret;
    char read_buf[128] = {0};

    printf("=== Test: O_APPEND Flag ===\n");

    // 1. 打开文件，关键点：带上 O_APPEND 标志
    fd = open("FSHAO:/append_flag_test", O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        printf("Failed to open file.\n");
        exit(1);
    }

    // 2. 第一次写入
    char str1[] = "AAAAA";
    ret = write(fd, str1, strlen(str1));
    printf("[WRITE 1] Wrote %d bytes: '%s'\n", ret, str1);

    // 3. 核心测试：故意将 offset 移到文件开头
    off_t pos = lseek(fd, 0, SEEK_SET);
    printf("[LSEEK] Forced offset back to: %ld\n", (long)pos);

    // 4. 第二次写入
    // 如果 O_APPEND 生效，这里会无视上面的 lseek，依然追加到末尾
    char str2[] = "BBBBB";
    ret = write(fd, str2, strlen(str2));
    printf("[WRITE 2] Wrote %d bytes: '%s'\n", ret, str2);

    // 5. 验证结果
    // 注意：O_APPEND 只强制限制 write，不限制 read。
    // 所以我们需要用 lseek 再次回到开头，才能把全文件读出来。
    lseek(fd, 0, SEEK_SET);
    ret = read(fd, read_buf, sizeof(read_buf) - 1);

    printf("[READ] Read %d bytes total.\n", ret);
    printf("[RESULT] File content: '%s'\n", read_buf);

    // 6. 结果诊断
    printf("\n--- DIAGNOSIS ---\n");
    if (strcmp(read_buf, "AAAAABBBBB") == 0) {
        printf("[SUCCESS] 完美！O_APPEND 成功拦截了写入，数据追加到了末尾。\n");
    } else if (strcmp(read_buf, "BBBBB") == 0) {
        printf("[FAILED] O_APPEND 没生效。第二次写入覆盖了文件开头。\n");
    } else {
        printf("[UNKNOWN] 出现了意外的结果，请检查底层 offset 或 size 逻辑。\n");
    }

    close(fd);
    printf("\nTest finished.\n");
    exit(0);
}