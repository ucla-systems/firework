#include "runtime.hpp"
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define N 8192     // Matrix size
#define THREADS_PER_COLUMN_BLOCK 4


#ifndef THREADS_PER_PROCESS
#define THREADS_PER_PROCESS 4
#endif


int fw_server_idx = 0;
int fw_server_size = 0;

// Shared memory: dynamically allocated matrices
int **process_A, **process_B, **process_C;
int rows_per_thread = N / THREADS_PER_COLUMN_BLOCK;
int columns_per_process = 0;

pthread_barrier_t process_barrier;

typedef struct {
    int thread_id;
    int *A;
    int *B;
    int *C;
    int server_idx;
} ThreadArg;

void *multiply_block(void *arg) {
    ThreadArg *args = (ThreadArg *)arg;
    int thread_id = args->thread_id;
    int local_thread_id = thread_id % THREADS_PER_COLUMN_BLOCK;
    int server_idx = args->server_idx;
    int row_start = local_thread_id * rows_per_thread;
    int col_start = server_idx * columns_per_process;

    // Each thread create a process-local buffer
    if (local_thread_id == 0) {
        // A is always duplicated on all processes
        process_A[server_idx] = (int *)malloc(N * N * sizeof(int));
        if (!process_A[server_idx]) {
            fprintf(stderr, "Memory allocation failed\n");
            return NULL;
        }
        memcpy(process_A[server_idx], args->A, N * N * sizeof(int));
    } else if (local_thread_id == 1) {
        // Only a range of columns is partitioned on B
        process_B[server_idx] = (int *)malloc(columns_per_process * N * sizeof(int));
        if (!process_B[server_idx]) {
            fprintf(stderr, "Memory allocation failed\n");
            return NULL;
        }
        for (int k = 0; k < N; k++)
            for (int j = 0; j < columns_per_process; j++)
                process_B[server_idx][k * columns_per_process + j] = args->B[k * N + (col_start + j)];

    } else if (local_thread_id == 2) {
        // Same size for C as B
        process_C[server_idx] = (int *)malloc(columns_per_process * N * sizeof(int));
        if (!process_C[server_idx]) {
            fprintf(stderr, "Memory allocation failed\n");
            return NULL;
        }
        memset(process_C[server_idx], 0, columns_per_process * N * sizeof(int));
    } else if (local_thread_id != 3) {
        printf("Error: local_thread_id = %d\n", local_thread_id);
    }

    pthread_barrier_wait(&process_barrier);

    // Perform matrix multiplication using thread-local memory

    for (int i = 0; i < rows_per_thread; i++) {
        for (int k = 0; k < N; k++) { 
            int a_val = process_A[server_idx][(i + row_start) * N + k];

            for (int j = 0; j < columns_per_process; j++) {
                process_C[server_idx][(i + row_start) * columns_per_process + j] += 
                    a_val * process_B[server_idx][k * columns_per_process + j];
            }
        }
    }

    // Store results back to shared memory
    for (int i = 0; i < rows_per_thread; i++)
        for (int j = 0; j < columns_per_process; j++)
            args->C[(row_start + i) * N + (col_start + j)] = process_C[server_idx][(i+row_start) * columns_per_process + j];
    
    printf("Thread %d finished multiplication\n", thread_id);

    return NULL;
}

int main() {
    const char* fw_server_idx_ch = getenv("FW_SERVER_IDX");
    const char* fw_server_size_ch = getenv("FW_SERVER_SIZE");
    fw_server_idx = atoi(fw_server_idx_ch);
    fw_server_size = atoi(fw_server_size_ch);
    firework_init(fw_server_idx, fw_server_size, 1000, 1);

    fw_server_idx = 0;
    fw_server_size = THREADS_PER_PROCESS / THREADS_PER_COLUMN_BLOCK;

    columns_per_process = N / fw_server_size;

    int *A, *B, *C;

    A = (int *)malloc(N * N * sizeof(int));
    B = (int *)malloc(N * N * sizeof(int));
    C = (int *)malloc(N * N * sizeof(int));

    process_A = (int **)malloc(fw_server_size * sizeof(int*));
    process_B = (int **)malloc(fw_server_size * sizeof(int*));
    process_C = (int **)malloc(fw_server_size * sizeof(int*));

    if (!A || !B || !C) {
        fprintf(stderr, "Memory allocation failed\n");
        return 1;
    }

    // Initialize matrices with random values
    for (int i = 0; i < N * N; i++) {
        A[i] = (int)1;
        B[i] = (int)1;
        C[i] = 0;
    }

    int total_threads = THREADS_PER_PROCESS;
    printf("Total threads: %d\n", total_threads);

    pthread_barrier_init(&process_barrier, NULL, THREADS_PER_PROCESS);

    pthread_t threads[total_threads];
    ThreadArg *args = (ThreadArg*)malloc(total_threads * sizeof(ThreadArg));
    int target = 0;

    // Time the computation
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Spawn threads for each block
    int thread_count = 0;
    while (thread_count < total_threads) {
        args[thread_count].thread_id = thread_count;
        args[thread_count].A = A;
        args[thread_count].B = B;
        args[thread_count].C = C;
        args[thread_count].server_idx = fw_server_idx;
        threads[thread_count] = remote_pthread_create(target, (void*)multiply_block, &args[thread_count]);
        thread_count++;
        if (thread_count % THREADS_PER_COLUMN_BLOCK == 0) {
            fw_server_idx += 1;
        }
    }

    // Join threads
    for (int i = 0; i < total_threads; i++) {
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

    return 0;
}