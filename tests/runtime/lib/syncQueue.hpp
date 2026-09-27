#pragma once
#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

// Assume queues are at the start of the shared memory
// Queue structure in the shared memory
// The first 64 bytes are for metadata (head, tail, finish)
// After the 64 bytes, it's the actual data and flags
// Suppose we have a maximum of 8 servers
// Each servers have 2 queues (migration queue, thread queue)
// If the max capacity is 2000, max queue element is 32 bytes
// The size of a queue is 2000 * 32 + 64 <= 64 * 1024 B = 64KB
// So we can reserve 64KB * 16 = 1 MB for the queues


enum qkind {
    Q_MIGRATE = 0,
    Q_THD_CREATE = 1,
    Q_THD_JOIN = 2,
    Q_UNMAP = 3,
    Q_UNSHARE = 4,
    Q_UNMAP2 = 5,     /* unused; kept so the numbering matches the kernel */
    Q_UNSHARE2 = 6,   /* unused; kept so the numbering matches the kernel */
    Q_SCAN = 7,
    Q_READ = 8,       /* unused; kept so the numbering matches the kernel */
};

struct queue_data {
    int kind;
    int src_node;
    union {
        struct {
            unsigned long addr;
            unsigned long num_pages;
            int pfns_idx;
        } mig;

        struct {
            void *func;
            void* args;
            remote_tid_t id;
        } thd;

        struct  {
            unsigned long addr;
            int owner;
        } unshare;

        struct {
            int head;
            int unshare_num;
        } unmap2;

        struct {
            int fd;
            size_t count;
        } rw;
    } payload; 
};

struct SynchronousQueue {
public:
    // Everything in the queue should be stored in shared memory
    queue_data *data;
    // Flags is used to indicate that an event has finished processing
    // 0 -> empty or not populated, ready to be produced by not ready to be consumed
    // 1 -> populated (ready to be consumed)
    // 2+ -> return values from the processing function, ready to be consumed but not yet ready to be produced
    char *flags;
    void **return_values; // Store retuan values for threading queue
    int *head; // Points to next element to be processed, if any
    int *tail; // Points to next empty slot to be filled
    int *done;
    size_t capacity = 2000;

    void setReturnValue(int index, void* value) { return_values[index] = value; }
    void* getReturnValue(int index) { return return_values[index]; }

    // Have a constructor to initialize the data
    SynchronousQueue(size_t index, bool clear = true, size_t queue_capacity = 0);
    int produce(const queue_data& event, void ** returnValue = NULL);
    int getEvent(queue_data* event);
    void finishEvent(int index, int ret = 1);
    void updateDone();
};