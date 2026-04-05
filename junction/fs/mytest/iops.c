#define _GNU_SOURCE // 启用 O_DIRECT 等 GNU 扩展
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define FILE_SIZE_LIMIT (1024 * 1024 * 1024)   // 宏定义：测试文件的大小空间 (1GB)，用于限制随机写入的范围
#define BLOCKSIZE       4096

typedef struct {    // 线程参数
    int thread_id;
    int fd;
    size_t block_size;
    int ops_per_thread;
} thread_args_t;

// 线程工作函数：执行具体的 pwrite 写入操作
void* worker_thread(void* arg) 
{
    thread_args_t* args = (thread_args_t*)arg;
    void* buffer;
    
    // O_DIRECT 要求内存地址、写入大小和文件偏移量都必须对齐（通常是 512 或 4096 字节），这里使用 posix_memalign 分配对齐的内存
    if (posix_memalign(&buffer, 4096, args->block_size) != 0) 
    {
        fprintf(stderr, "线程 %d: 内存对齐分配失败\n", args->thread_id);
        pthread_exit(NULL);
    }
    memset(buffer, 'A' + (args->thread_id % 26), args->block_size);  // 填充测试数据
    
    // 为每个线程初始化独立的随机数种子，避免锁竞争
    unsigned int seed = time(NULL) + args->thread_id; 

    for (int i = 0; i < args->ops_per_thread; i++) 
    {
        off_t random_offset = (rand_r(&seed) % (FILE_SIZE_LIMIT / args->block_size)) * args->block_size;   // 生成随机的对齐偏移量 (模拟随机 IO)
    
        ssize_t bytes_written = pwrite(args->fd, buffer, args->block_size, random_offset);    
        if (bytes_written != args->block_size) 
        {
            fprintf(stderr, "线程 %d: 写入失败 (偏移量: %ld), 错误: %s\n", args->thread_id, random_offset, strerror(errno));
            break; // 遇到错误及时退出循环
        }
    }

    free(buffer); 
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) 
{
    if (argc != 4)   // 命令行参数
    {
        printf("用法: %s <文件路径> <线程数量> <每线程操作次数>\n", argv[0]);
        printf("示例: %s ./test.dat 4 10000\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* file_path = argv[1];
    int       num_threads = atoi(argv[2]);
    int    ops_per_thread = atoi(argv[3]);
    size_t     block_size = BLOCKSIZE;
    if (num_threads <= 0 || block_size <= 0 || ops_per_thread <= 0) 
    {
        fprintf(stderr, "错误: 参数无效。请确保线程数和操作数 > 0\n");
        return EXIT_FAILURE;
    }

    // 2. 打开测试文件
    // 使用 O_DIRECT 绕过 Page Cache
    // O_CREAT 如果文件不存在则创建，O_RDWR 可读可写
    int fd = open(file_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) 
    {
        perror("致命错误: 无法打开或创建测试文件");
        return EXIT_FAILURE;
    }

    printf("--- 开始 IOPS 测试 ---\n");
    printf("目标文件: %s\n", file_path);
    printf("并发线程: %d\n", num_threads);
    printf("I/O 块大小: %zu 字节\n", block_size);
    printf("总操作次数: %d\n\n", num_threads * ops_per_thread);

    pthread_t threads[num_threads];
    thread_args_t t_args[num_threads];
    struct timespec start_time, end_time;

    clock_gettime(CLOCK_MONOTONIC, &start_time);   // 记录开始时间 (使用高精度单调时钟)
    for (int i = 0; i < num_threads; i++)  
    {
        t_args[i].thread_id = i;
        t_args[i].fd = fd;
        t_args[i].block_size = block_size;
        t_args[i].ops_per_thread = ops_per_thread;
        
        if (pthread_create(&threads[i], NULL, worker_thread, &t_args[i]) != 0) 
        {
            perror("致命错误: 线程创建失败");
            close(fd);
            return EXIT_FAILURE;
        }
    }
    for (int i = 0; i < num_threads; i++) pthread_join(threads[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &end_time);     // 记录结束时间
    
    double elapsed_seconds = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    int total_ops = num_threads * ops_per_thread;
    double iops = total_ops / elapsed_seconds;
    double throughput_mb = (total_ops * block_size) / (1024.0 * 1024.0) / elapsed_seconds;

    printf("--- 测试结果 ---\n");
    printf("总耗时: %.4f 秒\n", elapsed_seconds);
    printf("整体 IOPS: %.2f 次/秒\n", iops);
    printf("吞吐量: %.2f MB/s\n", throughput_mb);

    // 8. 资源清理
    close(fd);
    // 可选：unlink(file_path); 测试完毕后删除文件
    return EXIT_SUCCESS;
}