#include "runtime.hpp"
#include "syncQueue.hpp"
#include "utils.hpp"
#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <cstring>

#define QUEUE_HEADER_SIZE 64

SynchronousQueue::SynchronousQueue(size_t index, bool clear, size_t queue_capacity) {
    // Hack here: allocate a return value array only for threading queue
    bool return_array = false;
    if (index >= MIG_QUEUE_NUM) {
        return_array = true;
    }
    if (queue_capacity) {
        capacity = queue_capacity;
    }
    size_t actual_queue_size_bytes = QUEUE_HEADER_SIZE + capacity * sizeof(queue_data) + capacity * sizeof(bool);
    if (return_array) {
        actual_queue_size_bytes += capacity * sizeof(void*);
    }
    if (actual_queue_size_bytes > queue_size_bytes) {
        std::cerr << "Queue capacity exceeds the shared memory size" << std::endl;
        std::exit(1);
    }
    if (clear) {
        std::memset((char *)sh_queues_start + index * queue_size_bytes, 0, queue_size_bytes);
    }
    data = (queue_data *)((char *)sh_queues_start + index * queue_size_bytes + QUEUE_HEADER_SIZE);
    flags = ((char *)sh_queues_start + index * queue_size_bytes + QUEUE_HEADER_SIZE + capacity * sizeof(queue_data));
    if (return_array) {
        return_values = (void**)(flags + capacity * sizeof(char));
    } else {
        return_values = NULL;
    }
    head = (int *)((char *)sh_queues_start + index * queue_size_bytes);
    tail = head + 1;
    done = tail + 1;
}

int SynchronousQueue::produce(const queue_data& event, void **returnValue) {
    while (true) {
        int currentTail = __atomic_load_n(tail, __ATOMIC_SEQ_CST);
        int currentDone = __atomic_load_n(done, __ATOMIC_SEQ_CST);

        // Check if the queue is full
        // i.e., the state of next slot is ready to be produced
        if ((currentTail - currentDone) >= capacity) {
            // When working with small queues,
            // Sometimes done does not get a chance to be successfully updated
            // due to our optimistic compare and swap in finishEvent(),
            // causing infinite blocking.
            // Manually update done when we find that the queue is empty and
            // may be blocked (tail cannot be increased due to un-updated done)s
            updateDone();
            std::this_thread::yield();
            continue;
        }

        // Reserve the slot atomically
        if (__atomic_compare_exchange_n(tail, &currentTail, currentTail + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            int index = currentTail % capacity;
            data[index] = event;
            // Safe guard checking whether an in-progress event is overwritten
            if (__atomic_load_n(&flags[index], __ATOMIC_SEQ_CST) != 0) {
                std::cout << "An in-progress event is overwritten" << std::endl;
                exit(-1);
            }
            __atomic_store_n(&flags[index], 1, __ATOMIC_SEQ_CST);
            // Wait for the event to finish processing
            char ret = 0;
            do {
                std::this_thread::yield();
                ret = __atomic_load_n(&flags[index], __ATOMIC_SEQ_CST);
            } while (ret <= 1);
            if (returnValue) {
                // If returnValue is not NULL, set the return value
                // to the return value of the processing function
                *returnValue = getReturnValue(index);
            }
            __atomic_store_n(&flags[index], 0, __ATOMIC_SEQ_CST);
            return ret;
        }
    }
}

int SynchronousQueue::getEvent(queue_data* event) {
    while (true) {
        // Check if there are still unprocessed events
        int currentHead = __atomic_load_n(head, __ATOMIC_SEQ_CST);
        int currentTail = __atomic_load_n(tail, __ATOMIC_SEQ_CST);
        // Technically currentHead > currentTail is impossible
        // Just to be safe
        if (currentHead >= currentTail) {
            return -1;
        }

        // If the event is not ready yet, don't read it
        // This prevents done to be incremented before flag is set to 1 yet
        int index = currentHead % capacity;
        if (__atomic_load_n(&flags[index], __ATOMIC_SEQ_CST) != 1) {
            std::this_thread::yield();
            continue;
        }

        // If cmpxchg succeeds, return the event
        // If cmpxchg fails, try again
        if (__atomic_compare_exchange_n(head, &currentHead, currentHead + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            int index = currentHead % capacity;
            char ready = __atomic_load_n(&flags[index], __ATOMIC_SEQ_CST);
            while(ready != 1) {
                std::cout << "Error in getEvent!" << std::endl;
                exit(-1);
            }
            *event = data[index];
            return index;
        }
    }
}

void SynchronousQueue::finishEvent(int index, int ret) {
    // Now the flags should be 1
    // If not, something is wrong
    char flag_snapshot = __atomic_load_n(&flags[index], __ATOMIC_SEQ_CST);
    if (flag_snapshot != 1) {
        std::cerr << "Invalid flags in finishEvent: " << flag_snapshot << std::endl;
        exit(-1);
    }
    if (ret == 0 || ret == 1) {
        std::cerr << "Invalid return value" << std::endl;
        exit(-1);
    }
    if (ret != 2) {
        std::cerr << "Warning: return value is not appropriate" << std::endl;
    }
    __atomic_store_n(&flags[index], ret, __ATOMIC_SEQ_CST);
    updateDone();
}

void SynchronousQueue::updateDone() {
    // update done to the next element after all consecutively finished events
    int currentDone = __atomic_load_n(done, __ATOMIC_SEQ_CST);
    int currentHead = __atomic_load_n(head, __ATOMIC_SEQ_CST);
    int newDone = currentDone;
    // Cannot increment done while flag has not been reset
    // by produce() waiting for the result
    while (newDone < currentHead) {
        char ready = __atomic_load_n(&flags[newDone % capacity], __ATOMIC_SEQ_CST);
        if (ready != 0) {
            break;
        }
        newDone++;
    }
    // No matter whether this succeeds or not, it is okay
    // If it fails, it means that someone else has updated done during the time
    __atomic_compare_exchange_n(done, &currentDone, newDone, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
