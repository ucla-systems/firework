#include "runtime.hpp"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <random>
#include <functional>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <assert.h>
#include <iostream>

#define ARRAY_SIZE 1024*1024*1024 // 1GB
#define COUNT 3000000
#define THREADS_PER_PROCESS 4
#define THREADS_NUM (THREADS_PER_PROCESS * fw_server_size)

// barrier
pthread_barrier_t barrier;

pthread_spinlock_t lock;

int fw_server_size = 0;

struct ThreadArg {
    int* global_array;
};


void* bench_th(void* arg) {
    remote_threads_barrier_wait();
    auto start = std::chrono::high_resolution_clock::now();

    size_t index = 0;
    while (index < COUNT) {
        sh_lock(0);
        sh_unlock(0);
        index++;
    }
    remote_threads_barrier_wait();

    auto end = std::chrono::high_resolution_clock::now();

    // One representative thread prints the aggregate result.
    if (arg != NULL) {
        std::chrono::duration<double, std::milli> elapsed = end - start;
        double sec = elapsed.count() / 1000;
        size_t total_ops = (size_t)THREADS_NUM * COUNT;
        std::cout << "Threads: " << THREADS_NUM
                  << "  Total ops: " << total_ops
                  << "  Elapsed: " << sec << "s"
                  << "  Throughput: " << (total_ops / sec) << " ops/s"
                  << "  Latency: " << (sec / total_ops * 1e9) << " ns/op\n";
    }

    return NULL;
}

void bench() {
    remote_threads_barrier_init(THREADS_NUM);

    pthread_t thread[THREADS_NUM];

    for (int i = 0; i < THREADS_NUM; i++) {
        int target = i % fw_server_size;
        // Only the first thread gets a non-NULL arg: it reports the result.
        void* arg = (i == 0) ? (void*)1 : NULL;
        thread[i] = remote_pthread_create(target, (void*)bench_th, arg);
    }

    for (int i = 0; i < THREADS_NUM; i++) {
        int target = i % fw_server_size;
        remote_pthread_join(thread[i], target);
    }


    printf("Benchmark completed successfully.\n");

    return;
}

void* bench_single_th(void* arg) {
    pthread_barrier_wait(&barrier);
    auto start = std::chrono::high_resolution_clock::now();

    size_t index = 0;
    while (index < COUNT) {
        pthread_spin_lock(&lock);
        pthread_spin_unlock(&lock);
        index++;
    }
    pthread_barrier_wait(&barrier);

    auto end = std::chrono::high_resolution_clock::now();

    // One representative thread prints the aggregate result.
    if (arg != NULL) {
        std::chrono::duration<double, std::milli> elapsed = end - start;
        double sec = elapsed.count() / 1000;
        size_t total_ops = (size_t)THREADS_PER_PROCESS * COUNT;
        std::cout << "Threads: " << THREADS_PER_PROCESS
                  << "  Total ops: " << total_ops
                  << "  Elapsed: " << sec << "s"
                  << "  Throughput: " << (total_ops / sec) << " ops/s"
                  << "  Latency: " << (sec / total_ops * 1e9) << " ns/op\n";
    }

    return NULL;
}

void bench_single() {
    // normal pthread lib
    pthread_barrier_init(&barrier, NULL, THREADS_PER_PROCESS);
    pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
    pthread_t thread[THREADS_PER_PROCESS];
    for (int i = 0; i < THREADS_PER_PROCESS; i++) {
        // Only the first thread gets a non-NULL arg: it reports the result.
        void* arg = (i == 0) ? (void*)1 : NULL;
        pthread_create(&thread[i], NULL, bench_single_th, arg);
    }
    for (int i = 0; i < THREADS_PER_PROCESS; i++) {
        pthread_join(thread[i], NULL);
    }
    printf("Single process benchmark completed successfully.\n");
    return;
}

int main() {
    const char* fw_server_idx_ch = getenv("FW_SERVER_IDX");
    const char* fw_server_size_ch = getenv("FW_SERVER_SIZE");
    int fw_server_idx = atoi(fw_server_idx_ch);
    fw_server_size = atoi(fw_server_size_ch);

    firework_init(fw_server_idx, fw_server_size, 1000, 1);

    if (fw_server_size == 1) {
        bench_single();
        return 0;
    }

    if (fw_server_idx == fw_server_size - 1) {
        bench();
    } else {
        sleep(10000);
    }
    show_debug_fs();
    
    return 0;
}