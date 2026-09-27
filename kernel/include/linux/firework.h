/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FIREWORK_H
#define _LINUX_FIREWORK_H

#include <linux/printk.h>

extern uint8_t firework_hint_zone_mode;
extern uint64_t firework_fast_tier_size_byte;
extern uint8_t firework_dax_id;
// start and end physical page indices available to be allocated to shared pages
extern uint64_t dax_range_start_page;
extern uint64_t dax_range_end_page;
extern uint64_t dax_range_start_page_2;
extern uint64_t dax_range_end_page_2;

// defined in mm/firework.c, used by page fault handler to determine
// whether the shared page or anonymous page page should be taken
// extern uint32_t firework_managed_process_pid;
extern uint32_t firework_managed_process_pid_0;
extern uint32_t firework_managed_process_pid_1;
extern uint32_t firework_managed_process_pid_2;
extern uint32_t firework_managed_process_pid_3;
extern uint32_t firework_managed_process_pid_4;
extern uint32_t firework_managed_process_pid_5;
extern uint32_t firework_managed_process_pid_6;
extern uint32_t firework_managed_process_pid_7;
extern atomic_t pf_counter;
extern atomic_t mig_counter1;
extern atomic_t mig_counter2;
extern atomic_t unshare_counter;
extern uint8_t migration_batch_size;
extern uint64_t hwc_size;
extern atomic_t tmp1;
extern atomic_t tmp2;
extern uint32_t firework_num_nodes;
extern uint32_t program_end;
extern uint32_t unshare_to_accessor;
extern uint32_t unshare_to_accessor_count;
extern uint32_t scan_needed;
extern uint32_t epoch_unshare;
extern uint32_t epoch_interval_ms;
extern uint32_t epoch_min_pages;
extern uint32_t epoch_unshare_count;
extern uint32_t epoch_round_count;
extern uint32_t eviction_fifo;
extern uint32_t tmp_unshare_batch_size;

extern const uint64_t firework_heap_base;
extern const uint64_t firework_heap_bound;
extern const uint64_t firework_heap_region_size;
extern const uint64_t firework_stack_base;
extern const uint64_t firework_stack_bound;
extern const uint64_t firework_stack_region_size;
extern const uint64_t firework_hint_zone_base;
extern uint64_t firework_hint_zone_bound;
extern uint64_t firework_hint_zone_pfn_base;

// struct QueueData {
//     unsigned long addr;
//     unsigned long pfn;
//     unsigned long num_pages;
// };

enum qkind {
    Q_MIGRATE = 0,
    Q_THD_CREATE = 1,
    Q_THD_JOIN = 2,
    Q_UNMAP = 3,
    Q_UNSHARE = 4,
    Q_UNMAP2 = 5,
    Q_UNSHARE2 = 6,
    Q_SCAN = 7
};

struct queue_data {
    int kind;
    int src_node;
    union {
        struct {
            unsigned long addr;
            // unsigned long pfn;
            unsigned long num_pages;
            int pfns_idx;
        } mig;

        struct {
            void *func;
            void* args;
            int id;
        } thd;

        struct  {
            unsigned long addr;
            int owner;
        } unshare;

        struct {
            int head;
            int unshare_num;
        } unmap2;
    } payload;
};

// struct ThdInfo {
//     // 0 -> create
//     // 1 -> join
//     // 2 -> unmap
//     // 3 -> unshare
//     int request_type;
//     union {
//         struct {
//             void *func;
//             void *args;
//         } create;
//         struct {
//             uint64_t addr;
//         } unshare;
//         struct {
//             uint64_t addr;
//         } unmap;
//     } payload;
//     remote_tid_t id;
// };

struct SyncronousQueue {
    struct queue_data *data;
    uint8_t* flags;
    int* head;
    int* tail;
    int* done;
    size_t capacity;
};

bool is_firework_process(pid_t pid);
bool is_shared_page_pfn(unsigned long pfn);
bool is_shared_pool_region_pfn(unsigned long pfn);
bool is_shared_page(pid_t pid, unsigned long vaddr);

int shared_mem_init(size_t sharing_map_size, size_t queue_capacity);
unsigned long get_shared_page_pfn(int num_pages);
bool get_hint_zone_pfn(unsigned long addr, unsigned long *pfn);
int shared_mem_put(int host_idx, uint64_t addr, unsigned long pfn);
unsigned long shared_mem_get(int host_idx, uint64_t addr);
unsigned long shared_mem_get_or_create(uint64_t addr, int* status, uint8_t *num_pages, uint32_t *recheck_hash_idx, int *owner_node, int *pfns_idx);
unsigned long sharing_map_recheck(int *status, uint32_t hash_idx, unsigned long addr);
void shared_mem_exit(void);
int get_process_idx(pid_t pid);
int specify_process_idx(unsigned long addr);
int firework_network_init(int fw_server_idx, int fw_server_size);
uint32_t find_sharing_map_idx(unsigned long addr);
unsigned int check_sharing_map_sync(uint32_t idx);
void clear_sharing_map_sync(uint32_t idx);
void set_sharing_map_failed(uint32_t idx);
void sync_queue_init(size_t queue_capacity);
int sync_queue_produce(unsigned long addr, unsigned long pfn, unsigned long num_pages, int queue_idx, int pfns_idx);
int sync_queue_produce_async(unsigned long addr, unsigned long num_pages, int queue_idx, int pfns_idx);
int sync_queue_wait(int queue_idx, int index);
int sync_queue_poll(int queue_idx, int index);
void sync_queue_finish(int queue_idx, int index);
void sharing_map_fail_to_dead(uint32_t idx, int requester);
int sync_queue_get_event(unsigned long *addr, unsigned long *pfn);
void sync_queue_update_done(struct SyncronousQueue *queue);
int sync_queue_finish_event(struct SyncronousQueue *queue, int index, uint8_t ret);
// int unsharing_unmap(const void __user *addr);
int unmap_queue_produce(unsigned long addr);
// int unshare_queue_produce(unsigned long addr);
// unsigned long get_limited_shared_page_pfn(unsigned long addr, int num_pages);
unsigned long get_limited_shared_page_pfn(int num_pages);
int sharing_map_entry_reset(uint32_t idx);
// int unshare_if_needed(unsigned long addr);
int set_status_flag(uint64_t addr, int mode, int idx);
void save_addr_buffer(unsigned long addr);
int monitor_hwc(void);
void finish_unshare(void);
int check_ongoing_unshare(void);
int unmap_queue_check(void);
unsigned long find_fast_path(uint64_t addr);
int do_unmap(int head, int unshare_num, int* buf, int option);
int unshare_send(unsigned long addr);
void add_done_unmap(int num);
void clear_done_unmap(void);
int check_done_unmap(void);
void scan_shared_pages(void);
void shared_page_count(void);
void pick_eviction_pages(int num);
void analyze(void);
int firework_check_pte(unsigned long addr, unsigned long pfn);
void rmap_list_update(unsigned long addr, unsigned long pfn);
void rmap_list_update2(unsigned long addr, unsigned long pfn, int src_node);
void find_shareres(unsigned long addr);
int scan_request(void);
void shared_page_count_wrapper(void);
int pop_fifo(int num);
void enable_unsharing(void);
int active_pick_eviction_pages(int num);
void do_unsharing_all(void);
void free_pfn(unsigned long pfn);
int firework_check_pfn(unsigned long addr, unsigned long pfn);
int check_status_flag(unsigned long addr);
unsigned long check_pfn_mapcount(unsigned long addr);
unsigned long check_rmap_addr(unsigned long pfn);

#endif // _LINUX_FIREWORK_H