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

#include <cryptopp/aes.h>
#include <cryptopp/filters.h>
#include <cryptopp/modes.h>
#include <snappy.h>

#define VECTOR_ENTRY_NUM (128 * 1024 * 1024)
#ifndef THREADS_PER_PROCESS
#define THREADS_PER_PROCESS 4
#endif
#define MAX_TOTAL_THREADS 48

#define ELEMENT_NUM_TOTAL 2000000000ull

// Shared-heap baseline: build with -DSHARE_EVERYTHING to route all
// allocations into the coherent shared region (see eval.sh).
#ifdef SHARE_EVERYTHING
#define malloc malloc_shared
#endif

constexpr static uint32_t kArrayEntrySize = 8192;
constexpr static uint32_t kNumArrayEntries = 2 << 20;

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

unsigned char iv[CryptoPP::AES::BLOCKSIZE];
unsigned char key[CryptoPP::AES::DEFAULT_KEYLENGTH];

CryptoPP::AES::Encryption aesEncryption(key, CryptoPP::AES::DEFAULT_KEYLENGTH);
CryptoPP::CBC_Mode_ExternalCipher::Encryption cbcEncryption(aesEncryption, iv);

int fw_server_size;
unsigned long total_num_threads = 0;
unsigned long elements_per_thread = 0;
int* zipf;

struct ArrayEntry {
    uint8_t data[kArrayEntrySize];
};

ArrayEntry* array_entries = nullptr;

struct GlobalEntry {
    int key;
    int value;
};

struct ArgData {
    GlobalEntry* global_entries;
    uint32_t id;
};

int map_get(GlobalEntry* map, int key) {
    int bucket_id = key % VECTOR_ENTRY_NUM;
    GlobalEntry& entry = map[bucket_id];
    volatile bool match = entry.key == key;
    ACCESS_ONCE(match);

    int val = entry.value;
    return val;
}

void map_put(GlobalEntry* map, int key, int value) {
    int bucket_id = std::hash<int>{}(key) % VECTOR_ENTRY_NUM;
    sh_lock(bucket_id);
    map[bucket_id] = {key, value};
    sh_unlock(bucket_id);
    return;
}

// Function to generate random access indices
uint8_t* generate_random_indices(size_t count) {
    uint8_t* indices = static_cast<uint8_t*>(malloc(count * sizeof(uint8_t)));
    if (!indices) {
        perror("Failed to allocate memory for random indices");
        exit(EXIT_FAILURE);
    }
    
    // Fill with sequential indices
    for (size_t i = 0; i < count; i++) {
        indices[i] = i;
    }
    
    // Shuffle using Fisher-Yates algorithm
    for (size_t i = count - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        uint8_t temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
    
    return indices;
}

int data_init() {
    // Access indices follow the precomputed zipf distribution in data/zipf_14.txt.
    int fd = open("./data/zipf_14.txt", O_RDONLY);
    if (fd == -1) {
        perror("Error opening file\n");
        return -1;
    }
    zipf = (int*)malloc(ELEMENT_NUM_TOTAL * sizeof(int));
    if (zipf == NULL) {
        perror("Error allocating memory\n");
        return -1;
    }
    unsigned long res = 0;
    while (res < ELEMENT_NUM_TOTAL * sizeof(int)) {
        unsigned long r = read(fd, ((char*)zipf) + res, ELEMENT_NUM_TOTAL * sizeof(int) - res);
        if (r == -1) {
            perror("Error reading file\n");
            return -1;
        }
        res += r;
    }
    printf("Read %ld bytes from file\n", res);
    assert(res == ELEMENT_NUM_TOTAL * sizeof(int));
    close(fd);

    uint32_t num_entries = kNumArrayEntries / fw_server_size;
    array_entries = (ArrayEntry*)malloc(num_entries * sizeof(ArrayEntry));

    // Initialize the array into a graph, assume each node is 64 bytes
    uint8_t *indices = generate_random_indices(kArrayEntrySize/64);
    ArrayEntry sample_entry;
    for (size_t i = 0; i < kArrayEntrySize/64; i++) {
        sample_entry.data[64*indices[i]] = indices[((i+1)%(kArrayEntrySize/64))];;
    }
    // Copy sample entry to all array entries
    for (uint32_t i = 0; i < num_entries; i++) {
        memcpy(array_entries[i].data, sample_entry.data, kArrayEntrySize);
    }

    printf("data initialization done\n");

    return 0;
}

void* bench_th(void* arg) {
    struct ArgData* argdata = static_cast<ArgData*>(arg);
    GlobalEntry* global_entries = argdata->global_entries;
    uint32_t id = argdata->id;

    uint32_t num_entries = kNumArrayEntries / fw_server_size;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, UINT32_MAX);

    uint8_t *visited = (uint8_t*)malloc(sizeof(uint8_t) * kArrayEntrySize/64);

    for (unsigned long i = 0; i < elements_per_thread; i++) {
        uint32_t random_index = zipf[elements_per_thread * id + i];
        int val = map_get(global_entries, random_index);
        ACCESS_ONCE(val);

        if (i % 32 == 0) { // For every 32 kv queries, follow a graph traversal
            uint32_t rand = dist(gen);
            const auto &entry = array_entries[rand % num_entries];

            uint32_t current = 0;
            memset(visited, 0, sizeof(uint8_t) * (kArrayEntrySize/64));
            for (size_t i = 0; i < kArrayEntrySize/64; i++) {
                visited[current] = 1;
                current = entry.data[current*64];
            }
            ACCESS_ONCE(current);
        }

        if (i%(elements_per_thread/10)==0) {
            printf("Done %ld percent\n", i/(elements_per_thread/10)*10);
        }
    }

    end_th();

    return NULL;
}

void bench() {
    GlobalEntry* global_entries = (GlobalEntry*)malloc(VECTOR_ENTRY_NUM * sizeof(GlobalEntry));
    // memset to a non-0 value, or the memset can be optimized away
    memset(global_entries, 7, VECTOR_ENTRY_NUM * sizeof(GlobalEntry));
    printf("Global entries allocated at %p\n", global_entries);

    printf("elements_per_thread: %lu\n", elements_per_thread);
    printf("global_entries: %p\n", global_entries);
    printf("global_entries ends at: %p\n", global_entries + VECTOR_ENTRY_NUM);

    pthread_t threads[MAX_TOTAL_THREADS];

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < total_num_threads; i++) {
        struct ArgData* argdata = (ArgData*)malloc(sizeof(struct ArgData));
        int target = i % fw_server_size;
        argdata->global_entries = global_entries;
        argdata->id = i;
        threads[i] = remote_pthread_create(target, (void*)bench_th, argdata);
    }

    for (size_t i = 0; i < total_num_threads; i++) {
        int target = i % fw_server_size;
        remote_pthread_join(threads[i], target);
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    std::cout << "Elapsed time: " << elapsed.count() / 1000 << "s\n";
    return;
}

int main() {
    const char* fw_server_idx_ch = getenv("FW_SERVER_IDX");
    const char* fw_server_size_ch = getenv("FW_SERVER_SIZE");
    int fw_server_idx = atoi(fw_server_idx_ch);
    fw_server_size = atoi(fw_server_size_ch);
    firework_init(fw_server_idx, fw_server_size, 1000, 0);

    total_num_threads = THREADS_PER_PROCESS * fw_server_size;
    if (total_num_threads > MAX_TOTAL_THREADS) {
        printf("Error: total_num_threads exceeds MAX_TOTAL_THREADS\n");
        return 0;
    }
    elements_per_thread = ELEMENT_NUM_TOTAL / total_num_threads;

    data_init();

    if (fw_server_idx == fw_server_size - 1) {
        bench();
    } else {
        sleep(10000);
    }
    show_debug_fs();
    
    return 0;
}