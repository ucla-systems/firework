#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <thread>

typedef int remote_tid_t;


struct unshare_entry {
    uint64_t addr;
    int owner;
    std::atomic<int> sharer_bitmap;
};

#define MAX_MIG_PREFETCH_SIZE 128
#define MIG_PREFETCH_ENTRY_NUM 256
struct mig_prefetch_entry {
    unsigned long pfns[MAX_MIG_PREFETCH_SIZE];
};

struct saved_join_info {
    pthread_t thread;
    int index;
};

// These set of macros should always be consistent with the in-kernel ones
// defined in mm/firework_shared_mem.c
#define PAGE_SIZE 0x1000
#define SH_QUEUE_SIZE 0x10000 // 64KB
#define SH_QUEUE_SORT 4
#define NODE_NUM 8
#define SH_QUEUE_NUM (NODE_NUM * SH_QUEUE_SORT)
#define SH_QUEUE_TOTAL_SIZE (SH_QUEUE_SIZE * SH_QUEUE_NUM)
#define SH_QUEUE_ENTRY_SIZE 0x20 // 32 bytes
#define SH_QUEUE_ENTRY_NUM 2000
#define SH_QUEUE_HEADER_SIZE 64
#define SH_LOCK_SIZE_BYTES 0x100000000 // 4GB
#define SH_HINT_ZONE_SIZE_BYTES (0x400000000ul) // 16GB
#define SH_HINT_ZONE_SIZE_BYTES_DAX1 0x2400000000ULL // 144GB
#define MIG_QUEUE_NUM (SH_QUEUE_NUM / SH_QUEUE_SORT)

#define UNSHARE_BATCH_SIZE 1024

#define SH_HINT_ZONE_OFFSET_BYTES (SH_QUEUE_TOTAL_SIZE + SH_LOCK_SIZE_BYTES)
#define SH_FIFO_OFFSET_BYTES (SH_HINT_ZONE_OFFSET_BYTES + SH_HINT_ZONE_SIZE_BYTES)
#define SH_FIFO_SIZE_BYTES (pfn_range*sizeof(uint64_t))
#define SH_UNSHARE_LIST_OFFSET_BYTES (SH_FIFO_OFFSET_BYTES+SH_FIFO_SIZE_BYTES)
#define SH_UNSHARE_LIST_SIZE_BYTES (NODE_NUM*UNSHARE_BATCH_SIZE*sizeof(struct unshare_entry))
#define SH_IO_BUFFER_OFFSET_BYTES (SH_UNSHARE_LIST_OFFSET_BYTES+SH_UNSHARE_LIST_SIZE_BYTES)
#define SH_IO_BUFFER_SIZE_BYTES (PAGE_SIZE * 128)
#define SH_MIG_BUFFER_OFFSET_BYTES (SH_IO_BUFFER_OFFSET_BYTES+SH_IO_BUFFER_SIZE_BYTES)
#define SH_MIG_BUFFER_SIZE_BYTES (MIG_PREFETCH_ENTRY_NUM*sizeof(struct mig_prefetch_entry))


// End of macros matching the in-kernel ones

#define THD_QUEUE_NUM (SH_QUEUE_NUM / SH_QUEUE_SORT)
#define UNSHARE_QUEUE_NUM (SH_QUEUE_NUM / SH_QUEUE_SORT)
#define SCAN_QUEUE_NUM (SH_QUEUE_NUM / SH_QUEUE_SORT)


extern const uint64_t firework_heap_base;
extern const uint64_t firework_heap_bound;
extern const uint64_t firework_heap_region_size;
extern const uint64_t firework_stack_base;
extern const uint64_t firework_stack_bound;
extern const uint64_t firework_stack_region_size;

// Launch a thread on a `node_id` of a function at addr `func` with args `args`
// Return a thread id that can be used to join the thread
remote_tid_t remote_pthread_create(int node_id, void *func, void *args);
// Join a thread with id `tid` on a `node_id`
void* remote_pthread_join(remote_tid_t tid, int target);

// Set up the connection and thread that listen for remote thread spawning requests
int remote_threads_init(int node_id, int num_nodes, size_t queue_capacity = 0);


// initialize the firework runtime
int firework_init(int node_id, int num_nodes, size_t queue_capacity = 0, size_t vector_entry_num = 0);

int mig_queues_init(size_t queue_capacity = 0);
void record_pid(int idx);
int unshare_queue_init(size_t queue_capacity);
int firework_exit();
int shlock_init(size_t entry_num);
void sh_lock(size_t index);
void sh_unlock(size_t index);
void sh_rlock(size_t index);
void sh_runlock(size_t index);
void sh_wlock(size_t index);
void sh_wunlock(size_t index);
void remote_threads_barrier_init(int count);
void remote_threads_barrier_wait();
void remote_threads_barrier2_init(int count);
void remote_threads_barrier2_wait();
void *malloc_shared(size_t size);
void show_debug_fs();
int unshare_thread_create(int node_id);
void* unshare_poll(void* arg);
void end_th();
int scan_queue_init(size_t queue_capacity, bool clear);
int program_end();
int scan_needed();
size_t get_int(const char* filePath);
void* daemon_execute(void* arg);
void daemon_thread_create(int node_id);

class ShLock {
public:
    std::atomic<uint8_t>* lock_map;
    size_t entry_num;

public:
    ShLock(void* lock_memory, size_t entry_num);
    void lock(size_t index);
    void unlock(size_t index);
    // RW lock: bit7=write, bits0-6=reader count
    void rlock(size_t index);
    void runlock(size_t index);
    void wlock(size_t index);
    void wunlock(size_t index);
};