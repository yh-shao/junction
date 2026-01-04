#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* cpu_hog(void* arg)
{
    double duration = 3.0;
    clock_t start = clock();
    clock_t end_goal = start + (clock_t)(duration * CLOCKS_PER_SEC);

    volatile long long counter = 0;
    while (clock() < end_goal) counter++;

    return NULL;
}

int main()
{
    int N = 2;
    pthread_t tids[2];

    for (int i = 0; i < N; i++) 
    {
        int ret = pthread_create(&tids[i], NULL, cpu_hog, NULL);
        if (ret != 0) 
        {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(ret));
            exit(1);
        }
    }

    for (int i = 0; i < N; i++) 
    {
        int ret = pthread_join(tids[i], NULL);
        if (ret != 0) 
        {
            fprintf(stderr, "pthread_join failed: %s\n", strerror(ret));
            exit(1);
        }
    }

    return 0;
}
