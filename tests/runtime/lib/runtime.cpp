#include "runtime.hpp"
#include "syncQueue.hpp"
#include "utils.hpp"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unordered_map>
#include <mutex>
#include <assert.h>
#include <random>

#define INITIAL_INTERVAL_MSEC 0.4
#define MSEC_DELTA_RATIO 0.25

SynchronousQueue *pthreadQueues[THD_QUEUE_NUM];

// Use an atomic accumulator to generate unique thread ids
int remote_thread_id = 0;
unsigned long *hint_zone_offset_bytes = (unsigned long*)0xbaadf00d;
std::unordered_map<remote_tid_t, pthread_t> remote_thread_map;
std::mutex mtx;


SynchronousQueue *migrateQueues[MIG_QUEUE_NUM];
SynchronousQueue *unshareQueues[UNSHARE_QUEUE_NUM];
SynchronousQueue *scanQueues[SCAN_QUEUE_NUM];


int pidfd = -1;
size_t pfn_range;
struct unshare_entry* ulist = nullptr;

int total_nodes = 0;


const uint64_t firework_heap_base = 0x1000000000; // 64GB
const uint64_t firework_heap_bound = 0x101000000000; // 16TB + 64GB
const uint64_t firework_heap_region_size = 0x10000000000; // 1TB
const uint64_t firework_stack_base = 0x600000000000;
const uint64_t firework_stack_bound = 0x602000000000;
const uint64_t firework_stack_region_size = 0x400000000; // 16GB
const uint64_t firework_hint_zone_base = 0x200000000000;
static size_t hint_zone_size = 0;

ShLock *shlock;

int mig_batch = 1;
double interval_msec = INITIAL_INTERVAL_MSEC;


struct mig_prefetch_entry* mig_prefetch_pfns = NULL;


size_t get_int(const char* filePath) {
    int fd = open(filePath, O_RDONLY);
    if (fd == -1) {
        printf("Error opening file %s: %s\n", filePath, strerror(errno));
        return -1;
    }

    // Buffer for string representation
    char buffer[32] = {0};
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer)-1);
    if (bytes_read == -1) {
        printf("Error reading from file %s: %s\n", filePath, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    // Remove newline if present
    char* newline = strchr(buffer, '\n');
    if (newline) *newline = '\0';

    // Convert string to integer
    return strtoul(buffer, NULL, 10);
}

int put_int(const char* filePath, int value) {
    char command[100];
    snprintf(command, sizeof(command), "%d", value);
    int fd = open(filePath, O_WRONLY);
    if (fd == -1) {
        printf("Error opening file %s: %s\n", filePath, strerror(errno));
        return -1;
    }
    size_t bytes_written = write(fd, command, strlen(command));
    if (bytes_written == -1ul) {
        printf("Error writing to file %s: %s\n", filePath, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

void clear_counter(const char* filePath) {
    // Write the process ID to debugfs
    char command[100] = "0";
    int fd = open(filePath, O_WRONLY);
    if (fd == -1) {
        printf("Error opening file %s: %s\n", filePath, strerror(errno));
        return;
    }
    size_t bytes_written = write(fd, command, strlen(command));
    if (bytes_written == -1ul) {
        printf("Error writing to file %s: %s\n", filePath, strerror(errno));
        close(fd);
        return;
    }
    close(fd);
    return;
}

int unshare_queue_init(size_t queue_capacity) {
    for (int i=0; i<UNSHARE_QUEUE_NUM; i++) {
        unshareQueues[i] = new SynchronousQueue(i + MIG_QUEUE_NUM + THD_QUEUE_NUM, true, queue_capacity);
    }
    return 0;
}

int scan_queue_init(size_t queue_capacity, bool clear) {
    for (int i=0; i<SCAN_QUEUE_NUM; i++) {
        scanQueues[i] = new SynchronousQueue(i + MIG_QUEUE_NUM + THD_QUEUE_NUM + UNSHARE_QUEUE_NUM, clear, queue_capacity);
    }
    return 0;
}

int remote_threads_init(int node_id, int num_nodes, size_t queue_capacity) {
    for (int i = 0; i< THD_QUEUE_NUM; i++) {
        pthreadQueues[i] = new SynchronousQueue(i + MIG_QUEUE_NUM, true, queue_capacity);
    }

    return 0;
}

void* daemon_execute(void* arg) {
    int node_id = *(int*)arg;
    printf("Daemon thread started on node %d\n", node_id);
    queue_data req;
    int index;

    remote_tid_t thread_id = -1;
    unsigned long thread_ret = 0;
    void *thread_ret_ptr = &thread_ret;
    int to_join = 0;
    struct saved_join_info saved_info;

    void *mig_pages[MAX_MIG_PREFETCH_SIZE* 128] = {NULL};
    unsigned long mig_pfns[MAX_MIG_PREFETCH_SIZE* 128] = {0};
    int mig_status[MAX_MIG_PREFETCH_SIZE* 128] = {0};
    int mig_srcnodes[MAX_MIG_PREFETCH_SIZE* 128] = {0};
    int mig_indexes[MAX_MIG_PREFETCH_SIZE* 128] = {0};
    
    void* pages_buf[MAX_MIG_PREFETCH_SIZE] = {NULL};
    unsigned long num_pages_buf[MAX_MIG_PREFETCH_SIZE] = {0};
    int pfns_idx_buf[MAX_MIG_PREFETCH_SIZE] = {0};
    int src_node_buf[MAX_MIG_PREFETCH_SIZE] = {0};
    int index_buf[MAX_MIG_PREFETCH_SIZE];
    int cur = 0;
    struct timespec start, now;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 99);
    std::uniform_int_distribution<> dist2(0, 4999);
    int interval_test_status = 0;
    double th0 = 0.0;
    double th1 = 0.0;
    double th2 = 0.0;

    int total_pages = 0;
    int buf_idx = 0;

    while(1) {
        if (to_join) {
            int ret = pthread_tryjoin_np(saved_info.thread, &thread_ret_ptr);
            if (ret == 0) {
                to_join = 0;
                pthreadQueues[node_id]->setReturnValue(saved_info.index, thread_ret_ptr);
                pthreadQueues[node_id]->finishEvent(saved_info.index, 2);
            }
            else if (ret != EBUSY) {
                to_join = 0;
                fprintf(stderr, "Error joining thread: %s\n", strerror(ret));
                pthreadQueues[node_id]->finishEvent(saved_info.index, 4);
            } else {
                goto unshare;
            }
        }

        index = pthreadQueues[node_id]->getEvent(&req);
        // queue is empty
        if (index == -1) {
            goto unshare;
        }

        switch(req.kind) {
            case Q_THD_CREATE: {
                printf("Creating thread: %d\n", req.payload.thd.id);
                pthread_t thread;
                thread_id = req.payload.thd.id;
                if (req.payload.thd.func == NULL) {
                    fprintf(stderr, "Function pointer is NULL\n");
                    pthreadQueues[node_id]->finishEvent(index, 3);
                    break;
                }
                int ret = pthread_create(&thread, NULL, (void *(*)(void *))req.payload.thd.func, req.payload.thd.args);
                if (ret != 0) {
                    fprintf(stderr, "Error creating thread: %s\n", strerror(ret));
                    ret = 3;
                } else {
                    ret = 2;
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        remote_thread_map.emplace(thread_id, thread);
                    }
                }
                pthreadQueues[node_id]->finishEvent(index, ret);
            } break;
            case Q_THD_JOIN: {
                printf("Joining thread: %d\n", req.payload.thd.id);
                // Join a thread
                pthread_t thread;
                int ret;
                thread_id = req.payload.thd.id;
                try {
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        thread = remote_thread_map.at(thread_id);
                    }
                    ret = pthread_tryjoin_np(thread, &thread_ret_ptr);
                    if (ret != 0) {
                        if (ret == EBUSY) {
                            to_join = 1;
                            saved_info.thread = thread;
                            saved_info.index = index;
                            goto unshare;
                        } else {
                            fprintf(stderr, "Error joining thread: %s\n", strerror(ret));
                            ret = 4;
                        }
                    } else {
                        pthreadQueues[node_id]->setReturnValue(index, thread_ret_ptr);
                        ret = 2;
                    }
                } catch (const std::out_of_range& oor) {
                    fprintf(stderr, "Thread id not found: %d\n", thread_id);
                    ret = 5;
                }
                pthreadQueues[node_id]->finishEvent(index, ret);
            } break;
            default: {
                fprintf(stderr, "Unknown request type: %d\n", req.kind);
                pthreadQueues[node_id]->finishEvent(index, 6);
            } break;
        }
unshare:
        index = unshareQueues[node_id]->getEvent(&req);
        // queue is empty
        if (index == -1) {
            goto migrate;
        }

        switch(req.kind) {
            case Q_UNMAP: {
                int unshare_num = req.payload.unmap2.unshare_num;
                struct iovec iov[unshare_num];
                int unmap_indexes[unshare_num];
                int unshare_indexes[unshare_num];
                int unmap_count = 0;
                int unshare_count = 0;
                for (int i=0; i<unshare_num; i++) {
                    if (!((1<<node_id) & ulist[i].sharer_bitmap.load())) {
                        continue;
                    }
                    if (ulist[i].owner == node_id) {
                        unshare_indexes[unshare_count++] = i;
                        continue;
                    }
                    unsigned long addr = ulist[i].addr;
                    iov[unmap_count].iov_base = (void*)(addr & ~(PAGE_SIZE - 1));
                    iov[unmap_count].iov_len = PAGE_SIZE;
                    unmap_indexes[unmap_count] = i;
                    unmap_count++;
                }

                syscall(481, pidfd, iov, unmap_count, MADV_DONTNEED, node_id); // syscall for batch madvise
                for (int i=0; i<unmap_count; i++) {
                    int unmap_index = unmap_indexes[i];
                    int old_value = ulist[unmap_index].sharer_bitmap.fetch_and(~(1 << node_id));
                    assert(std::__popcount(old_value) > 1);
                }

                void* pages[unshare_num];
                int nodes[unshare_num] = {0};
                int unmap_status[unshare_num] = {0};
                for (int i=0; i<unshare_count; i++) {
                    int unshare_index = unshare_indexes[i];
                    while(std::__popcount(ulist[unshare_index].sharer_bitmap.load()) != 1) {
                        std::this_thread::yield();
                    }
                    pages[i] = (void*)ulist[unshare_index].addr;
                }
                assert(unshare_count + unmap_count <= unshare_num);
                syscall(462, unshare_count, pages, nodes, unmap_status, 0); // syscall for unsharing
                unshareQueues[node_id]->finishEvent(index, 2);
            } break;
            default: {
                fprintf(stderr, "Unknown request type: %d\n", req.kind);
                unshareQueues[node_id]->finishEvent(index, 6);
            } break;
        }

migrate:
        if (cur > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed_msec = (now.tv_sec - start.tv_sec) * 1000.0 + (now.tv_nsec - start.tv_nsec) / 1e6;
            if (elapsed_msec >= interval_msec) { // timeout
                int total_pages = 0;
                int buf_idx = 0;
                for (int i=0; i<cur; i++) {
                    total_pages += num_pages_buf[i];
                    memcpy(mig_pfns + buf_idx, mig_prefetch_pfns[pfns_idx_buf[i]].pfns, num_pages_buf[i] * sizeof(unsigned long));
                    memset(mig_prefetch_pfns[pfns_idx_buf[i]].pfns, 0, num_pages_buf[i] * sizeof(unsigned long));
                    for (int j=0; j<num_pages_buf[i]; j++) {
                        mig_pages[buf_idx] = (void*)((uintptr_t)pages_buf[i] + j * PAGE_SIZE);
                        mig_srcnodes[buf_idx] = src_node_buf[i];
                        buf_idx++;
                    }
                }
                int ret = syscall(460, total_pages, mig_pages, mig_pfns, mig_status, mig_srcnodes);

                for (int i=0; i<cur; i++) {
                    migrateQueues[node_id]->finishEvent(index_buf[i], 2);
                }

                /*
                 * Adaptive batching timeout: every so often perturb
                 * interval_msec (shorter, then longer) and keep the direction
                 * that yields more requests per millisecond; occasionally
                 * reset to the initial value. States 1-4 are the steps of one
                 * such probe.
                 */
                if (interval_test_status == 3) {
                    th1 = cur / interval_msec;
                    if (th0 > th1) {
                        interval_msec = interval_msec / (1.0-MSEC_DELTA_RATIO);
                        interval_test_status = 0;
                    } else if (cur == 1) {
                        interval_test_status = 0;
                    }
                     else {
                        interval_msec = interval_msec * (1.0-MSEC_DELTA_RATIO);
                        th0 = th1;
                    }
                    goto migrate2;
                }

                if (interval_test_status == 4) {
                    th2 = cur / interval_msec;
                    if (th0 > th2) {
                        interval_msec = interval_msec / (1.0+MSEC_DELTA_RATIO);
                        interval_test_status = 0;
                    } else {
                        interval_msec = interval_msec * (1.0+MSEC_DELTA_RATIO);
                        th0 = th2;
                    }
                    goto migrate2;
                }

                if (interval_test_status == 2) {
                    th2 = cur / interval_msec;
                    if (th0 > th1 && th0 > th2) {
                        interval_msec = interval_msec / (1.0+MSEC_DELTA_RATIO);
                        interval_test_status = 0;
                    }
                    else if (th1 > th0 && th1 > th2) {
                        interval_msec = interval_msec * (1.0-MSEC_DELTA_RATIO) / (1.0+MSEC_DELTA_RATIO);
                        interval_msec = interval_msec * (1.0-MSEC_DELTA_RATIO);
                        th0 = th1;
                        interval_test_status = 3;
                    }
                    else {
                        interval_msec = interval_msec * (1.0+MSEC_DELTA_RATIO);
                        th0 = th2;
                        interval_test_status = 4;
                    }

                    goto migrate2;
                }

                if (interval_test_status == 1) {
                    th1 = cur / interval_msec;
                    interval_msec = interval_msec * (1.0+MSEC_DELTA_RATIO) / (1.0-MSEC_DELTA_RATIO);
                    interval_test_status = 2;
                }

                if (dist(gen) == 0 && interval_test_status == 0) {
                    th0 = cur / interval_msec;
                    interval_msec = interval_msec * (1.0-MSEC_DELTA_RATIO);
                    interval_test_status = 1;
                }

                if (dist2(gen) == 0 && interval_test_status == 0) {
                    interval_msec = INITIAL_INTERVAL_MSEC;
                }
migrate2:
                cur = 0;
            }
        }

        index = migrateQueues[node_id]->getEvent(&req);
        if (index == -1) {
            goto scan;
        }
        if (req.kind != Q_MIGRATE) {
            fprintf(stderr, "Error: unexpected request type: %d\n", req.kind);
            migrateQueues[node_id]->finishEvent(index, 3);
            goto scan;
        }

        if (req.payload.mig.num_pages > 256) {
            printf("Error: too many pages to migrate\n");
            migrateQueues[node_id]->finishEvent(index, 3);
            goto scan;
        }

        index_buf[cur] = index;
        pages_buf[cur] = (void*)req.payload.mig.addr;
        src_node_buf[cur] = req.src_node;
        num_pages_buf[cur] = req.payload.mig.num_pages;
        pfns_idx_buf[cur++] = req.payload.mig.pfns_idx;
        if (cur == 1) {
            clock_gettime(CLOCK_MONOTONIC, &start);
        }
        if (cur<mig_batch) goto scan;


        total_pages = 0;
        buf_idx = 0;
        for (int i=0; i<cur; i++) {
            total_pages += num_pages_buf[i];
            memcpy(mig_pfns + buf_idx, mig_prefetch_pfns[pfns_idx_buf[i]].pfns, num_pages_buf[i] * sizeof(unsigned long));
            memset(mig_prefetch_pfns[pfns_idx_buf[i]].pfns, 0, num_pages_buf[i] * sizeof(unsigned long));
            for (int j=0; j<num_pages_buf[i]; j++) {
                mig_pages[buf_idx] = (void*)((uintptr_t)pages_buf[i] + j * PAGE_SIZE);
                mig_srcnodes[buf_idx] = src_node_buf[i];
                buf_idx++;
            }
        }
                
        syscall(460, total_pages, mig_pages, mig_pfns, mig_status, mig_srcnodes);
        

        for (int i=0; i<cur; i++) {
            migrateQueues[node_id]->finishEvent(index_buf[i], 2);
        }
        cur = 0;

scan:
        index = scanQueues[node_id]->getEvent(&req);
        if (index == -1) {
            std::this_thread::yield();
            continue;
        }

        if (req.kind != Q_SCAN) {
            fprintf(stderr, "Error: unexpected request type: %d\n", req.kind);
            scanQueues[node_id]->finishEvent(index, 3);
            continue;
        }

        syscall(491);
        scanQueues[node_id]->finishEvent(index, 2);
    }
}

void daemon_thread_create(int node_id) {
    pthread_t daemon_thread;
    int* arg = (int*)malloc(sizeof(int));
    *arg = node_id;
    pthread_create(&daemon_thread, NULL, daemon_execute, arg);
    pthread_detach(daemon_thread);
}

int firework_init(int node_id, int num_nodes, size_t queue_capacity, size_t vector_entry_num) {
    void *ret = 0;
    total_nodes = num_nodes;

    // Line-buffer stdout even when redirected to a file, so that output from
    // ranks that never exit normally (idle ranks killed by the orchestrator)
    // is not lost in a full buffer.
    setvbuf(stdout, NULL, _IOLBF, 0);

    put_int("/sys/kernel/debug/firework/num_nodes", num_nodes);
    put_int("/sys/kernel/debug/firework/scan_needed", 0);

    record_pid(node_id);
    // shared_memory_init, sharing_map_size (0 can be default), queue_capacity
    syscall(452, 0, queue_capacity);
    // call malloc to trigger malloc initialization
    void *tmp = malloc(0x1000);
    if (tmp == NULL) {
        perror("Error allocating memory\n");
        return -1;
    }
    free(tmp);

    // Register the heaps and stacks of other processes to avoid segfaults
    for (int i = 0; i < num_nodes; i++) {
        if (i == node_id) {
            continue;
        }
        ret = mmap((void*)(firework_heap_base + i * firework_heap_region_size), firework_heap_region_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (ret != (void*)(firework_heap_base + i * firework_heap_region_size)) {
            printf("%s: Error mapping remote heap. ret: %p; errno: %d\n", __func__, ret, errno);
            return -1;
        }
        ret = mmap((void*)(firework_stack_base + i * firework_stack_region_size), firework_stack_region_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (ret != (void*)(firework_stack_base + i * firework_stack_region_size)) {
            printf("%s: Error mapping remote stack. ret: %p; errno: %d\n", __func__, ret, errno);
            return -1;
        }
    }

    size_t hint_zone_mode = get_int("/sys/kernel/debug/firework/hint_zone_mode");
    printf("Hint Zone Mode: %lu\n", hint_zone_mode);

    switch(hint_zone_mode) {
        case 0: // Plain
            hint_zone_size = SH_HINT_ZONE_SIZE_BYTES;
            break;
        case 1: // Interleaved
            hint_zone_size = 2 * SH_HINT_ZONE_SIZE_BYTES;
            break;
        case 2: // Tiered
            size_t fast_tier_size_bytes = get_int("/sys/kernel/debug/firework/fast_tier_size");
            printf("Fast Tier Size: %luGB\n", fast_tier_size_bytes >> 30);
            hint_zone_size = fast_tier_size_bytes + SH_HINT_ZONE_SIZE_BYTES_DAX1; // 2 * SH_HINT_ZONE_SIZE_BYTES
            break;
    }

    printf("Hint Zone Size: %luGB\n", hint_zone_size>>30);

    ret = mmap((void*)firework_hint_zone_base, hint_zone_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ret != (void*)(firework_hint_zone_base)) {
        printf("%s: Error mapping hint zone. ret: %p; errno: %d\n", __func__, ret, errno);
        return -1;
    }
    // Set up a bump pointer in the shared memory
    hint_zone_offset_bytes = (unsigned long*)firework_hint_zone_base;
    if (node_id == 0) {
        // Reserve one page for metadata
        // e.g., the offset bump pointer
        __atomic_store_n(hint_zone_offset_bytes, PAGE_SIZE, __ATOMIC_SEQ_CST);
    }

    pidfd = syscall(SYS_pidfd_open, getpid(), 0);
    if (pidfd == -1) {
        perror("Error opening pidfd\n");
        return -1;
    }

    int fd = open("/dev/dax0.0", O_RDWR);
    if (fd == -1) {
        perror("Error opening file\n");
        return -1;
    }

    size_t hwc_size = get_int("/sys/kernel/debug/firework/hwc_size");
    if (hwc_size <= 0) {
        fprintf(stderr, "Error getting HWC size\n");
        return -1;
    }
    pfn_range = hwc_size >> 12; // page shift

    ulist = (struct unshare_entry*)mmap(NULL, UNSHARE_BATCH_SIZE * sizeof(struct unshare_entry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, SH_UNSHARE_LIST_OFFSET_BYTES + 0 * UNSHARE_BATCH_SIZE * sizeof(struct unshare_entry));
    if (ulist == MAP_FAILED) {
        perror("Error mapping unshare list\n");
        return -1;
    }
    memset(ulist, 0, UNSHARE_BATCH_SIZE * sizeof(struct unshare_entry));

    mig_batch = 4 * num_nodes;

    if (mig_queues_init(queue_capacity)) {
        perror("Error initializing migration queues\n");
        return -1;
    }
    if (node_id == 0)scan_queue_init(queue_capacity, true);
    else scan_queue_init(queue_capacity, false);

    unshare_queue_init(queue_capacity);
    remote_threads_init(node_id, num_nodes, queue_capacity);
    printf("Remote threads initialized\n");
    if (vector_entry_num) {
        shlock_init(vector_entry_num);
        printf("Shlock initialized\n");
    }
    if (node_id == 0) {
        unshare_thread_create(node_id);
    }
    daemon_thread_create(node_id);

    mig_prefetch_pfns = (struct mig_prefetch_entry*)mmap(NULL, MIG_PREFETCH_ENTRY_NUM * sizeof(struct mig_prefetch_entry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, SH_MIG_BUFFER_OFFSET_BYTES);
    if (mig_prefetch_pfns == MAP_FAILED) {
        perror("Error mapping mig prefetch pfns\n");
        return -1;
    }
    memset(mig_prefetch_pfns, 0, MIG_PREFETCH_ENTRY_NUM * sizeof(struct mig_prefetch_entry));
    clear_counter("/sys/kernel/debug/firework/pf_counter");
    clear_counter("/sys/kernel/debug/firework/mig_counter1");
    clear_counter("/sys/kernel/debug/firework/mig_counter2");
    printf("Counters cleared\n");
    return 0;
}

int firework_exit() {
    munmap(sh_queues_start, SH_QUEUE_TOTAL_SIZE);
    munmap(sh_lock_start, SH_LOCK_SIZE_BYTES);
    return 0;
}

int unshare_thread_create(int node_id) {
    pthread_t unshare_thread;
    int *arg = (int*)malloc(sizeof(int));
    *arg = node_id;
    int res = pthread_create(&unshare_thread, NULL, unshare_poll, arg);
    if (res != 0) {
        perror("Error creating thread\n");
        return -1;
    }
    pthread_detach(unshare_thread);

    return 0;
}

void* unshare_poll(void* arg) {
    int node_id = *(int*) arg;
    assert(node_id == 0);
    struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 100000000,
    };

    while(!program_end()) {
        syscall(480);
        nanosleep(&ts, NULL);
    }
    printf("Unshare thread exiting\n");

    return NULL;
}

remote_tid_t remote_pthread_create(int target, void *func, void *args) {
    remote_tid_t tid = __sync_fetch_and_add(&remote_thread_id, 1);

    queue_data req;
    req.kind = Q_THD_CREATE;
    // Assign a function pointer to the func member
    req.payload.thd.func = (void *)func;
    req.payload.thd.args = args;
    req.payload.thd.id = tid;

    printf("Creating a remote thread: %d\n", tid);
    int ret = pthreadQueues[target]->produce(req);

    if (ret != 2) {
        fprintf(stderr, "Failed to create the thread: %d\n", ret);
        exit(EXIT_FAILURE);
        return -1;
    }

    return tid;
}

void* remote_pthread_join(remote_tid_t tid, int target) {
    printf("Joining a remote thread: %d\n", tid);
    queue_data req;
    void *returnValue = NULL;
    req.kind = Q_THD_JOIN;
    req.payload.thd.id = tid;

    int ret = pthreadQueues[target]->produce(req, &returnValue);

    if (ret != 2) {
        fprintf(stderr, "Failed to join the thread: %d\n", ret);
        exit(EXIT_FAILURE);
    }

    return returnValue;
}

int mig_queues_init(size_t queue_capacity) {
    int fd = open("/dev/dax0.0", O_RDWR);   
    if (fd == -1) {
        perror("Error opening file\n");
        return -1;
    }

    sh_queues_start = mmap(NULL, SH_QUEUE_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (sh_queues_start == MAP_FAILED) {
        perror("Error mapping file\n");
        return -1;
    }

    for (int i=0; i<MIG_QUEUE_NUM; i++) {
        migrateQueues[i] = new SynchronousQueue(i, true, queue_capacity);
    }

    return 0;
}

void record_pid(int idx) {
    // Write the process ID to debugfs
    char command[100];
    char filePath[100];
    snprintf(command, sizeof(command), "%d", getpid());
    snprintf(filePath, sizeof(filePath), "/sys/kernel/debug/firework/pid_%d", idx);
    printf("pid: %d, idx: %d\n", getpid(), idx);
    int fd = open(filePath, O_WRONLY);
    if (fd == -1) {
        printf("Error opening file %s: %s\n", filePath, strerror(errno));
        return;
    }
    size_t bytes_written = write(fd, command, strlen(command));
    if (bytes_written == -1ul) {
        printf("Error writing to file %s: %s\n", filePath, strerror(errno));
        close(fd);
        return;
    }
    close(fd);
    return;
}

int shlock_init(size_t entry_num) {
    int fd = open("/dev/dax0.0", O_RDWR);   
    if (fd == -1) {
        perror("Error opening file\n");
        return -1;
    }

    sh_lock_start = mmap(NULL, SH_LOCK_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SH_QUEUE_SIZE * SH_QUEUE_NUM);
    close(fd);
    if (sh_lock_start == MAP_FAILED) {
        perror("Error mapping file\n");
        return -1;
    }
    memset(sh_lock_start, 0, SH_LOCK_SIZE_BYTES);

    shlock = new ShLock(sh_lock_start, entry_num);

    return 0;
}

void remote_threads_barrier_init(int count) {
    shlock->lock_map[shlock->entry_num] = count;
    shlock->lock_map[shlock->entry_num + 1] = 0;
    shlock->lock_map[shlock->entry_num + 2] = 0;
}

void remote_threads_barrier_wait() {
    int local_phase = shlock->lock_map[shlock->entry_num + 2].load(std::memory_order_seq_cst);
    
    int arrived = shlock->lock_map[shlock->entry_num + 1].fetch_add(1, std::memory_order_seq_cst) + 1;

    if (arrived == shlock->lock_map[shlock->entry_num]) {
        shlock->lock_map[shlock->entry_num + 1].store(0, std::memory_order_seq_cst);
        shlock->lock_map[shlock->entry_num + 2].fetch_add(1, std::memory_order_seq_cst);
    } else {
        while (shlock->lock_map[shlock->entry_num + 2].load(std::memory_order_seq_cst) == local_phase) {
            std::this_thread::yield();
        }
    }
}

void remote_threads_barrier2_init(int count) {
    shlock->lock_map[shlock->entry_num + 3] = count;
    shlock->lock_map[shlock->entry_num + 4] = 0;
    shlock->lock_map[shlock->entry_num + 5] = 0;
}

void remote_threads_barrier2_wait() {
    int local_phase = shlock->lock_map[shlock->entry_num + 5].load(std::memory_order_seq_cst);
    
    int arrived = shlock->lock_map[shlock->entry_num + 4].fetch_add(1, std::memory_order_seq_cst) + 1;

    if (arrived == shlock->lock_map[shlock->entry_num + 3]) {
        shlock->lock_map[shlock->entry_num + 4].store(0, std::memory_order_seq_cst);
        shlock->lock_map[shlock->entry_num + 5].fetch_add(1, std::memory_order_seq_cst);
    } else {
        while (shlock->lock_map[shlock->entry_num + 5].load(std::memory_order_seq_cst) == local_phase) {
            std::this_thread::yield();
        }
    }
}


ShLock::ShLock(void* lock_memory, size_t entry_num) : entry_num(entry_num) {
    lock_map = reinterpret_cast<std::atomic<uint8_t>*>(lock_memory);
}

void ShLock::lock(size_t index) {

    while (true) {
        uint8_t expected = lock_map[index].load(std::memory_order_seq_cst);
        if (expected == 0) {
            if (lock_map[index].compare_exchange_weak(expected, 1, std::memory_order_seq_cst)) {
                return;
            }
        }
    }
}

void ShLock::unlock(size_t index) {

    uint8_t expected;
    do {
        expected = lock_map[index].load(std::memory_order_seq_cst);
    } while (!lock_map[index].compare_exchange_weak(expected, 0, std::memory_order_seq_cst));
}

// RW lock layout (uint8_t):
//   bit 7 (0x80) = writer holds the lock
//   bits 0-6    = reader count (max 127, safe for <=32 threads)
// wlock: CAS 0 -> 0x80  (writer waits until no readers and no writer)
// rlock: CAS n -> n+1 when bit7==0  (reader waits only for writer)

void ShLock::rlock(size_t index) {
    while (true) {
        uint8_t cur = lock_map[index].load(std::memory_order_seq_cst);
        if ((cur & 0x80) == 0) {
            if (lock_map[index].compare_exchange_weak(cur, cur + 1, std::memory_order_seq_cst))
                return;
        }
        std::this_thread::yield();
    }
}

void ShLock::runlock(size_t index) {
    lock_map[index].fetch_sub(1, std::memory_order_seq_cst);
}

void ShLock::wlock(size_t index) {
    while (true) {
        uint8_t expected = 0;
        if (lock_map[index].compare_exchange_weak(expected, 0x80, std::memory_order_seq_cst))
            return;
        std::this_thread::yield();
    }
}

void ShLock::wunlock(size_t index) {
    lock_map[index].store(0, std::memory_order_seq_cst);
}

void sh_lock(size_t index) {
    shlock->lock(index);
}

void sh_unlock(size_t index) {
    shlock->unlock(index);
}

void sh_rlock(size_t index) {
    shlock->rlock(index);
}

void sh_runlock(size_t index) {
    shlock->runlock(index);
}

void sh_wlock(size_t index) {
    shlock->wlock(index);
}

void sh_wunlock(size_t index) {
    shlock->wunlock(index);
}

void *malloc_shared(size_t size) {
    // Strawman solution: always make allocations page-aligned
    size_t aligned_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    unsigned long offset = __atomic_fetch_add(hint_zone_offset_bytes, aligned_size, __ATOMIC_SEQ_CST);
    // If offset+aligned size go beyond the hint zone, complain
    if (offset + aligned_size > hint_zone_size) {
        fprintf(stderr, "Hint zone exhausted. offset: %lx, size: %lx, hint zone size: %lx\n", offset, aligned_size, hint_zone_size);
        return NULL;
    }
    // If offset is not page-aligned, complain
    if (offset & (PAGE_SIZE - 1)) {
        fprintf(stderr, "Offset is not page-aligned: %lx\n", offset);
    }
    return (void*)(firework_hint_zone_base + offset);
}

void show_debug_fs() {
    put_int("/sys/kernel/debug/firework/program_end", 1);
    size_t pf_counter = get_int("/sys/kernel/debug/firework/pf_counter");
    size_t mig_counter1 = get_int("/sys/kernel/debug/firework/mig_counter1");
    size_t mig_counter2 = get_int("/sys/kernel/debug/firework/mig_counter2");
    size_t unshare_counter = get_int("/sys/kernel/debug/firework/unshare_counter");
    // pages unshared to a process other than their allocator (0 unless the
    // unshare_to_accessor policy is on); reported so every run records
    // whether that policy actually engaged
    size_t to_accessor = get_int("/sys/kernel/debug/firework/unshare_to_accessor_count");
    printf("Page Faults: %lu\n", pf_counter);
    printf("Migrations (demand): %lu\n", mig_counter1);
    printf("Migrations (prefetch): %lu\n", mig_counter2);
    printf("Unshares: %lu\n", unshare_counter);
    printf("Unshares to accessor: %lu\n", to_accessor);
    sleep(1);
}

void end_th() {
    if (mig_batch > 1) {
        mig_batch--;
    }
}

int program_end() {
    int ret = get_int("/sys/kernel/debug/firework/program_end");
    return ret;
}

int scan_needed() {
    int ret = get_int("/sys/kernel/debug/firework/scan_needed");
    return ret;
}
