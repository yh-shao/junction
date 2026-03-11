#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>

#define PAGE_SIZE 4096
#define THREAD_COUNT 4      // 模拟的并发 worker 数量
#define TEST_TIME_SEC 5     // 测试持续时间（秒）
#define TEST_ROOT_DIR "FSHAO:/test_private_root"

volatile int stop_flag = 0;
int use_direct_io = 0;

// 线程传入的参数结构，包含 worker_id
struct worker_args {
    int id;
    long long works; 
    int fd;           // 相当于 worker->private[0]
};

// 相当于 pre_work
void prepare_worker_environment(struct worker_args *worker) {
    char dir_path[256];
    char file_path[256];
    char *page = NULL;
    
    // 1. 生成私有目录并创建
    snprintf(dir_path, sizeof(dir_path), "%s/%d", TEST_ROOT_DIR, worker->id);
    mkdir(dir_path, 0755); // 忽略已存在的错误，实际使用应严格处理

    // 2. 拼接私有文件路径
    snprintf(file_path, sizeof(file_path), "%s/n_file_rd.dat", dir_path);

    // 3. 分配对齐的内存
    if (posix_memalign((void **)&page, PAGE_SIZE, PAGE_SIZE) != 0) {
        perror("posix_memalign failed");
        exit(1);
    }
    memset(page, 'B', PAGE_SIZE);

    // 4. 打开/创建文件
    int flags = O_CREAT | O_RDWR | O_TRUNC;
    if (use_direct_io) flags |= O_DIRECT;
    
    int fd = open(file_path, flags, 0644);
    if (fd < 0) {
        perror("Failed to create private test file");
        exit(1);
    }

    // 5. 写入一页数据初始化
    write(fd, page, PAGE_SIZE);
    free(page);

    // 6. 核心：不关闭文件！而是将 fd 保存到 worker 的上下文中
    worker->fd = fd; 
    printf("[Info] Worker %d prepared its private file.\n", worker->id);
}

// 相当于 main_work
void* worker_thread(void* arg) {
    struct worker_args *my_args = (struct worker_args *)arg;
    char *page = NULL;
    long long iter = 0;

    posix_memalign((void **)&page, PAGE_SIZE, PAGE_SIZE);

    // 1. 从“私有抽屉”里取出早已准备好的 fd
    int fd = my_args->fd;

    // 2. 核心循环：毫无竞争地读取私有文件
    while (!stop_flag) {
        if (pread(fd, page, PAGE_SIZE, 0) != PAGE_SIZE) {
            perror("pread error");
            break;
        }
        iter++;
    }

    // 3. 压测结束，收尾清理
    close(fd);
    free(page);
    my_args->works = iter;
    return NULL;
}

int main(int argc, char *argv[]) 
{
    if (argc > 1 && strcmp(argv[1], "direct") == 0) 
    {
        use_direct_io = 1;
        printf("[Info] Direct I/O Mode Enabled.\n");
    }

    mkdir(TEST_ROOT_DIR, 0755);   // 创建根测试目录

    pthread_t threads[THREAD_COUNT];
    struct worker_args args[THREAD_COUNT];

    // 阶段1：预热阶段，所有 worker 创建自己的环境和文件
    for (int i = 0; i < THREAD_COUNT; i++) 
    {
        args[i].id = i;
        args[i].works = 0;
        prepare_worker_environment(&args[i]);
    }

    // 阶段2：并发压测阶段
    printf("[Info] Starting %d isolated read threads...\n", THREAD_COUNT);
    for (int i = 0; i < THREAD_COUNT; i++) 
    {
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    printf("[Info] Benchmarking for %d seconds...\n", TEST_TIME_SEC);
    sleep(TEST_TIME_SEC);

    stop_flag = 1;

    // 阶段3：统计分数
    long long total_works = 0;
    for (int i = 0; i < THREAD_COUNT; i++) 
    {
        pthread_join(threads[i], NULL);
        total_works += args[i].works;
    }

    printf("=================================\n");
    printf("Test: Isolated Private File Reads\n");
    printf("Total Reads: %lld\n", total_works);
    printf("IOPS (Reads/sec): %lld\n", total_works / TEST_TIME_SEC);
    printf("=================================\n");

    return 0;
}