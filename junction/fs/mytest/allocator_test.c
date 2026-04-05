#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define DEFAULT_THREADS 8
#define DEFAULT_OPS     20000
#define DEFAULT_ROUND   32

typedef struct {
    int tid;
    int ops;
    int round_n;
    pthread_barrier_t* barrier;
} WorkerArg;

static int do_cmd(const char* path)
{
    int rc = open(path, O_RDONLY);
    if (rc < 0) 
        fprintf(stderr, "[USER] open failed: %s errno=%d (%s)\n", path, errno, strerror(errno));
    return rc;
}

static void* worker_main(void* arg_)
{
    WorkerArg* arg = (WorkerArg*)arg_;

    char path[128];
    snprintf(path, sizeof(path), "FSHAO:/__test__/round/%d", arg->round_n);

    pthread_barrier_wait(arg->barrier);

    for (int i = 0; i < arg->ops; ++i) {
        if (do_cmd(path) < 0) 
        {
            fprintf(stderr, "[USER] thread %d failed at iter %d\n", arg->tid, i);
            return (void*)1;
        }
    }

    return NULL;
}

int main(int argc, char** argv)
{
    int nthreads = DEFAULT_THREADS;
    int ops      = DEFAULT_OPS;
    int round_n  = DEFAULT_ROUND;

    if (argc >= 2) nthreads = atoi(argv[1]);
    if (argc >= 3) ops      = atoi(argv[2]);
    if (argc >= 4) round_n  = atoi(argv[3]);

    printf("[USER] simple stress start: threads=%d ops=%d round=%d\n", nthreads, ops, round_n);

    if (do_cmd("FSHAO:/__test__/check") < 0) 
    {
        fprintf(stderr, "[USER] pre-check failed\n");
        return 1;
    }

    pthread_t*   th = (pthread_t*)malloc(sizeof(pthread_t) * (size_t)nthreads);
    WorkerArg* args = (WorkerArg*)malloc(sizeof(WorkerArg) * (size_t)nthreads);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, (unsigned)nthreads);

    for (int t = 0; t < nthreads; ++t) 
    {
        args[t].tid = t;
        args[t].ops = ops;
        args[t].round_n = round_n;
        args[t].barrier = &barrier;

        if (pthread_create(&th[t], NULL, worker_main, &args[t]) != 0) 
        {
            perror("pthread_create");
            return 1;
        }
    }

    for (int t = 0; t < nthreads; ++t) 
    {
        void* ret = NULL;
        pthread_join(th[t], &ret);
        if (ret != NULL) 
        {
            fprintf(stderr, "[USER] worker %d failed\n", t);
            return 1;
        }
    }

    if (do_cmd("FSHAO:/__test__/check") < 0) 
    {
        fprintf(stderr, "[USER] post-check failed\n");
        return 1;
    }

    pthread_barrier_destroy(&barrier);
    free(th);
    free(args);

    printf("[USER] simple allocator stress test passed\n");
    return 0;
}