// Page sharing / unsharing throughput vs. batch size (Figure 8c).
//
// Two processes. The benchmark rank (rank 1) allocates a 1 GB array; a remote
// thread on rank 0 writes one word in every migration_batch_size-th page, so
// every access is a sharing fault that migrates a batch of
// migration_batch_size pages into the coherent region. Sharing throughput is
// the array size over the time of that loop. With FW_BATCH_UNSHARE=1 the
// benchmark rank then asks the kernel to unshare every shared page in
// batches of tmp_unshare_batch_size (syscall 493) and reports the unsharing
// throughput the same way.
#include "runtime.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <chrono>
#include <string.h>

#define ARRAY_SIZE (1024ul * 1024 * 1024)   // 1 GB
#define ENTRY_NUM (ARRAY_SIZE / sizeof(int))

int fw_server_size = 0;

struct ThreadArg { int* global_array; };

static double seconds_since(std::chrono::high_resolution_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
}

void* share_th(void* arg) {
    int* a = ((ThreadArg*)arg)->global_array;
    int batch = get_int("/sys/kernel/debug/firework/migration_batch_size");
    printf("migration_batch_size: %d\n", batch);
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < ENTRY_NUM; i += 1024 * batch)   // one touch per batch of pages
        a[i] = 1;
    double s = seconds_since(t0);
    printf("Sharing: %.3f s, %.1f MB/s\n", s, (ARRAY_SIZE >> 20) / s);
    return NULL;
}

int main() {
    int fw_server_idx = atoi(getenv("FW_SERVER_IDX"));
    fw_server_size = atoi(getenv("FW_SERVER_SIZE"));
    firework_init(fw_server_idx, fw_server_size, 1000, 0);
    if (fw_server_size != 2) { fprintf(stderr, "batching needs exactly 2 processes\n"); return 1; }

    if (fw_server_idx == fw_server_size - 1) {
        int* global_array = (int*)malloc(ARRAY_SIZE);
        // Materialise every page in the owner with non-zero data: a zero-fill
        // loop is turned into calloc() by the compiler and leaves the pages
        // untouched, and untouched pages cannot be shared.
        memset(global_array, 1, ARRAY_SIZE);
        ThreadArg* args = (ThreadArg*)malloc(sizeof(ThreadArg));
        args->global_array = global_array;
        pthread_t t = remote_pthread_create(0, (void*)share_th, args);
        remote_pthread_join(t, 0);

        const char* un = getenv("FW_BATCH_UNSHARE");
        if (un && atoi(un)) {
            int ubatch = get_int("/sys/kernel/debug/firework/tmp_unshare_batch_size");
            printf("tmp_unshare_batch_size: %d\n", ubatch);
            auto t0 = std::chrono::high_resolution_clock::now();
            syscall(493);                          // firework_do_unsharing_all
            double s = seconds_since(t0);
            printf("Unsharing: %.3f s, %.1f MB/s\n", s, (ARRAY_SIZE >> 20) / s);
        }
        printf("Benchmark completed\n");
    } else {
        sleep(10000);
    }
    show_debug_fs();
    return 0;
}
