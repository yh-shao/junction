#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>
#define DISK_BLOCK_SIZE 4096   // 盘上每一块的大小

static inline uint32_t xorshift32(uint32_t *state)    
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// 传递给线程的参数结构体
typedef struct {
    int tid;                // 线程编号
    char filepath[256];     // 目标文件路径
    size_t file_size;       // 文件大小 (Bytes)
    size_t io_size;         // 读/写的大小 (Bytes)
    int r_pct;              // 读比例 (0-100)
    int w_pct;              // 写比例 (0-100)
    int op_count;           // 每个线程的操作次数
    int direct_io;          // 是否开启 O_DIRECT
    double elapsed_sec;     // 该线程压测耗时记录
    int actual_ops;
} thread_arg_t;

double get_time_sec() 
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

// 压测工作线程
void *worker_thread(void *arg)
{
    thread_arg_t *targ = (thread_arg_t *)arg;
    void *buf;

    if (posix_memalign(&buf, DISK_BLOCK_SIZE, targ->io_size) != 0)
    {
        perror("posix_memalign failed");
        exit(EXIT_FAILURE);
    }
    memset(buf, 0xAA, targ->io_size);

    int flags = O_RDWR;
    if (targ->direct_io) flags |= O_DIRECT;

    int fd = open(targ->filepath, flags);
    if (fd < 0)
    {
        perror("Failed to open file for testing");
        free(buf);
        exit(EXIT_FAILURE);
    }

    uint32_t seed = (time(NULL) ^ (targ->tid * 1999999973u)) | 1u;
    
    // 计算当前文件能够划分的最大块数
    size_t max_blocks = targ->file_size / targ->io_size;
    if (max_blocks == 0)
    {
        fprintf(stderr, "Thread %d: file size too small for block testing.\n", targ->tid);
        close(fd);
        free(buf);
        exit(EXIT_FAILURE);
    }

    double start_time = get_time_sec();
    int i = 0;
    for (; i < targ->op_count; i++)
    {
        // 随机生成文件偏移量和读写指令
        off_t offset = (xorshift32(&seed) % max_blocks) * targ->io_size;
        int is_read = (xorshift32(&seed) % 100) < targ->r_pct;

        if (is_read)
        {
            if (pread(fd, buf, targ->io_size, offset) < 0)
            {
                perror("pread error");
                break;
            }
        }
        else
        {
            if (pwrite(fd, buf, targ->io_size, offset) < 0)
            {
                perror("pwrite error");
                break;
            }
        }
    }
    targ->elapsed_sec = get_time_sec() - start_time;
    targ->actual_ops  = i;

    close(fd);
    free(buf);
    return NULL;
}

int main(int argc, char *argv[]) 
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc != 9) 
    {
        fprintf(stderr, "Usage: %s <THREAD_NUM> <DIR_PATHNAME> <FILE_SIZE> <R> <W> <OP_PERTHREAD> <directIO> <IO_SIZE>\n", argv[0]);
        fprintf(stderr, "Example (1MB Block): %s 4 /tmp 104857600 70 30 10000 1 1048576\n", argv[0]);
        return EXIT_FAILURE;
    }

    int    thread_num   = atoi(argv[1]);
    char*  dir_pathname = argv[2];
    size_t file_size    = atoll(argv[3]);
    int    r_pct        = atoi(argv[4]);
    int    w_pct        = atoi(argv[5]);
    int    op_perthread = atoi(argv[6]);
    int    direct_io    = atoi(argv[7]);
    size_t io_size      = atoll(argv[8]);

    if (r_pct + w_pct != 100) 
    {
        fprintf(stderr, "Error: R + W must equal 100.\n");
        return EXIT_FAILURE;
    }

    // O_DIRECT 对齐校验 
    if (direct_io && (io_size % DISK_BLOCK_SIZE != 0)) 
    {
        fprintf(stderr, "Error: When DirectIO is enabled, IO_SIZE must be a multiple of %u.\n", DISK_BLOCK_SIZE);
        return EXIT_FAILURE;
    }

    struct stat st = {0};
    if (stat(dir_pathname, &st) == -1)     
    {
        if (mkdir(dir_pathname, 0755) != 0) 
        {
            perror("Failed to create test directory");
            return EXIT_FAILURE;
        }
        printf("Created test directory: %s\n", dir_pathname);
    } 
    else 
    {
        if (!S_ISDIR(st.st_mode))   
        {
            fprintf(stderr, "Error: '%s' exists but is not a directory!\n", dir_pathname);
            return EXIT_FAILURE;
        }
    }

    printf("--- Test Configuration ---\n");
    printf("Threads: %d\nDirectory: %s\nFile Size: %zu Bytes\n", thread_num, dir_pathname, file_size);
    printf("per IO Size: %zu Bytes\n", io_size);
    printf("Read Ratio: %d%%\nWrite Ratio: %d%%\nOps per Thread: %d\nDirectIO: %s\n", r_pct, w_pct, op_perthread, direct_io ? "YES" : "NO");
    printf("--------------------------\n");

    pthread_t *threads  = malloc(thread_num * sizeof(pthread_t));
    thread_arg_t *targs = malloc(thread_num * sizeof(thread_arg_t));

    printf("Initializing files and allocating disk blocks...\n");

    // 阶段 1：使用固定的较大 Buffer 预生成文件，以加速初始化过程
    {
        size_t init_buf_size = 1024 * 1024; // 固定 1MB
        if (file_size < init_buf_size) {
            init_buf_size = file_size;
        }

        void *buf;
        posix_memalign(&buf, DISK_BLOCK_SIZE, init_buf_size);
        memset(buf, 0xAA, init_buf_size);

        for (int i = 0; i < thread_num; i++)
        {
            snprintf(targs[i].filepath, sizeof(targs[i].filepath), "%s/testfile_t%d.dat", dir_pathname, i);
            int fd = open(targs[i].filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd < 0) { perror("Failed to create file"); free(buf); return EXIT_FAILURE; }

            size_t written = 0;
            while (written < file_size)
            {
                size_t to_write = (file_size - written >= init_buf_size) ? init_buf_size : (file_size - written);
                if (write(fd, buf, to_write) < 0) { perror("Write failed"); close(fd); free(buf); return EXIT_FAILURE; }
                written += to_write;
            }
            fsync(fd);
            close(fd);
        }
        free(buf);
    }
    printf("File preparation complete.\n");

    // 阶段 2：创建并启动压测线程
    double wall_start = get_time_sec();   

    for (int i = 0; i < thread_num; i++)
    {
        targs[i].tid          = i;
        targs[i].file_size    = file_size;
        targs[i].io_size      = io_size;  // 传入动态 I/O 大小
        targs[i].r_pct        = r_pct;
        targs[i].w_pct        = w_pct;
        targs[i].op_count     = op_perthread;
        targs[i].direct_io    = direct_io;
        targs[i].elapsed_sec  = 0.0;

        if (pthread_create(&threads[i], NULL, worker_thread, &targs[i]) != 0)
        {
            perror("Failed to create thread");
            return EXIT_FAILURE;
        }
    }

    // 阶段 3：等待所有测试线程完成压测并收集数据
    long total_ops = 0;
    for (int i = 0; i < thread_num; i++)
    {
        pthread_join(threads[i], NULL);
        total_ops += targs[i].actual_ops;
    }

    double wall_elapsed = get_time_sec() - wall_start;  

    // 计算吞吐和 IOPS
    double iops = (wall_elapsed > 0) ? (total_ops / wall_elapsed) : 0;
    double throughput_mb = (iops * io_size) / (1024.0 * 1024.0);

    printf("\n--- Test Results ---\n");
    printf("Total Operations : %ld\n", total_ops);
    printf("Wall Elapsed Time: %.4f seconds\n", wall_elapsed);
    printf("Total IOPS       : %.2f\n", iops);
    printf("Throughput       : %.2f MB/s\n", throughput_mb);
    printf("--------------------\n");

    free(threads);
    free(targs);
    return EXIT_SUCCESS;
}