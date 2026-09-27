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

#ifndef THREADS_PER_PROC
#define THREADS_PER_PROC 4
#endif

#define TABLE_SIZE (400 * 1024 * 1024) // 400MB
#define ENTRY_SIZE (8)
#define ENTRY_NUMBER (TABLE_SIZE / ENTRY_SIZE)
#define COLLISION_NUM 3
#define KEY_RANGE (ENTRY_NUMBER / COLLISION_NUM)

#define IRESULT_VALUES 5 // 5 was default

// Shared-heap baseline: build with -DSHARE_EVERYTHING (see eval.sh).
#ifdef SHARE_EVERYTHING
#define malloc malloc_shared
#endif

struct Table {
    int key;
    int value;
};

struct IntermediateResultEntry {
    std::atomic<int> idx;
    int values[IRESULT_VALUES];
};

struct ArgData {
    int id;
    IntermediateResultEntry* result_entries;
};

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

int fw_server_size;
int total_thread_num = 0;

struct Table* bigtablea = nullptr;
struct Table* bigtableb = nullptr;


Table* read_small_data();
Table* read_large_table();


void* bench_small_th(void* arg) {
    struct ArgData* argdata = static_cast<ArgData*>(arg);
    int id = argdata->id;

    Table* result_entries = (Table*)malloc(TABLE_SIZE);
    IntermediateResultEntry* local_intermediates = (IntermediateResultEntry*)malloc(KEY_RANGE * sizeof(IntermediateResultEntry));

    Table* tablea = read_small_data();
    Table* tableb = read_small_data();

    auto start = std::chrono::high_resolution_clock::now();
    for (int i=0; i<ENTRY_NUMBER; i++) {
        int key = tablea[i].key;
        int value = tablea[i].value;
        int pos = local_intermediates[key].idx.fetch_add(1) % IRESULT_VALUES;
        local_intermediates[key].values[pos] = value;
    }

    int count = 0;
    for (int i=0; i<ENTRY_NUMBER; i++) {
        int pos = local_intermediates[tableb[i].key].idx.load();
        if (pos > 0) {
            for (int j=0; j<IRESULT_VALUES; j++) {
                if (j == pos) {
                    break;
                }
                int value = local_intermediates[tableb[i].key].values[j] + tableb[i].value;
                int index = count % ENTRY_NUMBER;
                result_entries[index].key = tableb[i].key;
                result_entries[index].value = value;
                count++;
            }
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    printf("Thread %d: ", id);
    std::cout << "Elapsed time: " << elapsed.count() / 1000 << "s\n";

    double* result = (double*)malloc(sizeof(double));
    *result = elapsed.count() / 1000;
    end_th();

    return (void*)result;
}

void* bench_large_th(void* arg) {
    struct ArgData* argdata = static_cast<ArgData*>(arg);
    IntermediateResultEntry* global_intermediates = argdata->result_entries;
    int id = argdata->id;

    int offset_idx = id - total_thread_num * 3 / 4;

    Table* result_entries = (Table*)malloc(TABLE_SIZE);
    
    if ((fw_server_size==1 && offset_idx==0) || (fw_server_size > 1 && offset_idx%THREADS_PER_PROC == 0)) {
        bigtablea = read_large_table();
        bigtableb = read_large_table();
    }
    remote_threads_barrier_wait();
    assert(bigtablea != nullptr);
    assert(bigtableb != nullptr);

    Table* tmpa = (Table*) ((char*)bigtablea + (size_t)offset_idx * TABLE_SIZE);
    Table* tmpb = (Table*) ((char*)bigtableb + (size_t)offset_idx * TABLE_SIZE);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i=0; i<ENTRY_NUMBER; i++) {
        int key = tmpa[i].key;        
        int value = tmpa[i].value;
        int pos = global_intermediates[key].idx.fetch_add(1) % IRESULT_VALUES;
        global_intermediates[key].values[pos] = value;
    }

    remote_threads_barrier_wait();

    int count = 0;
    for (int i=0; i<ENTRY_NUMBER; i++) {
        int pos = global_intermediates[tmpb[i].key].idx.load();
        if (pos > 0) {
            for (int j=0; j<IRESULT_VALUES; j++) {
                if (j == pos) {
                    break;
                }
                int value = global_intermediates[tmpb[i].key].values[j] + tmpb[i].value;
                int index = count % ENTRY_NUMBER;
                result_entries[index].key = tmpb[i].key;
                result_entries[index].value = value;
                count++;
            }
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    printf("Thread %d: ", id);
    std::cout << "Elapsed time: " << elapsed.count() / 1000 << "s\n";

    double* result = (double*)malloc(sizeof(double));
    *result = elapsed.count() / 1000;
    end_th();

    return (void*)result;
}

Table* read_small_data() {
    const char* filename = "./data/input_small_400M";
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        perror("Error opening file\n");
        return NULL;
    }
    Table* table = (struct Table*)malloc(TABLE_SIZE);
    if (table == NULL) {
        perror("Error allocating memory\n");
        return NULL;
    }
    int res = read(fd, table, TABLE_SIZE);
    if (res == -1) {
        perror("Error reading file\n");
        return NULL;
    }
    assert(res == TABLE_SIZE);
    close(fd);
    return table;
}

struct Table* read_large_table() {
    const char* filename = "./data/input_large_400M";
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        perror("Error opening file\n");
        return NULL;
    }
    size_t large_table_size = (size_t)TABLE_SIZE * (total_thread_num / 4);
    struct Table* table = (struct Table*)malloc(large_table_size);
    if (table == NULL) {
        perror("Error allocating memory\n");
        return NULL;
    }
    size_t res = 0;
    size_t additional_table_size = 0;
    if (total_thread_num > 32) {
        large_table_size = (size_t)TABLE_SIZE * (32/4);
        additional_table_size = (size_t)TABLE_SIZE * (total_thread_num / 4 - 32/4);
    }
    while (res < large_table_size) {
        ssize_t bytes_read = read(fd, (char*)table + res, large_table_size - res);
        if (bytes_read == -1) {
            perror("Error reading file\n");
            return NULL;
        }
        res += bytes_read;
    }
    if (additional_table_size) {
        size_t additional_res = 0;
        lseek(fd, 0, SEEK_SET);
        while (additional_res < additional_table_size) {
            ssize_t bytes_read = read(fd, (char*)table + large_table_size + additional_res, additional_table_size - additional_res);
            if (bytes_read == -1) {
                perror("Error reading file\n");
                return NULL;
            }
            additional_res += bytes_read;
        }
        res += additional_res;
    }
    close(fd);

    return table;
}

void bench() { // executed by only main node
    remote_threads_barrier_init(total_thread_num/4);
    IntermediateResultEntry* global_intermediates = (IntermediateResultEntry*)malloc(KEY_RANGE * sizeof(IntermediateResultEntry));
    for (int i = 0; i < KEY_RANGE; i++) {
        global_intermediates[i].idx.store(0);
    }

    pthread_t threads[total_thread_num];

    for (int i = 0; i < total_thread_num; i++) {
        struct ArgData* argdata = (ArgData*)malloc(sizeof(struct ArgData));
        int target = i / THREADS_PER_PROC;
        argdata->result_entries = global_intermediates;
        argdata->id = i;

        if (i < total_thread_num*3/4) {
            threads[i] = remote_pthread_create(target, (void*)bench_small_th, argdata);
        } else {
            threads[i] = remote_pthread_create(target, (void*)bench_large_th, argdata);
        }
    }

    double sum = 0;
    for (int i = 0; i < total_thread_num; i++) {
        int target = i / THREADS_PER_PROC;
        double* t = (double*) remote_pthread_join(threads[i], target);
        sum += *t;
    }

    std::cout << "Total elapsed time: " << sum << "s\n";    
}

int main() {
    const char* fw_server_idx_ch = getenv("FW_SERVER_IDX");
    const char* fw_server_size_ch = getenv("FW_SERVER_SIZE");
    int fw_server_idx = atoi(fw_server_idx_ch);
    fw_server_size = atoi(fw_server_size_ch);
    total_thread_num = fw_server_size * THREADS_PER_PROC;
    assert(ENTRY_SIZE==sizeof(Table));
    firework_init(fw_server_idx, fw_server_size, 1000, 1);

    if (fw_server_idx == fw_server_size - 1) {
        bench();
    } else {
        sleep(1000);
    }
    show_debug_fs();
    
    return 0;
}