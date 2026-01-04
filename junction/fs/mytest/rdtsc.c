#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#define LOOPS_PER_SEC 500000000ULL   // 估算值：现代 CPU 每秒大概能跑这么多次空循环 (大约 2-4 GHz)，如果你的机器很快，这个值大概对应 0.5s - 1s 的延迟

void* cpu_hog(void* arg) 
{
    unsigned long long total_iterations = 2 * LOOPS_PER_SEC; 

    for (unsigned long long i = 0; i < total_iterations; i++) 
        __asm__ volatile ("nop");
    
    return NULL;
}

int main()
{
    int N = 500; 
    pthread_t tids[N];

    printf("[INFO] Spawning %d threads. Each will burn approx 1-2s of CPU time...\n", N);

    for (int i = 0; i < N; i++) 
    {
        if (pthread_create(&tids[i], NULL, cpu_hog, NULL) != 0) 
            perror("pthread_create failed");
    }

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
   
    for (int i = 0; i < N; i++) pthread_join(tids[i], NULL); 

    printf("[INFO] All threads finished.\n");
    return 0;
}