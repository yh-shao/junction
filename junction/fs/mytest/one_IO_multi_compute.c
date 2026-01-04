#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>


static inline long long thread_cpu_ns() 
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void* cpu_hog(void* arg) 
{
    const long long target = 3LL * 1000000000LL;
    long long start = thread_cpu_ns();
    volatile long long counter = 0;
    while (thread_cpu_ns() - start < target) counter++;
    return NULL;
}

// void* cpu_hog(void* arg)
// {
//     // while (1) asm volatile("nop");    // 简单的计算指令，防止编译器优化掉空循环

//     double duration = 5.0;

//     clock_t start = clock();
//     clock_t end_goal = start + (clock_t)(duration * CLOCKS_PER_SEC);

//     volatile long long counter = 0;
//     while (clock() < end_goal) counter++; 

//     return NULL;
// }

int main()
{
    int N = 100;
    pthread_t tids[100];

    for (int i = 0; i < N; i++) 
    {
        if (pthread_create(&tids[i], NULL, cpu_hog, NULL) != 0)  // 创建干扰线程
            perror("pthread_create failed");
    }

    // usleep(1000);  // 稍微给一点时间让子线程跑起来

    // 主线程
    int fd = open("FSHAO:/file", O_RDWR | O_CREAT, 0644), ret;
    printf("[OPEN] ret val = %d\n", fd);

    char str[] = "This is file!";

    ret = write(fd, str, sizeof(str));
    printf("[WRITE] ret val = %d\n", ret);

    size_t n = 2000;
    char *buf = (char*)malloc(n);
    buf[0] = 0;  // touch 一下
    
    ret = read(fd, buf, n);
    
    printf("[READ] ret val = %d\n", ret);
    for (int i = 0; i < ret; i++) printf("%c", buf[i]);
    printf("\n");

    close(fd);

    for (int i = 0; i < N; i++) pthread_join(tids[i], NULL);  // 等待所有干扰线程结束


    exit(0);
}