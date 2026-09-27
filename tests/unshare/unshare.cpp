#include "runtime.hpp"
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define N (2048*1024)       // Matrix size
#define THREADS_PER_PROCESS 4


int fw_server_idx = 0;
int fw_server_size = 0;

// Shared memory: dynamically allocated matrices
int rows_per_thread = N / THREADS_PER_PROCESS;
int columns_per_process = 0;

pthread_barrier_t process_barrier;

typedef struct {
    int thread_id;
    int *arr;
} ThreadArg;

void *foo2(void *arg) {
    ThreadArg *args = (ThreadArg *)arg;
    int thread_id = args->thread_id;
    int local_thread_id = thread_id % THREADS_PER_PROCESS;
    int proc_range = N / fw_server_size;
    int range = proc_range / THREADS_PER_PROCESS;
    int start = proc_range * fw_server_idx + range * local_thread_id;
    int end = start + range;
    printf("thread %d: start %d, end %d\n", local_thread_id, start, end);
    int fd = open("/dev/null", O_WRONLY);
    // UNSHARE_ITERS > 1 re-touches this thread's slice repeatedly so the
    // shared pages stay hot with a single accessor (the non-allocating
    // process for rank 0's half). Under coherent-region pressure that is the
    // case where the unshare-to-accessor policy should move the pages into
    // the accessor's DRAM instead of back to the allocator. Default 1 keeps
    // the original single-pass behavior used by kick-the-tires.
    const char *iters_env = getenv("UNSHARE_ITERS");
    int iters = iters_env ? atoi(iters_env) : 1;
    if (iters < 1) iters = 1;

    for (int it = 0; it < iters; it++)
    for (int i=start ; i<end; i++) {
        args->arr[i] = 2;
        write(fd, &args->arr[i], 4);
    }
    
    printf("accessed all\n");
    return NULL;
}

int main() {
    const char* fw_server_idx_ch = getenv("FW_SERVER_IDX");
    const char* fw_server_size_ch = getenv("FW_SERVER_SIZE");
    fw_server_idx = atoi(fw_server_idx_ch);
    fw_server_size = atoi(fw_server_size_ch);
    firework_init(fw_server_idx, fw_server_size, 1000, 1);

    int *arr;

    if (fw_server_idx == fw_server_size - 1) {
        arr = (int *)malloc(N * sizeof(int));

        if (!arr) {
            fprintf(stderr, "Memory allocation failed\n");
            return 1;
        }

        // Initialize matrices with random values
        for (int i = 0; i < N; i++) {
            arr[i] = (int)1;
        }
    }

    int total_threads = fw_server_size * THREADS_PER_PROCESS;
    printf("Total threads: %d\n", total_threads);

    pthread_barrier_init(&process_barrier, NULL, THREADS_PER_PROCESS);

    pthread_t threads[total_threads];
    ThreadArg *args = (ThreadArg*)malloc(total_threads * sizeof(ThreadArg));
    printf("args: %p\n", args);
    printf("arr: %p\n", arr);

    int thread_count = 0;
    int target = 0;

    if (fw_server_idx != fw_server_size - 1) {
        sleep(10000);
        return 0;
    }

    // Time the computation
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Spawn threads for each block
    while (thread_count < total_threads) {
        args[thread_count].thread_id = thread_count;
        args[thread_count].arr = arr;
        target = thread_count / THREADS_PER_PROCESS;
        threads[thread_count] = remote_pthread_create(target, (void*)foo2, &args[thread_count]);
        thread_count++;
    }

    // Join threads
    for (int i = 0; i < total_threads; i++) {
        target = i / THREADS_PER_PROCESS;
        remote_pthread_join(threads[i], target);
    }

    // End timing
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("Elapsed time: %f seconds\n", elapsed);
    fflush(stdout);

    for (int i=0; i<N; i++) {
        if (arr[i] != 2) {
            printf("Error: arr[%d] = %d\n", i, arr[i]);
            return 1;
        }
    }

    printf("Correct!\n");
    show_debug_fs();

    return 0;
}