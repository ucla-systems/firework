#include "runtime.hpp"
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define N (8192)       // Matrix size
#ifndef THREADS_PER_PROCESS
#define THREADS_PER_PROCESS 4
#endif

// Shared-heap baseline: build with -DSHARE_EVERYTHING (see eval.sh).
#ifdef SHARE_EVERYTHING
#define malloc malloc_shared
#endif

int fw_server_idx = 0;
int fw_server_size = 0;

// Shared memory: dynamically allocated matrices
int *process_A, *process_B, *process_C;
int rows_per_thread[THREADS_PER_PROCESS];
int columns_per_process = 0;

pthread_barrier_t process_barrier;

typedef struct {
    int thread_id;
    int *A;
    int *B;
    int *C;
} ThreadArg;

void *multiply_block(void *arg) {
    ThreadArg *args = (ThreadArg *)arg;
    int thread_id = args->thread_id;
    int local_thread_id = thread_id % THREADS_PER_PROCESS;
    int row_start = 0;
    for (int i = 0; i < local_thread_id; i++) {
        row_start += rows_per_thread[i];
    }
    int col_start = fw_server_idx * columns_per_process;

    // Each thread create a process-local buffer
    if (local_thread_id == 0) {
        // A is always duplicated on all processes
        process_A = (int *)malloc(N * N * sizeof(int));
        if (!process_A) {
            fprintf(stderr, "Memory allocation failed\n");
            return NULL;
        }
        memcpy(process_A, args->A, N * N * sizeof(int));
        // Only a range of columns is partitioned on B
        process_B = (int *)malloc(columns_per_process * N * sizeof(int));
        if (!process_B) {
            fprintf(stderr, "Memory allocation failed\n");
            return NULL;
        }
        for (int k = 0; k < N; k++)
            for (int j = 0; j < columns_per_process; j++)
                process_B[k * columns_per_process + j] = args->B[k * N + (col_start + j)];
        // Same size for C as B
        process_C = (int *)malloc(columns_per_process * N * sizeof(int));
        if (!process_C) {
            fprintf(stderr, "Memory allocation failed\n");
            return NULL;
        }
        memset(process_C, 0, columns_per_process * N * sizeof(int));
    } 

    pthread_barrier_wait(&process_barrier);

    // Perform matrix multiplication using thread-local memory

    for (int i = 0; i < rows_per_thread[local_thread_id]; i++) {
        for (int k = 0; k < N; k++) {
            int a_val = process_A[(i + row_start) * N + k];
            for (int j = 0; j < columns_per_process; j++) {
                process_C[(i + row_start) * columns_per_process + j] += 
                    a_val * process_B[k * columns_per_process + j];
            }
        }
    }

    // Store results back to shared memory
    for (int i = 0; i < rows_per_thread[local_thread_id]; i++)
        for (int j = 0; j < columns_per_process; j++)
            args->C[(row_start + i) * N + (col_start + j)] = process_C[(i+row_start) * columns_per_process + j];
    
    printf("Thread %d finished multiplication\n", thread_id);
    end_th();

    return NULL;
}

int main() {
    const char* fw_server_idx_ch = getenv("FW_SERVER_IDX");
    const char* fw_server_size_ch = getenv("FW_SERVER_SIZE");
    fw_server_idx = atoi(fw_server_idx_ch);
    fw_server_size = atoi(fw_server_size_ch);
    firework_init(fw_server_idx, fw_server_size, 1000, 1);

    columns_per_process = N / fw_server_size;

    for (int i = 0; i < THREADS_PER_PROCESS; i++) {
        rows_per_thread[i] = N / THREADS_PER_PROCESS;
    }

    for (int i = 0; i < N%THREADS_PER_PROCESS; i++) {
        rows_per_thread[i] += 1;
    }

    int *A, *B, *C;

    if (fw_server_idx == fw_server_size - 1) {
        A = (int *)malloc(N * N * sizeof(int));
        B = (int *)malloc(N * N * sizeof(int));
        C = (int *)malloc(N * N * sizeof(int));

        if (!A || !B || !C) {
            fprintf(stderr, "Memory allocation failed\n");
            return 1;
        }

        // Initialize matrices with random values
        for (int i = 0; i < N * N; i++) {
            A[i] = (int)1;
            B[i] = (int)1;
            volatile int* ptr = &C[i];
            *ptr = 0;
        }
    }

    int total_threads = fw_server_size * THREADS_PER_PROCESS;
    printf("Total threads: %d\n", total_threads);

    pthread_barrier_init(&process_barrier, NULL, THREADS_PER_PROCESS);

    pthread_t threads[total_threads];
    ThreadArg *args = (ThreadArg*)malloc(total_threads * sizeof(ThreadArg));
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
        args[thread_count].A = A;
        args[thread_count].B = B;
        args[thread_count].C = C;
        target = thread_count / THREADS_PER_PROCESS;
        threads[thread_count] = remote_pthread_create(target, (void*)multiply_block, &args[thread_count]);
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

    int columns_calculated = columns_per_process * fw_server_size;

    // Checking the result
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < columns_calculated; j++) {
            if (C[i * N + j] != N) {
                printf("Error: C[%d][%d] = %d\n", i, j, C[i * N + j]);
                return 1;
            }
        }
    }

    printf("Correct!\n");
    show_debug_fs();

    return 0;
}