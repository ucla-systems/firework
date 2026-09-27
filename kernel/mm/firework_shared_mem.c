/*
 * mm/firework_shared_mem.c - manage the shared memory
 */
#include <linux/firework.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/io.h>
#include <linux/hash.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/tcp.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/wait.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/limits.h>
#include <linux/types.h>
#include <linux/page-flags.h>
#include <linux/mmu_notifier.h>
#include <linux/pgtable.h>

// used by physical page allocator
// the offset from
// the first available PFN to be used by shared pages
static unsigned long shared_page_start;
// the number of PFNs for shared pages
static unsigned long shared_page_capacity;
// the offset from shared_page_start of the next available PFN
static atomic_t shared_page_offset;
static atomic_t addr_buffer_idx;
// static atomic64_t shared_page_position;
static atomic_t shared_page_head;
static atomic_t shared_page_tail;
static atomic_t available_queue_head;
static atomic_t available_queue_tail;
/* next ring slot to reserve for a return; tail is only advanced (in order)
 * after the pfns have been written, see pool_return_pfns() */
static atomic_t available_queue_reserve;
static atomic_t unmap_done;
static unsigned long sharing_map_entry_capacity;
// const uint64_t hwc_size = 0x200000; // 2MB, minimum size
// const uint64_t hwc_size = 0x20000000; // 512MB
// const uint64_t pfn_range = hwc_size >> PAGE_SHIFT;
#define pfn_range (hwc_size >> PAGE_SHIFT)
// static atomic_t hwc_head;
static atomic_t unshare_count;
static atomic_t index_fifo;

wait_queue_head_t shared_page_wq;
wait_queue_head_t scan_wq;

// For testing
//#define SHARING_MAP_DEBUG

#ifdef SHARING_MAP_DEBUG
#define SHARING_MAP_SIZE (96) 
#define SHARING_MAP_ENTRY_SIZE (24)
#define HASH_SEARCH_LIMIT 2
#else
// 1GB = 256K pages
// Assume no more than 64GB of shared pages
// The number of used entries tops at 8M
// To reduce collision, we give it total space of 1.5GB
// With a total of 1.5*1024/24 = 64M entries
#define SHARING_MAP_SIZE ((1024+512)*1024*1024)
// size of each hash table entry: 24 bytes
// virtual page index: 8 bytes
// physical page PFN: 8 bytes
// shared host bitmap: 4 bytes
// synchronization flags: 4 bytes
#define SHARING_MAP_ENTRY_SIZE (24)
#define HASH_SEARCH_LIMIT 32
#endif


#define FIREWORK_HASH_BITS 32

#define PINNED 0x18000000 // 384MB
// #define PINNED 0x1B333000 

// When PIE is disabled, the heap is on lower addresses
// Make sure the addresses here align with those in aligned_start_addr_goal
// in malloc.c, glibc
const uint64_t firework_heap_base = 0x1000000000; // 64GB
const uint64_t firework_heap_bound = 0x101000000000; // 16TB + 64GB
const uint64_t firework_heap_region_size = 0x10000000000; // 1TB
const uint64_t firework_stack_base = 0x600000000000;
const uint64_t firework_stack_bound = 0x602000000000;
const uint64_t firework_stack_region_size = 0x400000000; // 16GB
//const uint64_t firework_stack_size = 0x1000000; // 16MB
const uint64_t firework_hint_zone_base = 0x200000000000; // 32TB
uint64_t firework_hint_zone_bound = firework_hint_zone_base;
uint64_t firework_hint_zone_pfn_base = 0x0;
uint64_t firework_hint_zone_pfn_base_2 = 0x0;
uint64_t firework_hint_zone_pfn_bound_2 = 0x0;

struct sharing_map_entry {
    unsigned long virtual_page_index;
    unsigned long pfn;
    unsigned int sharing_bitmap;
    // 0: private-ready
    // 1: migration in progress
    // 2: migration failed, need retry
    // 3: shared-ready
    // 4: unmap in progress
    // 5: unshare in progress
    unsigned int status;
};

/*
 * Open-addressing rules for the sharing map (probe = hash+1, +2, ... up to
 * HASH_SEARCH_LIMIT slots):
 *   EMPTY  vpi == 0            never used; ends every probe chain
 *   TOMB   vpi == TOMBSTONE    entry fully torn down (migration rolled back /
 *                              untouched prefetch page); probes continue,
 *                              inserts may reuse it
 *   DEAD   pfn == 0, vpi set   page unshared back to a private copy; probes
 *                              continue (its bitmap still says who holds the
 *                              page), a fault on the same vpi re-arms it
 * A lookup must only stop at EMPTY: stopping at any pfn == 0 slot (the old
 * rule) lost entries that sat behind an unshared page in the same chain,
 * which left their status at "migration in progress" forever.
 */
#define SHARING_MAP_TOMBSTONE (~0UL)
static inline bool sharing_map_slot_free(const struct sharing_map_entry *e)
{
    return e->virtual_page_index == 0 || e->virtual_page_index == SHARING_MAP_TOMBSTONE;
}

struct sharing_map_entry* sharing_map = NULL;
spinlock_t hwc_lock;
spinlock_t *sharing_map_lock = NULL;

struct shared_page_info {
    unsigned long pfn;
    int yellow;
    int access_bitmap;
    struct list_head list;
};

struct page_access_info {
    atomic_t count;
    atomic_t accessors;
};

struct unshare_entry {
    uint64_t addr;
    int owner;
    atomic_t sharer_bitmap;
};

#define MAX_MIG_PREFETCH_SIZE 128
#define MIG_PREFETCH_ENTRY_NUM 256
struct mig_prefetch_entry {
    unsigned long pfns[MAX_MIG_PREFETCH_SIZE];
};

struct list_head active_pages;
struct list_head inactive_pages;

int need_active_unshare = 0;

#define SH_QUEUE_SIZE 0x10000 // 64KB
#define SH_QUEUE_SORT 4
#define NODE_NUM 8
#define SH_QUEUE_NUM (SH_QUEUE_SORT * NODE_NUM) // 8 for mig, 8 for thd, 8 for unshare, 8 for scan
#define SH_QUEUE_ENTRY_SIZE 0x20 // 32 bytes
#define SH_QUEUE_ENTRY_NUM 2000
#define SH_QUEUE_HEADER_SIZE 64
#define MIG_QUEUE_NUM (SH_QUEUE_NUM / SH_QUEUE_SORT)
#define THD_QUEUE_NUM (SH_QUEUE_NUM / SH_QUEUE_SORT)
#define UNSHARE_QUEUE_NUM (SH_QUEUE_NUM / SH_QUEUE_SORT)
#define SCAN_QUEUE_NUM (SH_QUEUE_NUM / SH_QUEUE_SORT)

#define UNSHARE_BATCH_SIZE 1024 // max: 1024 entries
#define MAX_LIMITED_MEMORY_SIZE SZ_4G
#define MAX_LIMITED_MEMORY_PAGE_NUM (MAX_LIMITED_MEMORY_SIZE >> PAGE_SHIFT)

// local CXL device
#define SH_QUEUES_OFFSET_BYTES 0UL
#define SH_QUEUES_SIZE_BYTES (SH_QUEUE_SIZE*SH_QUEUE_NUM)
#define SH_LOCK_OFFSET_BYTES (SH_QUEUES_OFFSET_BYTES+SH_QUEUES_SIZE_BYTES)
#define SH_LOCK_SIZE_BYTES SZ_4G
#define SH_HINT_ZONE_OFFSET_BYTES (SH_LOCK_OFFSET_BYTES+SH_LOCK_SIZE_BYTES)
#define SH_HINT_ZONE_SIZE_BYTES SZ_16G
// #define SH_FIFO_SIZE_BYTES (pfn_range*sizeof(uint64_t));
// #define SH_FIFO_OFFSET_BYTES (SH_SHARED_ZONE_OFFSET_BYTES+SH_FIFO_SIZE_BYTES)
#define SH_FIFO_OFFSET_BYTES (SH_HINT_ZONE_OFFSET_BYTES+SH_HINT_ZONE_SIZE_BYTES)
#define SH_FIFO_SIZE_BYTES (pfn_range*sizeof(uint64_t))
#define SH_UNSHARE_LIST_OFFSET_BYTES (SH_FIFO_OFFSET_BYTES+SH_FIFO_SIZE_BYTES)
#define SH_UNSHARE_LIST_SIZE_BYTES (NODE_NUM*UNSHARE_BATCH_SIZE*sizeof(struct unshare_entry))
#define SH_IO_BUFFER_OFFSET_BYTES (SH_UNSHARE_LIST_OFFSET_BYTES+SH_UNSHARE_LIST_SIZE_BYTES)
#define SH_IO_BUFFER_SIZE_BYTES (PAGE_SIZE*128)
#define SH_MIG_BUFFER_OFFSET_BYTES (SH_IO_BUFFER_OFFSET_BYTES+SH_IO_BUFFER_SIZE_BYTES)
#define SH_MIG_BUFFER_SIZE_BYTES (MIG_PREFETCH_ENTRY_NUM*sizeof(struct mig_prefetch_entry))
#define SH_AVAILABLE_REGION_QUEUE_OFFSET_BYTES (SH_MIG_BUFFER_OFFSET_BYTES+SH_MIG_BUFFER_SIZE_BYTES)
#define SH_AVAILABLE_REGION_QUEUE_SIZE_BYTES (MAX_LIMITED_MEMORY_PAGE_NUM*sizeof(int))
#define SH_PG_LIST_REGION_OFFSET_BYTES (SH_AVAILABLE_REGION_QUEUE_OFFSET_BYTES+SH_AVAILABLE_REGION_QUEUE_SIZE_BYTES)
#define SH_PG_LIST_REGION_SIZE_BYTES (pfn_range*(sizeof(struct shared_page_info)))
#define SH_RMAP_LIST_OFFSET_BYTES (SH_PG_LIST_REGION_OFFSET_BYTES+SH_PG_LIST_REGION_SIZE_BYTES)
#define SH_RMAP_LIST_SIZE_BYTES ((NODE_NUM+1)*pfn_range*sizeof(atomic64_t))
#define SH_PG_COUNT_REGION_OFFSET_BYTES (SH_RMAP_LIST_OFFSET_BYTES+SH_RMAP_LIST_SIZE_BYTES)
#define SH_PG_COUNT_REGION_SIZE_BYTES (pfn_range*sizeof(struct page_access_info))
// #define SH_IO_BUFFER_OFFSET_BYTES (SH_PG_COUNT_REGION_OFFSET_BYTES+SH_PG_COUNT_REGION_SIZE_BYTES)
// #define SH_IO_BUFFER_SIZE_BYTES PAGE_SIZE

#define SH_SHARED_ZONE_OFFSET_BYTES (SH_PG_COUNT_REGION_OFFSET_BYTES+SH_PG_COUNT_REGION_SIZE_BYTES)
// #define SH_SHARED_ZONE_OFFSET_BYTES (SH_HINT_ZONE_OFFSET_BYTES+SH_HINT_ZONE_SIZE_BYTES)

// remote CXL device
#define SH_HINT_ZONE_OFFSET_BYTES_DAX1 0UL
// #define SH_HINT_ZONE_SIZE_BYTES_DAX1 SZ_32G
// #define SH_HINT_ZONE_SIZE_BYTES_DAX1 0xE00000000ULL // 56GB
// #define SH_HINT_ZONE_SIZE_BYTES_DAX1 0x1600000000ULL // 88GB
#define SH_HINT_ZONE_SIZE_BYTES_DAX1 0x2400000000ULL // 144GB

void* sh_queues_start = NULL;
uint64_t* addr_buffer = NULL;

struct SyncronousQueue mig_queues[MIG_QUEUE_NUM];
struct SyncronousQueue thd_queues[THD_QUEUE_NUM];
struct SyncronousQueue unshare_queues[UNSHARE_QUEUE_NUM];
struct SyncronousQueue scan_queues[SCAN_QUEUE_NUM];

struct unshare_entry* unshare_lists = NULL;
struct unshare_entry* unshare_list[NODE_NUM] = {NULL};

int* available_region_queue = NULL;
struct shared_page_info* shared_page_data_list = NULL;

// atomic64_t* rmap_list = NULL;
atomic64_t* rmap_lists = NULL;
atomic64_t* rmap_list[NODE_NUM+1] = {NULL};
// atomic_t* pg_access_count = NULL;
struct page_access_info* pg_access_info_list = NULL;

unsigned long* pfn_buffer = NULL;
unsigned long* pfn_buffer2 = NULL;
/* accessor bitmap of each pool page in the previous epoch (epoch_unshare) */
uint8_t* epoch_prev_accessors = NULL;
size_t pbidx = 0;

int start_flag = 0;

struct mig_prefetch_entry* mig_prefetch_pfns = NULL;
atomic_t mig_prefetch_pfns_idx;
//#define SHARING_MEASURE

#ifdef SHARING_MEASURE
#define PERF_ARRAY_SIZE 2000000
static int create_array[PERF_ARRAY_SIZE];
static int get_array[PERF_ARRAY_SIZE];
static struct dentry *dir;
static struct debugfs_blob_wrapper create_blob;
static struct debugfs_blob_wrapper get_blob;
#endif

#define FW_SERVER_MAX 8
struct socket *global_sockets[FW_SERVER_MAX][FW_SERVER_MAX];

static_assert(sizeof(struct sharing_map_entry) == SHARING_MAP_ENTRY_SIZE);

bool is_firework_process(pid_t pid) {
    //return firework_enabled && pid == firework_managed_process_pid;
    return pid == firework_managed_process_pid_0 || pid == firework_managed_process_pid_1 || pid == firework_managed_process_pid_2 || pid == firework_managed_process_pid_3 || pid == firework_managed_process_pid_4 || pid == firework_managed_process_pid_5 || pid == firework_managed_process_pid_6 || pid == firework_managed_process_pid_7;
}

bool is_shared_page_pfn(unsigned long pfn) {
    return pfn >= shared_page_start && pfn < shared_page_start + shared_page_capacity;
}

/*
 * The pool start moves with hwc_size (the per-pfn tables in front of it are
 * sized by pfn_range), so a page handed out by the previous run can lie
 * below the current shared_page_start when a rank from that run is still
 * exiting while the next run re-initialises the layout. Every layout's pool
 * lies above the fixed regions (queues, locks, hint zone) and below the
 * sharing map; the tables in between are only ioremapped, never mapped to
 * user space, so any user-mapped device page in this window is a pool page
 * of some layout (M12 in CRASH_CANDIDATES.md).
 */
static unsigned long pool_region_start; /* first pfn above the hint zone */
static unsigned long pool_region_end;   /* sharing_map_start */

bool is_shared_pool_region_pfn(unsigned long pfn) {
    return pfn >= pool_region_start && pfn < pool_region_end;
}

/*
 * For a fault on a page of our own partition: does the PSM say the page
 * currently lives anywhere but in our private memory? True while it is
 * shared / in flight / being unshared, or once it was unshared INTO another
 * process (DEAD entry whose bitmap names someone else). False for no entry
 * and for a DEAD entry that names only us: then we hold (or held) the
 * private copy, and a missing PTE is an ordinary first touch, e.g. after
 * glibc's MADV_DONTNEED on a cached thread stack. Routing that case to
 * do_shared_page asked our own daemon to migrate the page to us, which
 * failed and left the entry "in progress" forever.
 */
static bool sharing_map_page_elsewhere(unsigned long addr, int self)
{
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry *entry;
    bool elsewhere;

    for (int i = 0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        spin_lock(&sharing_map_lock[hash_idx]);
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi) {
            elsewhere = entry->status != 0 ||
                        (entry->sharing_bitmap & ~(1u << self)) != 0;
            spin_unlock(&sharing_map_lock[hash_idx]);
            return elsewhere;
        }
        if (entry->virtual_page_index == 0) {
            spin_unlock(&sharing_map_lock[hash_idx]);
            return false;
        }
        spin_unlock(&sharing_map_lock[hash_idx]);
    }
    return false;
}

bool is_shared_page(pid_t pid, unsigned long vaddr) {
    uint64_t heap_start, heap_end, stack_start, stack_end;
    int process_idx;

    if (!is_firework_process(pid)) {
        return false;
    }

    process_idx = get_process_idx(pid);
    if (process_idx < 0) {
        return false;
    }

    // Is the page from hint zone
    if (vaddr >= firework_hint_zone_base && vaddr < firework_hint_zone_bound) {
        atomic_inc(&mig_counter2);
        return true;
    }

    // Is the page from its own heap / stack partition
    heap_start = firework_heap_base + process_idx * firework_heap_region_size;
    heap_end = heap_start + firework_heap_region_size;
    stack_start = firework_stack_base + process_idx * firework_stack_region_size;
    stack_end = stack_start + firework_stack_region_size;
    if ((vaddr >= heap_start && vaddr < heap_end) ||
        (vaddr >= stack_start && vaddr < stack_end)) {
        /*
         * Normally a page in our own partition is either still private to
         * us or lives in shared memory with our PTE already pointing at it,
         * so a fault here is a genuine first touch: plain anonymous fault.
         *
         * With unshare_to_accessor, however, a page of ours can be unshared
         * INTO ANOTHER process (its recent accessor) and we are unmapped as
         * a non-provider. A later fault on that address must then consult
         * the PSM (which records the current holder in sharing_bitmap) and
         * go through do_shared_page; taking do_anonymous_page instead would
         * hand us a fresh zero page while the accessor still holds the
         * real data - two private copies of one address, and the
         * inconsistency that crashed the earlier attempt at this policy.
         * Only an existing PSM entry means the page has ever left us; with
         * no entry it is a first touch as before.
         */
        if (unshare_to_accessor && sharing_map_page_elsewhere(vaddr, process_idx))
            return true;
        return false;
    }

    // If the page is outside of our managed virtual address range (heap or stack)
    if (vaddr < firework_heap_base || (vaddr >= firework_heap_bound && vaddr < firework_stack_base) || vaddr >= firework_stack_bound) {
        return false;
    }
    
    return true;
}

void unmap_sharing_map(void) {
    if (sharing_map) {
        iounmap(sharing_map);
        sharing_map = NULL;
    }
}

void unmap_sh_queues(void) {
    if (sh_queues_start) {
        iounmap(sh_queues_start);
        sh_queues_start = NULL;
    }
}

// System call to setup the shared memory device
// This should only be called once
// On success, return 0
// When the sharing map size is invalid, return -EINVAL
// When io mapping fails, return -ENOMEM
int shared_mem_init(size_t sharing_map_size, size_t queue_capacity) {
    unsigned long sharing_map_size_in_pages;
    unsigned long sharing_map_start;

    atomic_set(&pf_counter, 0);
    atomic_set(&mig_counter1, 0);
    atomic_set(&mig_counter2, 0);
    atomic_set(&unshare_counter, 0);
    atomic_set(&index_fifo, 0);

    // We assume all processes run on a single node
    // Therefore, this should only be called once from the main process
    if (get_process_idx(current->tgid) != 0) {
        return 0;
    }

    pr_info("shared_mem_init by main process: %ld\n", sharing_map_size);
    pr_info("firework_num_nodes: %d\n", firework_num_nodes);

    if (queue_capacity > SH_QUEUE_ENTRY_NUM) {
        pr_warn("Queue exceeding max capacity: %ld > %d\n", queue_capacity, SH_QUEUE_ENTRY_NUM);
        return -EINVAL;
    }

    if (sharing_map_size == 0)
        sharing_map_size = SHARING_MAP_SIZE;

    if (sharing_map_size > SHARING_MAP_SIZE) {
        pr_warn("The sharing map size is too large: %ld\n", sharing_map_size);
        return -EINVAL;
    }

    // sharing map should be page-aligned
    // so the sharing pages space is also page-aligned
    if (sharing_map_size % PAGE_SIZE != 0) {
        pr_warn("The sharing map size is not page-aligned: %ld\n", sharing_map_size);
        return -EINVAL;
    }

    sharing_map_size_in_pages = sharing_map_size >> PAGE_SHIFT;
    sharing_map_entry_capacity = sharing_map_size / SHARING_MAP_ENTRY_SIZE;

    if (SH_QUEUES_SIZE_BYTES % PAGE_SIZE) {
        pr_err("The queue space is not page-aligned: %x\n", SH_QUEUES_SIZE_BYTES);
        return -EINVAL;
    }

    firework_hint_zone_pfn_base = dax_range_start_page + (SH_HINT_ZONE_OFFSET_BYTES>>PAGE_SHIFT);

    switch(firework_hint_zone_mode) {
        case 0: // plain mode
            firework_hint_zone_bound = firework_hint_zone_base + SH_HINT_ZONE_SIZE_BYTES;
            break;
        case 1: // interleaved mode
            if (SH_HINT_ZONE_SIZE_BYTES_DAX1 < SH_HINT_ZONE_SIZE_BYTES) {
                pr_err("Size of dax 1 hint zone is too small: %llx < %llx\n", SH_HINT_ZONE_SIZE_BYTES_DAX1, SH_HINT_ZONE_SIZE_BYTES);
            }
            firework_hint_zone_pfn_base_2 = dax_range_start_page_2 + (SH_HINT_ZONE_OFFSET_BYTES_DAX1>>PAGE_SHIFT);
            firework_hint_zone_pfn_bound_2 = firework_hint_zone_pfn_base_2 + (SH_HINT_ZONE_SIZE_BYTES>>PAGE_SHIFT);
            if (firework_hint_zone_pfn_bound_2 > dax_range_end_page_2) {
                pr_err("The hint zone is too large: %llx > %llx\n", firework_hint_zone_pfn_bound_2, dax_range_end_page_2);
                return -EINVAL;
            }
            firework_hint_zone_bound = firework_hint_zone_base + (SH_HINT_ZONE_SIZE_BYTES*2);
            break;
        case 2: // tiered mode
            firework_hint_zone_pfn_base_2 = dax_range_start_page_2 + (SH_HINT_ZONE_OFFSET_BYTES_DAX1>>PAGE_SHIFT);
            firework_hint_zone_pfn_bound_2 = firework_hint_zone_pfn_base_2 + (SH_HINT_ZONE_SIZE_BYTES_DAX1>>PAGE_SHIFT);
            firework_hint_zone_bound = firework_hint_zone_base + firework_fast_tier_size_byte + SH_HINT_ZONE_SIZE_BYTES_DAX1;
            break;
        default:
            pr_err("Invalid hint zone mode: %d\n", firework_hint_zone_mode);
            return -EINVAL;
    }
    pr_info("firework_hint_zone_mode: %d\n", firework_hint_zone_mode);
    pr_info("hint zone size: %lldGB\n", (firework_hint_zone_bound - firework_hint_zone_base) >> 30);

    // Set up the physical page allocator
    sharing_map_start = dax_range_end_page - sharing_map_size_in_pages + 1;
    shared_page_start = dax_range_start_page + (SH_SHARED_ZONE_OFFSET_BYTES>>PAGE_SHIFT);
    shared_page_capacity = sharing_map_start - shared_page_start;
    pool_region_start = dax_range_start_page + (SH_FIFO_OFFSET_BYTES>>PAGE_SHIFT);
    pool_region_end = sharing_map_start;
    atomic_set(&addr_buffer_idx, 0);
    // atomic64_set(&shared_page_position, 0);
    atomic_set(&shared_page_head, 0);
    atomic_set(&shared_page_tail, 0);
    atomic_set(&unshare_count, 0);
    atomic_set(&unmap_done, 0);
    init_waitqueue_head(&shared_page_wq);
    init_waitqueue_head(&scan_wq);

    pr_info("dax_range_start_page: %llx, dax_range_end_page: %llx\n", dax_range_start_page, dax_range_end_page);
    if (shared_page_start >= sharing_map_start) {
        pr_err("Dax device is too small! shared zone start: %lx; sharing map start: %lx\n", shared_page_start, sharing_map_start);
    } else if (shared_page_capacity < (SH_HINT_ZONE_SIZE_BYTES>>PAGE_SHIFT)) {
        pr_warn("Size of shared zone is smaller than hint zone. Consider increasing shared memory size\n");
    }
    pr_info("shared_page_start: %lx, shared_page_end: %lx\n", shared_page_start, sharing_map_start);
    pr_info("hwc_size: 0x%llx, pfn_range: 0x%llx\n", hwc_size, pfn_range);
    /*
     * available_region_queue (and the other per-pfn tables in the dax
     * layout) are sized for MAX_LIMITED_MEMORY_PAGE_NUM entries. A larger
     * hwc_size used to overflow them from this very init loop (oops in
     * shared_mem_init). Refuse it up front instead.
     */
    if (pfn_range > MAX_LIMITED_MEMORY_PAGE_NUM) {
        pr_err("hwc_size 0x%llx exceeds the supported maximum 0x%lx (%lu MB); refusing to initialize\n",
               hwc_size, (unsigned long)MAX_LIMITED_MEMORY_SIZE, (unsigned long)(MAX_LIMITED_MEMORY_SIZE >> 20));
        return -EINVAL;
    }

    // In case the sharing map was not properly freed in the previous run
    unmap_sharing_map();
    // Set up the sharing map
    sharing_map = (struct sharing_map_entry*) ioremap_cache((sharing_map_start) << PAGE_SHIFT, sharing_map_size);
    if (!sharing_map) {
        pr_warn("Failed to map the sharing map\n");
        return -ENOMEM;
    }
    memset(sharing_map, 0, sharing_map_size);

    /* re-initialised on every program start: free the previous array or
     * 256 MB leak per run (86 GB after a day of sweeps) */
    if (sharing_map_lock)
        vfree(sharing_map_lock);
    sharing_map_lock = vmalloc(sizeof(spinlock_t) * sharing_map_entry_capacity);
    if (!sharing_map_lock) {
        pr_warn("Failed to allocate memory for sharing map lock\n");
        return -ENOMEM;
    }
    for (unsigned long i = 0; i < sharing_map_entry_capacity; i++) {
        spin_lock_init(&sharing_map_lock[i]);
    }
    pr_info("sharing_map_lock: %p\n", sharing_map_lock);
    
    spin_lock_init(&hwc_lock);

    unmap_sh_queues();
    sh_queues_start = ioremap_cache((dax_range_start_page) << PAGE_SHIFT, SH_QUEUES_SIZE_BYTES);
    if (!sh_queues_start) {
        pr_warn("Failed to map the shared queues\n");
        return -ENOMEM;
    }

    memset(sh_queues_start, 0, SH_QUEUES_SIZE_BYTES);

    if (addr_buffer) {
        iounmap(addr_buffer);
        addr_buffer = NULL;
    }
    addr_buffer = (uint64_t*) ioremap_cache((dax_range_start_page + (SH_FIFO_OFFSET_BYTES>>PAGE_SHIFT)) << PAGE_SHIFT, SH_FIFO_SIZE_BYTES);
    if (!addr_buffer) {
        pr_warn("Failed to map the address buffer\n");
        return -ENOMEM;
    }
    memset(addr_buffer, 0, SH_FIFO_SIZE_BYTES);

    if (unshare_lists) {
        iounmap(unshare_lists);
        unshare_lists = NULL;
    }
    unshare_lists = (struct unshare_entry*) ioremap_cache((dax_range_start_page + (SH_UNSHARE_LIST_OFFSET_BYTES>>PAGE_SHIFT)) << PAGE_SHIFT, SH_UNSHARE_LIST_SIZE_BYTES);
    if (!unshare_lists) {
        pr_warn("Failed to map the unshare lists\n");
        return -ENOMEM;
    }
    memset(unshare_lists, 0, SH_UNSHARE_LIST_SIZE_BYTES);
    for (int i = 0; i < NODE_NUM; i++) {
        unshare_list[i] = &unshare_lists[i * UNSHARE_BATCH_SIZE];
    }

    if (available_region_queue) {
        iounmap(available_region_queue);
        available_region_queue = NULL;
    }
    available_region_queue = (int*) ioremap_cache((dax_range_start_page + (SH_AVAILABLE_REGION_QUEUE_OFFSET_BYTES>>PAGE_SHIFT)) << PAGE_SHIFT, SH_AVAILABLE_REGION_QUEUE_SIZE_BYTES);
    if (!available_region_queue) {
        pr_warn("Failed to map the available region queue\n");
        return -ENOMEM;
    }
    memset(available_region_queue, 0, SH_AVAILABLE_REGION_QUEUE_SIZE_BYTES);
    for (int i=0; i<pfn_range; i++) {
        available_region_queue[i] = shared_page_start + i;
    }
    atomic_set(&available_queue_head, 0);
    atomic_set(&available_queue_tail, pfn_range);
    atomic_set(&available_queue_reserve, pfn_range);

    INIT_LIST_HEAD(&active_pages);
    INIT_LIST_HEAD(&inactive_pages);
    if (shared_page_data_list) {
        iounmap(shared_page_data_list);
        shared_page_data_list = NULL;
    }
    shared_page_data_list = (struct shared_page_info*) ioremap_cache((dax_range_start_page + (SH_PG_LIST_REGION_OFFSET_BYTES>>PAGE_SHIFT)) << PAGE_SHIFT, SH_PG_LIST_REGION_SIZE_BYTES);
    if (!shared_page_data_list) {
        pr_warn("Failed to map the shared page data list\n");
        return -ENOMEM;
    }
    memset(shared_page_data_list, 0, SH_PG_LIST_REGION_SIZE_BYTES);
    for (unsigned long i=0; i<pfn_range; i++) {
        shared_page_data_list[i].pfn = shared_page_start + i;
        shared_page_data_list[i].yellow = 0;
        shared_page_data_list[i].access_bitmap = 0;
        INIT_LIST_HEAD(&shared_page_data_list[i].list);
    }

    if (mig_prefetch_pfns) {
        iounmap(mig_prefetch_pfns);
        mig_prefetch_pfns = NULL;
    }
    mig_prefetch_pfns = (struct mig_prefetch_entry*) ioremap_cache((dax_range_start_page + (SH_MIG_BUFFER_OFFSET_BYTES>>PAGE_SHIFT)) << PAGE_SHIFT, SH_MIG_BUFFER_SIZE_BYTES);
    if (!mig_prefetch_pfns) {
        pr_warn("Failed to map the migration batch PFN buffer\n");
        return -ENOMEM;
    }
    memset(mig_prefetch_pfns, 0, SH_MIG_BUFFER_SIZE_BYTES);
    atomic_set(&mig_prefetch_pfns_idx, 0);

    if (rmap_lists) {
        iounmap(rmap_lists);
        rmap_lists = NULL;
    }
    rmap_lists = (atomic64_t*) ioremap_cache((dax_range_start_page + (SH_RMAP_LIST_OFFSET_BYTES>>PAGE_SHIFT)) << PAGE_SHIFT, SH_RMAP_LIST_SIZE_BYTES);
    if (!rmap_lists) {
        pr_warn("Failed to map the rmap lists\n");
        return -ENOMEM;
    }
    for (int n=0; n<NODE_NUM+1; n++) {
        rmap_list[n] = &rmap_lists[n * pfn_range];
        for (unsigned long i=0; i<pfn_range; i++) {
            atomic64_set(&rmap_list[n][i], 0);
        }
    }
    // for (unsigned long i=0; i<pfn_range; i++) {
    //     atomic64_set(&rmap_list[i], 0);
    // }

    // if (pg_access_count) {
    //     iounmap(pg_access_count);
    //     pg_access_count = NULL;
    // }
    // pg_access_count = (atomic_t*) ioremap_cache((dax_range_start_page + (SH_PG_COUNT_REGION_OFFSET_BYTES>>PAGE_SHIFT)) << PAGE_SHIFT, SH_PG_COUNT_REGION_SIZE_BYTES);
    // if (!pg_access_count) {
    //     pr_warn("Failed to map the page access count array\n");
    //     return -ENOMEM;
    // }
    // pr_info("pg_access_count: %px\n", pg_access_count);
    // for (unsigned long i=0; i<pfn_range; i++) {
    //     atomic_set(&pg_access_count[i], 0);
    // }

    if (pg_access_info_list) {
        iounmap(pg_access_info_list);
        pg_access_info_list = NULL;
    }
    pg_access_info_list = (struct page_access_info*) ioremap_cache((dax_range_start_page + (SH_PG_COUNT_REGION_OFFSET_BYTES>>PAGE_SHIFT)) << PAGE_SHIFT, SH_PG_COUNT_REGION_SIZE_BYTES);
    if (!pg_access_info_list) {
        pr_warn("Failed to map the page access info list\n");
        return -ENOMEM;
    }
    for (unsigned long i=0; i<pfn_range; i++) {
        atomic_set(&pg_access_info_list[i].count, 0);
        atomic_set(&pg_access_info_list[i].accessors, 0);
    }


    if (pfn_buffer)
        kvfree(pfn_buffer);
    pfn_buffer = kvmalloc(sizeof(unsigned long) * pfn_range, GFP_KERNEL);
    if (!pfn_buffer) {
        pr_warn("Failed to allocate pfn buffer\n");
        return -ENOMEM;
    }
    memset(pfn_buffer, 0, sizeof(unsigned long) * pfn_range);
    if (pfn_buffer2)
        kvfree(pfn_buffer2);
    pfn_buffer2 = kvmalloc(sizeof(unsigned long) * pfn_range, GFP_KERNEL);
    if (!pfn_buffer2) {
        pr_warn("Failed to allocate pfn buffer2\n");
        return -ENOMEM;
    }
    memset(pfn_buffer2, 0, sizeof(unsigned long) * pfn_range);
    pbidx = 0;
    if (epoch_prev_accessors)
        kvfree(epoch_prev_accessors);
    epoch_prev_accessors = kvzalloc(pfn_range, GFP_KERNEL);
    if (!epoch_prev_accessors) {
        pr_warn("Failed to allocate epoch accessor history\n");
        return -ENOMEM;
    }

    sync_queue_init(queue_capacity);

#ifdef SHARING_MEASURE
    memset(create_array, 0, sizeof(create_array));
    memset(get_array, 0, sizeof(get_array));
#endif

    program_end = 0;
    unshare_to_accessor_count = 0;
    pr_info("Shared memory initialized\n");

    return 0;
}

#ifdef SHARING_MEASURE

static inline int shared_mem_debufs_init(void) {
    dir = debugfs_create_dir("firework_sharing_measure", NULL);
    if (!dir) {
        pr_warn("Failed to create debugfs directory\n");
        return -ENOMEM;
    }

    memset(create_array, 0, sizeof(create_array));
    memset(get_array, 0, sizeof(get_array));

    create_blob.data = create_array;
    create_blob.size = sizeof(create_array);
    get_blob.data = get_array;
    get_blob.size = sizeof(get_array);

    if (!debugfs_create_blob("create", 0666, dir, &create_blob)) {
        pr_warn("Failed to create debugfs blob for the create operation\n");
        return -ENOMEM;
    }
    if (!debugfs_create_blob("get", 0666, dir, &get_blob)) {
        pr_warn("Failed to create debugfs blob for the get operation\n");
        return -ENOMEM;
    }

    pr_info("Debugfs entries created for evaluation\n");
    return 0;
}

int __init shared_mem_perf_init(void) {
    shared_mem_debufs_init();
    return 0;
}
__initcall(shared_mem_perf_init);

#endif

void shared_mem_exit(void) {
    unmap_sharing_map();
}

// On success, return 0
// When the entry already exists, return -EEXIST
// When the entry cannot be inserted because of the threshhold, return -ENOMEM
// When the PFN is invalid, return -EINVAL
int shared_mem_put(int host_idx, uint64_t addr, unsigned long pfn) {
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry* entry;

    if (!is_shared_page_pfn(pfn))
        return -EINVAL;

    // spin_lock(&sharing_map_lock);
    for (int i = 0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi) {
            // spin_unlock(&sharing_map_lock);
            pr_warn("Duplicate virtual page index: 0x%llx\n", addr);
            return -EEXIST;
        } else if (entry->pfn == 0) {
            entry->virtual_page_index = vpi;
            entry->pfn = pfn;
            if (host_idx >= 0) {
                entry->sharing_bitmap |= 1 << host_idx;
            }
            // spin_unlock(&sharing_map_lock);
            return 0;
        }
    }
    // spin_unlock(&sharing_map_lock);
    pr_err("Giving up inserting the entry due to the threshold: 0x%llx\n", addr);
    return -ENOMEM;
}

// Assuming the pfn is more than 1 on success
// On failure, return the pfn
// When the entry does not still exist, return 0
// When the entry cannot be found because of the threshhold, return 1
// When the entry is not ready, return 2
unsigned long shared_mem_get(int host_idx, uint64_t addr) {
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry* entry;
    unsigned long pfn;

    // spin_lock(&sharing_map_lock);

    for (int i = 0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi) {
            if (entry->status != 0) {
                pr_warn("The entry is not ready: 0x%llx\n", addr);
                // spin_unlock(&sharing_map_lock);
                return 2;
            }
            if (host_idx >= 0) {
                entry->sharing_bitmap |= 1 << host_idx;
            }
            pfn = entry->pfn;
            // spin_unlock(&sharing_map_lock);
            return pfn;
        }
        else if (entry->pfn == 0) { // ToDo? : For now, we do not consider the eviction.
            // Still no entry 
            // spin_unlock(&sharing_map_lock);
            return 0;
        }
    }
    // spin_unlock(&sharing_map_lock);
    pr_err("Giving up searching the entry due to the threshold: 0x%llx\n", addr);
    return 1;
}

// Repeatedly look for up to migration_batch_size entries
// And insert entries for all contiguous non-existing entries
// Stop when reaching the migration_batch_size or seeing an existing entry
// Write the number of pages to be migrated in num_pages
// And return the first PFN to use.
// Three cases for status:
// waiting on in-progress entry: status = 0 (wait)
// creating new entry: status = 1 (initiate migration request next)
// entry is ready: status = 2 (ready)
// last migration failed, retry migration: status = 3
// TODO: Shi: Cases where entries may be removed
// TODO: Shi: In that case, reaching an empty entry does not mean that the key is not in the sharing map
unsigned long shared_mem_get_or_create(uint64_t addr, int* status, uint8_t *num_pages, uint32_t *recheck_hash_idx, int* owner_node, int* pfns_idx) {
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash;
    uint32_t hash_idx;
    struct sharing_map_entry* entry;
    unsigned long pfn = 0;
    unsigned long pfn_start = 0;
    int host_idx;
    int page_idx;
    unsigned long *pfn_pointer_array[MAX_MIG_PREFETCH_SIZE] = {NULL};
    /* per batch index: 1 = entry newly created here, 2 = private entry
     * re-armed here (status 0->1, our bit added); used to undo on failure */
    uint8_t claimed[MAX_MIG_PREFETCH_SIZE] = {0};
    uint32_t free_idx = U32_MAX;
    bool matched = false;
    *num_pages = 0;
    host_idx = get_process_idx(current->tgid);

    start_flag = 1;

    if (*num_pages > MAX_MIG_PREFETCH_SIZE) {
        pr_err("%s: Invalid page num: %d\n", __func__, *num_pages);
        *status = -1;
        return 0;
    }

    for (page_idx = 0; page_idx < migration_batch_size; page_idx++) {
reprobe:
        free_idx = U32_MAX;
        matched = false;
        hash = hash_64(vpi, FIREWORK_HASH_BITS);
        hash_idx = hash % sharing_map_entry_capacity;
        for (int i = 0; i < HASH_SEARCH_LIMIT; i++) {
            // TODO: Shi: offseting all entry by one position?
            hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
            spin_lock(&sharing_map_lock[hash_idx]);
            entry = &sharing_map[hash_idx];
            if (entry->virtual_page_index == vpi) {
                // Find existing entry, time to stop batching
                // If this is the first page, check whether to wait depending on the sync flag
                // If not, allocate and set the PFNs
                if (entry->status == 0) {
                    // The page was unshared before, but it's private now
                    if (host_idx >= 0 && (entry->sharing_bitmap & ~(1u << host_idx)) == 0) {
                        /*
                         * The private copy is ours (or nobody's): there is
                         * nothing to fetch from anyone. This is a plain
                         * first touch of the page (e.g. after our own
                         * MADV_DONTNEED); asking our own daemon to migrate
                         * it to us cannot work and used to orphan the entry
                         * at "in progress".
                         */
                        spin_unlock(&sharing_map_lock[hash_idx]);
                        if (page_idx == 0) {
                            *status = -4;
                            return 0;
                        }
                        goto initiate_migration;   /* stop batching before this page */
                    }
                    entry->status = 1;
                    pfn_pointer_array[page_idx] = &(entry->pfn);
                    claimed[page_idx] = 2;
                    WARN(!is_power_of_2(entry->sharing_bitmap), "Multiple owners even though the page should be private: 0x%llx\n", addr);
                    *owner_node = ffs(entry->sharing_bitmap) - 1;
                    if (*owner_node < 0) {
                        pr_err("Invalid owner node\n");
                    }
                    if (host_idx >= 0) {
                        entry->sharing_bitmap |= 1 << host_idx;
                    }
                    matched = true;
                    spin_unlock(&sharing_map_lock[hash_idx]);
                    break;
                }
                if (page_idx == 0) {
                    // This is the first page, no need to set up PFN
                    // or launch migration request
                    if (entry->status == 1) {
                        // Migration in progress, recheck later
                        spin_unlock(&sharing_map_lock[hash_idx]);
                        *recheck_hash_idx = hash_idx;
                        // *status = 0;
                        *status = -2;
                        return pfn;
                    } else if (entry->status == 3) {
                        // Migration done, ready to map
                        if (host_idx >= 0) {
                            entry->sharing_bitmap |= 1 << host_idx;
                        }
                        pfn = entry->pfn;
                        spin_unlock(&sharing_map_lock[hash_idx]);
                        *status = 2;
                        return pfn;
                    } else if (entry->status == 2) {
                        // The last migration fails, should retry
                        entry->status = 1;
                        pfn = entry->pfn;
                        spin_unlock(&sharing_map_lock[hash_idx]);
                        *status = 3;
                        *num_pages = 1;
                        return pfn;
                    } 
                    // else if (entry->status == 0) {
                    //     // The page was unshared before, but it's private now
                    //     entry->status = 1;
                    //     pfn_pointer_array[page_idx] = &(entry->pfn);
                    //     WARN(!is_power_of_2(entry->sharing_bitmap), "Multiple owners even though the page should be private: 0x%llx\n", addr);
                    //     *owner_node = ffs(entry->sharing_bitmap) - 1;
                    //     if (*owner_node < 0) {
                    //         pr_err("Invalid owner node\n");
                    //     }
                    //     if (host_idx >= 0) {
                    //         entry->sharing_bitmap |= 1 << host_idx;
                    //     }
                    //     // No bataching for now
                    //     page_idx = 1;
                    //     spin_unlock(&sharing_map_lock[hash_idx]);
                    //     goto initiate_migration;
                    // } 
                    // else if (entry->status == 4) {
                    //     entry->status = 6;
                    //     spin_unlock(&sharing_map_lock[hash_idx]);
                    //     *status = -2;
                    //     return pfn;
                    // } 
                    else {
                        // pr_err("Unexpected sync flag: %d\n", entry->status);
                        spin_unlock(&sharing_map_lock[hash_idx]);
                        *status = -2;
                        return pfn;
                    }
                } else {
                    // Already have some page to migrate
                    // should allocate PFNs for previous pages
                    // And initiate migration request
                    // pr_err("Some pages are already migrated\n");
                    spin_unlock(&sharing_map_lock[hash_idx]);
                    goto initiate_migration;
                }
            } else if (sharing_map_slot_free(entry)) {
                /* remember the first reusable slot; a never-used slot ends the chain */
                if (free_idx == U32_MAX)
                    free_idx = hash_idx;
                if (entry->virtual_page_index == 0) {
                    spin_unlock(&sharing_map_lock[hash_idx]);
                    break;
                }
            }
            spin_unlock(&sharing_map_lock[hash_idx]);
        }
        if (!matched) {
            /* not in the map: create the entry in the first free slot of its chain */
            if (free_idx == U32_MAX) {
                pr_err("Giving up searching the entry due to the threshold: 0x%llx\n", addr);
                *status = -1;
                return pfn;
            }
            spin_lock(&sharing_map_lock[free_idx]);
            entry = &sharing_map[free_idx];
            if (entry->virtual_page_index == vpi || !sharing_map_slot_free(entry)) {
                /* raced with another inserter for this slot: look again */
                spin_unlock(&sharing_map_lock[free_idx]);
                goto reprobe;
            }
            entry->virtual_page_index = vpi;
            entry->pfn = 0;
            entry->sharing_bitmap = 0;
            pfn_pointer_array[page_idx] = &(entry->pfn);
            claimed[page_idx] = 1;
            entry->status = 1;
            if (host_idx >= 0)
                entry->sharing_bitmap |= 1 << host_idx;
            // Assume the batched pages do not belong to different nodes
            *owner_node = specify_process_idx(addr);
            if (*owner_node < 0)
                pr_err("Invalid owner node\n");
            entry->sharing_bitmap |= 1 << (*owner_node);
            spin_unlock(&sharing_map_lock[free_idx]);
        }
        vpi += 1;
    }

    // If we have reached the the migration batch size, allocate PFN and initiate migration
initiate_migration:
    // Allocate PFNs for previous pages
    // pfn_start = get_shared_page_pfn(page_idx);
    int mig_prefetch_pfns_idx_local;
    while(1) {
        mig_prefetch_pfns_idx_local = atomic_fetch_add(1, &mig_prefetch_pfns_idx) % (MIG_PREFETCH_ENTRY_NUM);
        if (mig_prefetch_pfns[mig_prefetch_pfns_idx_local].pfns[0] == 0)
            break;
    }
    for (int i = 0; i < page_idx; i++) {
        pfn_start = get_limited_shared_page_pfn(1);
        if (pfn_start == 0) {
            /*
             * Pool exhausted. Undo everything this call did so the fault
             * can be retried from scratch once the monitor has evicted:
             * return the PFNs already taken, clear the prefetch slot, and
             * put the entries back the way we found them.
             */
            for (int k = 0; k < i; k++) {
                free_pfn(*pfn_pointer_array[k]);
                *pfn_pointer_array[k] = 0;
                mig_prefetch_pfns[mig_prefetch_pfns_idx_local].pfns[k] = 0;
            }
            for (int k = 0; k < page_idx; k++) {
                struct sharing_map_entry *e;
                uint32_t idx;
                if (!pfn_pointer_array[k] || !claimed[k])
                    continue;
                e = container_of(pfn_pointer_array[k], struct sharing_map_entry, pfn);
                idx = e - sharing_map;
                spin_lock(&sharing_map_lock[idx]);
                if (claimed[k] == 1) {
                    e->virtual_page_index = SHARING_MAP_TOMBSTONE;
                    e->pfn = 0;
                    e->sharing_bitmap = 0;
                    e->status = 0;
                } else {
                    e->status = 0;
                    if (host_idx >= 0)
                        e->sharing_bitmap &= ~(1u << host_idx);
                }
                spin_unlock(&sharing_map_lock[idx]);
            }
            *status = -3;
            *num_pages = 0;
            return 0;
        }
        if (!is_shared_page_pfn(pfn_start)) {
            pr_err("Invalid PFN during batching: %ld\n", pfn_start);
            BUG_ON(1);
        }
        *pfn_pointer_array[i] = (unsigned long)(pfn_start);
        mig_prefetch_pfns[mig_prefetch_pfns_idx_local].pfns[i] = pfn_start;
        // pr_info("migration: addr=0x%llx, pfn=%ld, id=%d. proc: %d, owner: %d\n", addr + (i << PAGE_SHIFT), pfn_start, mig_prefetch_pfns_idx_local, host_idx, *owner_node);
    }
    *status = 1;
    *num_pages = page_idx;
    *pfns_idx = mig_prefetch_pfns_idx_local;
    return *pfn_pointer_array[0];



    // pfn_start = get_limited_shared_page_pfn(page_idx);
    // if (!is_shared_page_pfn(pfn_start)) {
    //     pr_err("Invalid PFN: %ld\n", pfn_start);
    //     // spin_unlock(&sharing_map_lock[hash_idx]);
    //     *status = -1;
    //     return pfn_start;
    // }
    // // Set PFNs for previous pages
    // for (int page_offset = 0; page_offset < page_idx; page_offset++) {
    //     *(pfn_pointer_array[page_offset]) = (unsigned long)(pfn_start + page_offset);
    // }
    // // Set status to initiate migration request
    // *status = 1;
    // *num_pages = page_idx;
    // // spin_unlock(&sharing_map_lock[hash_idx]);
    // return pfn_start;
}

unsigned long find_fast_path(uint64_t addr) {
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry* entry;
    unsigned long pfn = 0;
    int host_idx = get_process_idx(current->tgid);

    
    for (int i = 0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        spin_lock(&sharing_map_lock[hash_idx]);
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi && entry->status == 3) {
            if (host_idx >= 0) {
                entry->sharing_bitmap |= 1 << host_idx;
            }
            pfn = entry->pfn;
            spin_unlock(&sharing_map_lock[hash_idx]);
            return pfn;
        }
        spin_unlock(&sharing_map_lock[hash_idx]);
    }
    return ULONG_MAX;
}

int sharing_map_entry_reset(uint32_t idx) {
    struct sharing_map_entry* entry;

    /* Every other accessor of an entry holds its bucket lock; resetting
     * without it lets concurrent probes observe a torn entry (e.g. pfn==0
     * with a stale virtual_page_index). The only caller is the
     * firework_move_pages completion loop, which holds no bucket lock. */
    spin_lock(&sharing_map_lock[idx]);
    entry = &sharing_map[idx];
    entry->virtual_page_index = SHARING_MAP_TOMBSTONE;
    entry->pfn = 0;
    entry->sharing_bitmap = 0;
    entry->status = 0;
    spin_unlock(&sharing_map_lock[idx]);

    return 0;
}

// Fast path alternative of shared_mem_get_or_create() to check whether the page is ready
// waiting on in-progress entry: status = 0 (wait)
// entry is ready: status = 2 (ready)
// last migration failed, retry migration: status = 3
unsigned long sharing_map_recheck(int *status, uint32_t hash_idx, unsigned long addr) {
    struct sharing_map_entry* entry;
    unsigned long pfn = 0;

    spin_lock(&sharing_map_lock[hash_idx]);
    entry = &sharing_map[hash_idx];
    if (entry->virtual_page_index != (addr >> PAGE_SHIFT) || entry->status == 0) {
        /* the entry we were waiting on was rolled back (pool exhausted on
         * the initiator) or reused for another page: start over */
        *status = -2;
    } else if (entry->status == 1) {
        *status = 0;
    } else if (entry->status == 2) {
        entry->status = 1;
        *status = 3;
        pfn = entry->pfn;
    } else if (entry->status == 4 || entry->status == 5) {
        *status = -2;
    } else {
        *status = 2;
        pfn = entry->pfn;
    }
    spin_unlock(&sharing_map_lock[hash_idx]);
    return pfn;
}

int check_status_flag(unsigned long addr) {
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry* entry;
    
    for (int i=0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        spin_lock(&sharing_map_lock[hash_idx]);
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi) {
            int status = entry->status;
            spin_unlock(&sharing_map_lock[hash_idx]);
            return status;
        }
        spin_unlock(&sharing_map_lock[hash_idx]);
    }
    pr_err("No such entry to check status flag: 0x%lx\n", addr);
    return -1;
}

unsigned long check_pfn_mapcount(unsigned long addr) {
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry* entry;
    unsigned long pfn;
    struct folio* folio;
    
    for (int i=0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        spin_lock(&sharing_map_lock[hash_idx]);
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi) {
            pfn = entry->pfn;
            spin_unlock(&sharing_map_lock[hash_idx]);
            break;
        }
        spin_unlock(&sharing_map_lock[hash_idx]);
    }

    folio = pfn_folio(pfn);
    if (!folio) {
        BUG_ON(1);
    }
    pr_info("refcount. addr: 0x%lx, pfn: %lu, mapcount: %d. by %d\n", addr, pfn, folio_mapcount(folio), get_process_idx(current->tgid));
    return folio_mapcount(folio);
}

int set_status_flag(uint64_t addr, int mode, int idx) {
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry* entry;
    

    for (int i=0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        spin_lock(&sharing_map_lock[hash_idx]);
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi) {
            if (entry->status == 3 && mode == 0) {
                entry->status = 4;
                spin_unlock(&sharing_map_lock[hash_idx]);
                return 0;
            }
            if (entry->status == 4 && mode == 1) {
                entry->sharing_bitmap &= ~(1 << idx);
                BUG_ON(entry->sharing_bitmap == 0);
                if (is_power_of_2(entry->sharing_bitmap)) {
                    entry->status = 5;
                    spin_unlock(&sharing_map_lock[hash_idx]);
                    return 1;
                }
                spin_unlock(&sharing_map_lock[hash_idx]);
                return 0;
            }
            // if (entry->status == 6 && mode == 1) {
            //     entry->sharing_bitmap &= ~(1 << idx);
            //     WARN(entry->sharing_bitmap == 0, "Sharing bitmap becomes zero when unmapping: 0x%llx\n", addr);
            //     if (is_power_of_2(entry->sharing_bitmap)) {
            //         spin_unlock(&sharing_map_lock[hash_idx]);
            //         return 1;
            //     }
            //     spin_unlock(&sharing_map_lock[hash_idx]);
            //     return 0;
            // }
            if (entry->status == 5 && mode == 2) {
                entry->status = 0;
                entry->pfn = 0;
                spin_unlock(&sharing_map_lock[hash_idx]);
                return 0;
            }
            // if (entry->status == 6 && mode == 3) {
            //     // atomic_add(1, &unshare_counter);
            //     finish_unshare();
            //     save_addr_buffer(addr);
            //     entry->status = 3;
            //     spin_unlock(&sharing_map_lock[hash_idx]);
            //     return 1;
            // }
        }
        spin_unlock(&sharing_map_lock[hash_idx]);
    }
    if (mode != 3)
        pr_err("No such entry to set status flag: 0x%llx, %d, %d\n", addr, mode, idx);
    return -1;
}

// On success, return a free PFN and bump shared_page_offset
// On failure, return 0
unsigned long get_shared_page_pfn(int num_pages) {
    unsigned long pfn = 0;
    unsigned long pfn_offset = atomic_fetch_add(num_pages, &shared_page_offset);
    if (pfn_offset + num_pages - 1 < shared_page_capacity) {
        pfn = shared_page_start + pfn_offset;
    }
    return pfn;
}

/*
 * Return pfns to the free ring. Slots are reserved with a separate counter,
 * the pfns are written, and only then is available_queue_tail advanced, in
 * reservation order. Advancing tail first (the old code) let a faulter that
 * polls for free space claim a slot and read its STALE contents - a pfn
 * handed out long ago and possibly mapped right now ("Allocated PFN is
 * already mapped", sci_kernel N=3 under pool pressure). That was masked
 * while faulters slept on a wait queue that was only woken after the write.
 */
void pool_return_pfns(const int *pfns, int n)
{
    int start = atomic_fetch_add(n, &available_queue_reserve);

    for (int i = 0; i < n; i++)
        available_region_queue[(start + i) % pfn_range] = pfns[i];
    smp_wmb();
    /* publish in reservation order so tail never covers an unwritten slot */
    while (atomic_read(&available_queue_tail) != start)
        cpu_relax();
    atomic_set(&available_queue_tail, start + n);
}

unsigned long get_limited_shared_page_pfn(int num_pages) {
    int head, tail;
    long pfn;

    BUG_ON(num_pages != 1);

    /*
     * Claim exactly one filled slot with cmpxchg: the emptiness check and
     * the head increment must be one atomic step, otherwise two consumers
     * racing for the last slot both pass the check and one consumes a
     * slot at/past tail (a PFN still mapped elsewhere).
     */
    /*
     * Non-blocking: the only caller is the page-fault path, which holds
     * mmap_read_lock. Sleeping here for the eviction monitor deadlocked
     * (the monitor needs the daemons, whose kernel work needs this mm's
     * lock behind a queued writer). On an empty pool return 0; the caller
     * unwinds, drops mmap_lock and retries the fault.
     */
    for (;;) {
        head = atomic_read(&available_queue_head);
        tail = atomic_read(&available_queue_tail);
        if (tail - head <= 0) {
            scan_needed = 1;
            wake_up_all(&scan_wq);
            return 0;
        }
        if (atomic_cmpxchg(&available_queue_head, head, head + 1) == head)
            break;
    }
    smp_rmb();   /* pairs with the smp_wmb() in pool_return_pfns() */
    pfn = available_region_queue[head % pfn_range];
    /* a fresh sharing starts with no accessor history */
    if (epoch_prev_accessors)
        epoch_prev_accessors[pfn - shared_page_start] = 0;

    if (atomic64_read(&rmap_list[NODE_NUM][pfn - shared_page_start]) != 0) {
        pr_err("Allocated PFN is already mapped: %lu\n", pfn);
        pr_err("head: %d, tail: %d, claimed: %d\n", atomic_read(&available_queue_head), atomic_read(&available_queue_tail), head);
        BUG_ON(1);
    }

    // if (atomic64_read(&rmap_list[NODE_NUM][pfn - shared_page_start]) != 0) {
    //     pr_err("Allocated PFN is already mapped: %lu\n", pfn);
    //     pr_err("head: %d, tail: %d, available_queue_idx: %d\n", atomic_read(&available_queue_head), atomic_read(&available_queue_tail), available_queue_idx);
    //     BUG_ON(1);
    // }
    // WARN_ONCE(pfn >= shared_page_start + pfn_range, "Allocated PFN is out of range: %lu > %llu\n", pfn, shared_page_start + pfn_range);

    if (pfn_range - (tail - head) >= pfn_range*81/100) {
        scan_needed = 1;
        wake_up_all(&scan_wq);
    }

    /*
     * Hand the page out with the allocation reference that the migration
     * path assumes. The destination of a Firework migration comes from
     * this pool, not from get_new_page(), yet firework_migrate_folio_move()
     * still does the mainline folio_put(dst) on success, which is only
     * balanced if dst entered with a +1 allocation ref. Reclaim (do_unmap)
     * drops every mapping, so a recycled pool page returns to the free
     * queue at refcount 0; without this a migration into it ends at
     * refcount 0 and the accessor's folio_try_get() fails forever
     * ("failed to get folio ... refcount: 0"). Only normalize 0 -> 1; a
     * page already holding references is left alone rather than clobbered.
     */
    {
        struct page *page = pfn_to_page(pfn);
        if (page_count(page) == 0)
            set_page_count(page, 1);
    }

    return pfn;
}

// unsigned long get_limited_shared_page_pfn(int num_pages) {
//     unsigned long head, tail;
//     // uint64_t position;
//     int cur_range;

//     while(1) {
//         spin_lock(&hwc_lock);
//         // spin_lock_irqsave(&hwc_lock, flags);
//         // position = atomic64_read(&shared_page_position);
//         // head = position >> 32;
//         // tail = position & 0xFFFFFFFF;
//         head = atomic_read(&shared_page_head);
//         tail = atomic_read(&shared_page_tail);
//         // head = atomic_read(&hwc_head);
//         // tail = atomic_read(&shared_page_offset);
        
//         if (tail - head + num_pages < pfn_range) {
//             // atomic_add(num_pages, &shared_page_offset);
//             // atomic64_add((uint64_t)num_pages, &shared_page_position);
//             atomic_add(num_pages, &shared_page_tail);
//             spin_unlock(&hwc_lock);
//             // spin_unlock_irqrestore(&hwc_lock, flags);
//             break;
//         }
//         spin_unlock(&hwc_lock);
//         // spin_unlock_irqrestore(&hwc_lock, flags);
//         // cond_resched();
//     }

//     cur_range = tail - head;

//     // spin_lock(&hwc_lock);
//     // head = atomic_read(&hwc_head);
//     // pfn_offset = atomic_fetch_add(num_pages, &shared_page_offset);
//     // spin_unlock(&hwc_lock);
//     // cur_range = pfn_offset - head;
//     WARN_ONCE(shared_page_capacity < pfn_range, "Shared page capacity is smaller than PFN range: %lu < %lld\n", shared_page_capacity, pfn_range);
//     WARN_ONCE(cur_range >= pfn_range, "cur_range >= pfn_range: %d >= %lld\n", cur_range, pfn_range);

//     // return shared_page_start + pfn_offset;
//     return shared_page_start + tail;
// }


// int unshare_if_needed(unsigned long addr, struct unshare_info* info) {
//     // pr_info("accessed: addr=%lx\n", addr);
//     if (info->flag == 1) {
//         unsigned long unsharing_start = (info->pfn_offset > pfn_range) ? (info->pfn_offset - pfn_range) : 0;
//         unsigned long unsharing_end = info->pfn_offset + info->num_pages - pfn_range;
//         //unsharing (unsharing_end - unsharing_start) pages
//         for(unsigned long i = unsharing_start; i < unsharing_end; i++) {
//             unsigned long unsharing_addr = addr_buffer[i % pfn_range];
//             // pr_info("unsharing!!: %llu, %lx\n", i%pfn_range, unsharing_addr);
//             set_status_flag(unsharing_addr, 0);
//             unmap_queue_produce(unsharing_addr);
//             // set_status_flag(unsharing_addr, 1);
//             // unshare_queue_produce(unsharing_addr);
//             // set_status_flag(unsharing_addr, 2);
//         }
//     }

//     // for(int i = 0; i < info->num_pages; i++) {
//     //     addr_buffer[(info->pfn_offset + i) % pfn_range] = addr + i * PAGE_SIZE;
//     //     pr_info("save buffer[%llu] = %llx\n", (info->pfn_offset + i) % pfn_range, addr_buffer[(info->pfn_offset + i) % pfn_range]);
//     // }

//     return 0;
// }

void save_addr_buffer(unsigned long addr) {
    unsigned long idx;

    idx = atomic_fetch_add(1, &addr_buffer_idx);
    addr_buffer[idx % pfn_range] = addr;
}

bool get_hint_zone_pfn(unsigned long addr, unsigned long *pfn) {
    if (addr >= firework_hint_zone_base && addr < firework_hint_zone_bound) {
        uint64_t page_offset = (addr - firework_hint_zone_base)>>PAGE_SHIFT;
        switch (firework_hint_zone_mode) {
        case 0: // plain mode
            *pfn = firework_hint_zone_pfn_base + page_offset;
            if (*pfn >= shared_page_start)
                pr_err("Invalid hint zone PFN: %lx\n", *pfn);
            break;
        case 1: // interleaved mode
            if (page_offset % 2 == 0) {
                *pfn = firework_hint_zone_pfn_base + (page_offset>>1);
                if (*pfn >= shared_page_start)
                    pr_err("Invalid hint zone PFN from dax0: %lx\n", *pfn);
            } else {
                *pfn = firework_hint_zone_pfn_base_2 + (page_offset>>1);
                if (*pfn >= firework_hint_zone_pfn_bound_2)
                    pr_err("Invalid hint zone PFN from dax1: %lx\n", *pfn);
            }
            break;
        case 2: // tiered mode
            if (addr < firework_hint_zone_base + firework_fast_tier_size_byte) {
                *pfn = firework_hint_zone_pfn_base + (page_offset);
                if (*pfn >= shared_page_start)
                    pr_err("Invalid hint zone PFN from fast tier: %lx\n", *pfn);
            } else {
                // This is in the second tier
                *pfn = firework_hint_zone_pfn_base_2 + (page_offset - (firework_fast_tier_size_byte >> PAGE_SHIFT));
                if (*pfn >= firework_hint_zone_pfn_bound_2)
                    pr_err("Invalid hint zone PFN from dax1: %lx\n", *pfn);
            }
            break;
        default:
            pr_err("Invalid hint zone mode: %d\n", firework_hint_zone_mode);
            return false;
        }
        return true;
    }
    return false;
}

int get_process_idx(pid_t pid) {
    if (pid == firework_managed_process_pid_0) {
        return 0;
    } else if (pid == firework_managed_process_pid_1) {
        return 1;
    } else if (pid == firework_managed_process_pid_2) {
        return 2;
    } else if (pid == firework_managed_process_pid_3) {
        return 3;
    } else if (pid == firework_managed_process_pid_4) {
        return 4;
    } else if (pid == firework_managed_process_pid_5) {
        return 5;
    } else if (pid == firework_managed_process_pid_6) {
        return 6;
    } else if (pid == firework_managed_process_pid_7) {
        return 7;
    }
    else {
        return -1;
    }
}

int specify_process_idx(unsigned long addr) {

    pid_t pid = current->tgid;
    int self_process_idx = get_process_idx(pid);
    int ret_process_idx = -1;
    
    if (unlikely(self_process_idx < 0)) {
        pr_err("Invalid process pid: %d\n", pid);
        return -1;
    }

    if (unlikely(!is_firework_process(pid))) {
        pr_err("Not a firework process: %d\n", pid);
        return -1;
    }

    // If this is a heap address
    if (addr >= firework_heap_base && addr < firework_heap_bound) {
        unsigned long diff = addr - firework_heap_base;
        ret_process_idx = (int)(diff / firework_heap_region_size);
    }
    // If this is a stack address
    else if (addr >= firework_stack_base && addr < firework_stack_bound) {
        unsigned long diff = addr - firework_stack_base;
        ret_process_idx = (int)(diff / firework_stack_region_size);
    }
    else {
        pr_err("Not a heap or stack address: %lx\n", addr);
        ret_process_idx = -1;
    }

    return ret_process_idx;
}

uint32_t find_sharing_map_idx(unsigned long addr) {
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry* entry;

    for (int i = 0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        spin_lock(&sharing_map_lock[hash_idx]);
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi) {
            spin_unlock(&sharing_map_lock[hash_idx]);
            return hash_idx;
        }
        else if (entry->virtual_page_index == 0) {
            spin_unlock(&sharing_map_lock[hash_idx]);
            return U32_MAX;
        }
        spin_unlock(&sharing_map_lock[hash_idx]);
    }
    return U32_MAX;
}

unsigned int check_sharing_map_sync(uint32_t idx) {
    struct sharing_map_entry* entry = &sharing_map[idx];
    return entry->status;
}

void clear_sharing_map_sync(uint32_t idx) {
    // TODO: Shi: double check whether this is correct without lock
    // Logically the sync flag has only one writer (the migration procedure)
    struct sharing_map_entry* entry;
    spin_lock(&sharing_map_lock[idx]);
    entry = &sharing_map[idx];
    if (entry->status == 1) entry->status = 3;
    else pr_err("Unexpected sync flag when clearing: %d\n", entry->status);
    // entry->status = 0;
    spin_unlock(&sharing_map_lock[idx]);
}

/*
 * A migration for this entry failed: put it back to the private/DEAD state
 * with only the holder's bit left (drop the requester's), so the retry
 * re-arms it through the normal path with the right owner. Marking it
 * "failed" (status 2) made the retry reuse stale owner/prefetch state.
 */
void sharing_map_fail_to_dead(uint32_t idx, int requester) {
    struct sharing_map_entry* entry;
    spin_lock(&sharing_map_lock[idx]);
    entry = &sharing_map[idx];
    entry->status = 0;
    entry->pfn = 0;
    if (requester >= 0 && requester < NODE_NUM)
        entry->sharing_bitmap &= ~(1u << requester);
    spin_unlock(&sharing_map_lock[idx]);
}

void set_sharing_map_failed(uint32_t idx) {
    // TODO: Shi: double check whether this is correct without lock
    // Logically the sync flag has only one writer (the migration procedure)
    struct sharing_map_entry* entry;
    spin_lock(&sharing_map_lock[idx]);
    entry = &sharing_map[idx];
    entry->status = 2;
    spin_unlock(&sharing_map_lock[idx]);
}

int firework_network_init(int fw_server_idx, int fw_server_size) {
    struct sockaddr_in server_addr;
    int err;

    for (int i=0; i<fw_server_size; i++) {
        if (fw_server_idx == i) {
            continue;
        }

        err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &global_sockets[fw_server_idx][i]);
        if (err < 0) {
            printk(KERN_ERR "Error creating socket: %d\n", i);
            return err;
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(40000+i);
        server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    retry:
        err = global_sockets[fw_server_idx][i]->ops->connect(global_sockets[fw_server_idx][i], (struct sockaddr *)&server_addr, sizeof(server_addr), 0);
        if (err < 0) {
            // printk(KERN_ERR "Error connecting to server\n");
            // sock_release(sock[i]);
            // return err;
            goto retry;
        }

    }
    return 0;
}

void sync_queue_init(size_t queue_capacity) {
    struct SyncronousQueue *queue;
    for (int i=0; i<SH_QUEUE_NUM; i++) {
        if (i < MIG_QUEUE_NUM) {
            queue = &mig_queues[i];    
        } else if (i < MIG_QUEUE_NUM + THD_QUEUE_NUM) {
            queue = &thd_queues[i - MIG_QUEUE_NUM];
        } else if (i < MIG_QUEUE_NUM + THD_QUEUE_NUM + UNSHARE_QUEUE_NUM) {
            queue = &unshare_queues[i - MIG_QUEUE_NUM - THD_QUEUE_NUM];
        } else {
            queue = &scan_queues[i - MIG_QUEUE_NUM - THD_QUEUE_NUM - UNSHARE_QUEUE_NUM];
        }

        queue->data = (struct queue_data*) ((uint64_t)sh_queues_start + SH_QUEUE_SIZE * i + SH_QUEUE_HEADER_SIZE);
        queue->head = (int*) ((uint64_t)sh_queues_start + SH_QUEUE_SIZE * i);
        queue->tail = queue->head + 1;
        queue->done = queue->tail + 1;
        if (queue_capacity)
            queue->capacity = queue_capacity;
        else
            queue->capacity = SH_QUEUE_ENTRY_NUM;

        queue->flags = (uint8_t*) ((uint64_t)sh_queues_start + SH_QUEUE_SIZE * i + SH_QUEUE_HEADER_SIZE + queue->capacity * sizeof(struct queue_data));
        // queue->flags = (uint8_t*) ((uint64_t)sh_queues_start + SH_QUEUE_SIZE * i + SH_QUEUE_HEADER_SIZE + queue->capacity * sizeof(struct queue_data));

        // mig_queues[i].data = (struct QueueData*) ((uint64_t)sh_queues_start + SH_QUEUE_SIZE * i + SH_QUEUE_HEADER_SIZE);
        // mig_queues[i].head = (int*) ((uint64_t)sh_queues_start + SH_QUEUE_SIZE * i);
        // mig_queues[i].tail = mig_queues[i].head + 1;
        // mig_queues[i].done = mig_queues[i].tail + 1;
        // if (queue_capacity)
        //     mig_queues[i].capacity = queue_capacity;
        // else
        //     mig_queues[i].capacity = SH_QUEUE_ENTRY_NUM;

        // mig_queues[i].flags = (uint8_t*) ((uint64_t)sh_queues_start + SH_QUEUE_SIZE * i + SH_QUEUE_HEADER_SIZE + mig_queues[i].capacity * sizeof(struct QueueData));
    }
}

/*
 * Enqueue a migration request on the owner's daemon queue WITHOUT waiting
 * for it. Returns the slot index (>= 0) to pass to sync_queue_wait(), or
 * < 0 on error. Split out so the page fault path can drop mmap_lock
 * before it waits: waiting with the lock held lets a queued writer on
 * this mm (thread-stack mmap, brk) block our own daemon's madvise, which
 * the owner's daemon may be waiting on before it can serve our request.
 */
int sync_queue_produce_async(unsigned long addr, unsigned long num_pages, int queue_idx, int pfns_idx) {
    int currentTail, currentDone, index;
    struct queue_data data = {Q_MIGRATE, .payload.mig = {addr, num_pages, pfns_idx}};
    struct SyncronousQueue *queue;

    if (queue_idx < 0 || queue_idx >= NODE_NUM) {
        pr_err("%s: Invalid process idx: %d\n", __func__, queue_idx);
        return -1;
    }
    if (queue_idx == get_process_idx(current->tgid)) {
        pr_err("%s: address is local. addr: %lx; idx: %d\n", __func__, addr, queue_idx);
        return -1;
    }
    data.src_node = get_process_idx(current->tgid);
    queue = &mig_queues[queue_idx];

    while (true) {
        currentTail = __atomic_load_n(queue->tail, __ATOMIC_SEQ_CST);
        currentDone = __atomic_load_n(queue->done, __ATOMIC_SEQ_CST);
        if ((currentTail - currentDone) >= queue->capacity) {
            sync_queue_update_done(queue);
            cond_resched();
            continue;
        }
        if (__atomic_compare_exchange_n(queue->tail, &currentTail, currentTail + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            index = currentTail % queue->capacity;
            queue->data[index] = data;
            if (__atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST) != 0) {
                pr_err("An in-progress event is overwritten\n");
                return -1;
            }
            __atomic_store_n(&queue->flags[index], 1, __ATOMIC_SEQ_CST);
            return index;
        }
    }
}

/* Current completion flag of a request enqueued by sync_queue_produce_async():
 * <= 1 still pending, 2 success, >2 failure. Takes no locks. */
int sync_queue_poll(int queue_idx, int index) {
    return __atomic_load_n(&mig_queues[queue_idx].flags[index], __ATOMIC_SEQ_CST);
}

/* Release the slot once sync_queue_poll() reported completion. */
void sync_queue_finish(int queue_idx, int index) {
    __atomic_store_n(&mig_queues[queue_idx].flags[index], 0, __ATOMIC_SEQ_CST);
}

/* Wait for a request enqueued by sync_queue_produce_async(); returns the
 * daemon's completion code (2 = success). Takes no locks. */
int sync_queue_wait(int queue_idx, int index) {
    int ret;

    do {
        cond_resched();
        ret = sync_queue_poll(queue_idx, index);
    } while (ret <= 1);
    sync_queue_finish(queue_idx, index);
    return ret;
}

int sync_queue_produce(unsigned long addr, unsigned long pfn, unsigned long num_pages, int queue_idx, int pfns_idx) {
    int currentTail, currentDone, index;
    uint8_t ret;
    // struct QueueData data = {addr, pfn, num_pages};
    // struct queue_data data = {Q_MIGRATE, .payload.mig = {addr, pfn, num_pages}};
    struct queue_data data = {Q_MIGRATE, .payload.mig = {addr, num_pages, pfns_idx}};
    // int queue_idx = specify_process_idx(addr);
    struct SyncronousQueue *queue;
    int src_node;
    // Only used to ensure sync flag is clear after migration is done
    // TODO: Shi: May be removed later
    uint32_t sharing_map_idx = find_sharing_map_idx(addr);
    if (sharing_map_idx == U32_MAX) {
        pr_err("%s: sharing_map entry not found for addr: %lx\n", __func__, addr);
        return -1;
    }

    src_node = get_process_idx(current->tgid);
    data.src_node = src_node;

    if (queue_idx < 0) {
        pr_err("%s: Invalid process idx: %d\n", __func__, queue_idx);
        return -1;
    } else if (queue_idx == get_process_idx(current->tgid)) {
        pr_err("%s: address is local. addr: %lx; idx: %d\n", __func__, addr, queue_idx);
        return -1;
    }
    queue = &mig_queues[queue_idx];

    while(true) {
        currentTail = __atomic_load_n(queue->tail, __ATOMIC_SEQ_CST);
        currentDone = __atomic_load_n(queue->done, __ATOMIC_SEQ_CST);

        if ((currentTail - currentDone) >= queue->capacity) {
            sync_queue_update_done(queue);
            cond_resched();
            continue;
        }

        if (__atomic_compare_exchange_n(queue->tail, &currentTail, currentTail + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            index = currentTail % queue->capacity;
            queue->data[index] = data;

            if (__atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST) != 0) {
                pr_err("An in-progress event is overwritten\n");
                return -1;
            }

            __atomic_store_n(&queue->flags[index], 1, __ATOMIC_SEQ_CST);
            ret = 0;
            do {
                cond_resched();
                ret = __atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST);
            } while (ret <= 1);

            __atomic_store_n(&queue->flags[index], 0, __ATOMIC_SEQ_CST);            
            break;
        }
    }

    // Flag update should happen after the data is written
    // So if the sharing map bit is not set. There is an issue
    if (check_sharing_map_sync(sharing_map_idx) != 3) {
        pr_err("Sync flag not set in sharing map for addr: %lx, pfn: %ld, queue_idx: %d, num_pages: %lu\n", addr, pfn, queue_idx, num_pages);
        pr_err("Sync flag: %d\n", check_sharing_map_sync(sharing_map_idx));
        BUG_ON(1);
        return -1;
    }

    WARN_ONCE(pfn-shared_page_start >= pfn_range, "PFN out of range in rmap_list: %lx\n", pfn);
    WARN_ONCE(pfn < shared_page_start, "PFN below shared_page_start in rmap_list: %lx\n", pfn);

    // spin_lock(&hwc_lock);
    // list_add_tail(&shared_page_data_list[pfn - shared_page_start].list, &inactive_pages);
    // spin_unlock(&hwc_lock);

    return ret;
}

int sync_queue_get_event(unsigned long *addr, unsigned long *pfn) {
    int process_idx = get_process_idx(current->tgid);
    struct SyncronousQueue *queue;
    int currentHead, currentTail, index;
    uint8_t ready;

    pr_err("This function should not be used anymore!\n");
    BUG_ON(1);

    pr_err("%s: should not reach here!\n", __func__);
    return 0;

    if (process_idx < 0) {
        pr_err("Invalid process idx: %d\n", process_idx);
        return -1;
    }
    queue = &mig_queues[process_idx];

    while (true) {
        currentHead = __atomic_load_n(queue->head, __ATOMIC_SEQ_CST);
        currentTail = __atomic_load_n(queue->tail, __ATOMIC_SEQ_CST);

        if (currentHead >= currentTail) {
            return -1;
        }

        index = currentHead % queue->capacity;
        if (__atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST) != 1) {
            cond_resched();
            continue;
        }

        if (__atomic_compare_exchange_n(queue->head, &currentHead, currentHead + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            index = currentHead % queue->capacity;
            ready = __atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST);
            while (ready != 1) {
                pr_err("Invalid flags in sync_queue_get_event\n");
                return -1;
            }
            *addr = queue->data[index].payload.mig.addr;
            // *pfn = queue->data[index].payload.mig.pfn;
            return index;
        }
    }
}

int sync_queue_finish_event(struct SyncronousQueue *queue, int index, uint8_t ret) {
    uint8_t flag_snapshot = __atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST);

    pr_err("%s: should not reach here!\n", __func__);
    return 0;

    if (flag_snapshot != 1) {
        pr_err("Invalid flags\n");
        return -1;
    }
    if (ret == 0 || ret == 1) {
        pr_err("Invalid return value\n");
        return -1;
    }

    __atomic_store_n(&queue->flags[index], ret, __ATOMIC_SEQ_CST);
    sync_queue_update_done(queue);
    return 0;
}

void sync_queue_update_done(struct SyncronousQueue *queue) {
    int currentDone, currentHead, newDone;
    uint8_t ready;

    currentDone = __atomic_load_n(queue->done, __ATOMIC_SEQ_CST);
    currentHead = __atomic_load_n(queue->head, __ATOMIC_SEQ_CST);
    newDone = currentDone;

    while(newDone < currentHead) {
        ready = __atomic_load_n(&queue->flags[newDone % queue->capacity], __ATOMIC_SEQ_CST);
        if(ready != 0) {
            break;
        }
        newDone++;
    }
    __atomic_compare_exchange_n(queue->done, &currentDone, newDone, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/*
 * Epoch-based candidate selection (paper §4.4, clock-style two-epoch rule).
 * Called right after a cluster-wide accessed-bit scan, so
 * pg_access_info_list[] holds this epoch's accessor bitmap per pool page and
 * epoch_prev_accessors[] the previous epoch's. A page is a candidate when
 * each epoch saw at most one accessor, at least one epoch saw an access, and
 * if both did it was the same node. Candidates go to pfn_buffer[] with the
 * accessor encoded in the top 4 bits (consumed by do_unmap). Returns the
 * number picked. This epoch's bitmap becomes the history for the next one.
 */
static int epoch_pick_eviction_pages(void) {
    int picked = 0;

    for (unsigned long idx = 0; idx < pfn_range; idx++) {
        unsigned long addr = atomic64_read(&rmap_list[NODE_NUM][idx]);
        unsigned int cur = atomic_read(&pg_access_info_list[idx].accessors);
        unsigned int prev = epoch_prev_accessors[idx];
        unsigned int both;

        atomic_set(&pg_access_info_list[idx].count, 0);
        atomic_set(&pg_access_info_list[idx].accessors, 0);
        if (addr == 0) {
            epoch_prev_accessors[idx] = 0;
            continue;
        }
        epoch_prev_accessors[idx] = cur;

        both = cur | prev;
        if (both == 0)                       /* idle: keep it, capacity permitting */
            continue;
        if (!is_power_of_2(both))            /* >1 node in either epoch, or two nodes across epochs */
            continue;
        pfn_buffer[picked] = (shared_page_start + idx) | ((unsigned long)(ffs(both) - 1) << 60);
        picked++;
    }
    return picked;
}

/* One periodic epoch of the unsharing manager: scan, pick, unshare. */
static int epoch_unshare_round(void) {
    int picked, count = 0, done, unshare_num;
    int returned = 0;
    int *buf;

    if (!start_flag || program_end)
        return 0;
    /* the scan daemons in the runtime only serve requests while this is set */
    scan_needed = 1;
    if (scan_request() < 0)
        return 0;
    picked = epoch_pick_eviction_pages();
    if (picked < (int)epoch_min_pages)
        return 0;

    buf = kvmalloc(sizeof(int) * UNSHARE_BATCH_SIZE, GFP_KERNEL);
    if (!buf)
        return 0;
    while (picked > 0) {
        if (!unmap_queue_check()) {
            pr_info("Unmap queue full during epoch unshare, wait\n");
            break;
        }
        unshare_num = (picked > UNSHARE_BATCH_SIZE) ? UNSHARE_BATCH_SIZE : picked;
        picked -= unshare_num;
        done = do_unmap(count, unshare_num, buf, 0);
        count += unshare_num;
        if (done <= 0)
            continue;
        pool_return_pfns(buf, done);
        wake_up_all(&shared_page_wq);
        returned += done;
    }
    kvfree(buf);
    if (returned > 0) {
        epoch_unshare_count += returned;
        epoch_round_count++;
        pr_info_ratelimited("epoch unshare: %d pages returned to the pool\n", returned);
    }
    return 1;
}

int monitor_hwc() {
    int head, tail, usage, exceed, pbsize;
    int unshare_num;
    int count = 0;
    int done;
    int* buf;
    int ret;

#define HWC_WAKE_COND ({ \
        head = atomic_read(&available_queue_head); \
        tail = atomic_read(&available_queue_tail); \
        usage = pfn_range - (tail - head); \
        usage >= pfn_range*81/100 || need_active_unshare == 1; \
    })
    if (epoch_unshare) {
        /*
         * Periodic epoch; pressure or a hint still wake us immediately.
         * While the pool sits above the 81% wake threshold the wait
         * returns at once every time, so the timeout alone would never
         * fire: run the epoch by elapsed time as well, before the
         * pressure logic gets its turn (it re-reads the pool afterwards).
         */
        static unsigned long epoch_last;
        ret = wait_event_interruptible_timeout(scan_wq, HWC_WAKE_COND,
                                               msecs_to_jiffies(epoch_interval_ms));
        if (ret == 0 || time_after_eq(jiffies, epoch_last + msecs_to_jiffies(epoch_interval_ms))) {
            epoch_last = jiffies;
            epoch_unshare_round();
            if (ret == 0)
                return 1;
        }
    } else {
        ret = wait_event_interruptible(scan_wq, HWC_WAKE_COND);
    }
#undef HWC_WAKE_COND

    if (ret < 0) {
        pr_info("wait_event_interruptible error: %d\n", ret);
        return 0;
    }

    shared_page_count_wrapper();

    head = atomic_read(&available_queue_head);
    tail = atomic_read(&available_queue_tail);
    usage = pfn_range - (tail - head);

    // if (usage >= pfn_range*8/10) 
    //     shared_page_count_wrapper();

    if (usage < pfn_range*95/100 && need_active_unshare == 0) {
        return 0;
    }

    if (need_active_unshare == 1 && usage < pfn_range*5/100) {
        need_active_unshare = 0;
        scan_needed = 0;
    }

    pr_info("HWC monitor: usage=%d, head=%d, tail=%d\n", usage, head, tail);
    exceed = usage - pfn_range*8/10;
    if (usage >= pfn_range*81/100) {
        if (eviction_fifo)
            exceed = pop_fifo(exceed);
        else
            pick_eviction_pages(exceed);
    } else
        exceed = active_pick_eviction_pages(usage);
    pr_info("HWC monitor: exceed=%d\n", exceed);

    buf = kvmalloc(sizeof(int)*UNSHARE_BATCH_SIZE, GFP_KERNEL);
    while(exceed > 0) {
        if (!unmap_queue_check()) {
            pr_info("Unmap queue full during hwc monitor, wait\n");
            break;
        }
        unshare_num = (exceed > UNSHARE_BATCH_SIZE) ? UNSHARE_BATCH_SIZE : exceed;
        exceed -= unshare_num;
        /* only PFNs actually unshared may re-enter the free queue */
        done = do_unmap(count, unshare_num, buf, 0);
        if (done <= 0) {
            count += unshare_num;
            continue;
        }
        pool_return_pfns(buf, done);
        // atomic_add(unshare_num, &available_queue_tail);
        // pr_info("hwc monitor unmap: head=%d -> %d. exceed: %d, usage; %d, head: %d, tail: %d, count: %d\n", hoge, hoge + unshare_num, exceed, usage, head, tail, count);
        wake_up_all(&shared_page_wq);
        count += unshare_num;
    }

    pbsize = pbidx;
    count = 0;
    while(pbsize > 0) {
        pr_info("HWC monitor: unmapping from pbsize=%d\n", pbsize);
        if (!unmap_queue_check()) {
            pr_info("Unmap queue full during hwc monitor, wait\n");
            break;
        }
        unshare_num = (pbsize > UNSHARE_BATCH_SIZE) ? UNSHARE_BATCH_SIZE : pbsize;
        pbsize -= unshare_num;
        /* only PFNs actually unshared may re-enter the free queue */
        done = do_unmap(count, unshare_num, buf, 1);
        if (done <= 0) {
            count += unshare_num;
            continue;
        }
        pool_return_pfns(buf, done);
        // atomic_add(unshare_num, &available_queue_tail);
        // pr_info("hwc monitor unmap: head=%d -> %d. exceed: %d, usage; %d, head: %d, tail: %d, count: %d\n", hoge, hoge + unshare_num, exceed, usage, head, tail, count);
        wake_up_all(&shared_page_wq);
        count += unshare_num;
    }
    pbidx = 0;

    kfree(buf);
    
    return 1;
}

// do_unmap to move back pages only to the original provider
// int do_unmap(int head, int unshare_num, int* buf) {
//     int ulist_idx[NODE_NUM] = {0};
//     int currentTail, currentDone;
//     int indexes[NODE_NUM] = {0};
//     struct SyncronousQueue *queue;
//     struct queue_data data = {Q_UNMAP2, .payload = {.unmap2 = {0, 0}}};
//     int ret;
//     int owner = 0;

//     for (int un=0; un<unshare_num; un++) {
//         // struct shared_page_info* spi;
//         unsigned long pfn, unsharing_addr, vpi;
//         uint32_t hash, hash_idx;
//         int new_provider;
//         struct sharing_map_entry* entry;
//         unsigned int current_shareres;

//         // spin_lock(&hwc_lock);
//         // spi = list_first_entry_or_null(&inactive_pages, struct shared_page_info, list);
//         // if (!spi) {
//         //     // pr_err("fetch from active pages during unmap\n");
//         //     spi = list_first_entry_or_null(&active_pages, struct shared_page_info, list);
//         //     list_del(&spi->list);
//         // } else {
//         //     list_del(&spi->list);
//         // }
//         // spin_unlock(&hwc_lock);
//         // pfn = spi->pfn;
//         // // unsharing_addr = rmap_list[get_process_idx(current->tgid)][pfn - shared_page_start];
//         // unsharing_addr = atomic64_read(&rmap_list[pfn - shared_page_start]);
//         // if (unsharing_addr == 0) {
//         //     pr_err("do_unmap: No rmap entry for pfn: %ld\n", pfn);
//         // }
//         // atomic64_set(&rmap_list[pfn - shared_page_start], 0);
//         // pr_info("do_unmap: pfn: %ld, offset: %ld, addr: %lx\n", pfn, pfn-shared_page_start, unsharing_addr);

//         pfn = pfn_buffer[(head + un) % pfn_range] & 0x0FFFFFFFFFFFFFFF; // 4 bits reserved for accessors
//         if (pfn < shared_page_start || pfn >= shared_page_start + pfn_range) {
//             pr_err("do_unmap: Invalid pfn in pfn_buffer: %ld\n", pfn);
//             BUG_ON(1);
//         }

//         unsharing_addr = atomic64_read(&rmap_list[NODE_NUM][pfn - shared_page_start]);
//         if (unsharing_addr == 0) {
//             pr_err("do_unmap: No rmap entry for pfn: %ld\n", pfn);
//             BUG_ON(1);
//         }
        
//         for (int i=0; i<NODE_NUM+1; i++) {
//             atomic64_set(&rmap_list[i][pfn - shared_page_start], 0);
//         }

//         vpi = unsharing_addr >> PAGE_SHIFT;
//         hash = hash_64(vpi, FIREWORK_HASH_BITS);
//         hash_idx = hash % sharing_map_entry_capacity;
//         // new_provider = specify_process_idx(unsharing_addr);
//         new_provider = pfn_buffer[(head + un) % pfn_range] >> 60; // upper 4 bits for accessor idx
//         BUG_ON(new_provider < 0 || new_provider >= NODE_NUM);

//         set_status_flag(unsharing_addr, 0, -1);

//         for (int i=0; i < HASH_SEARCH_LIMIT; i++) {
//             hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
//             spin_lock(&sharing_map_lock[hash_idx]);
//             entry = &sharing_map[hash_idx];
//             if (entry->virtual_page_index == vpi) {
//                 if (entry->status != 4) pr_err("do_unmap: Unsharing unmap on entry with unexpected status: head=%d, addr=%lx, status=%d\n", head, unsharing_addr, entry->status);
//                 current_shareres = entry->sharing_bitmap;
//                 BUG_ON(is_power_of_2(current_shareres));
//                 BUG_ON(entry->pfn != pfn);
//                 buf[un] = entry->pfn;
//                 for (int j=0; j<UNSHARE_QUEUE_NUM; j++) {
//                     if (j == new_provider) {
//                         owner = j;
//                     }
//                     if (1<<j & current_shareres) {
//                         struct unshare_entry* ulist = unshare_list[j];
//                         // if (ulist_idx[j] == 0) {
//                         //     while(ulist[0] != 0) {
//                         //         // pr_info("unshare_list[%d] full, wait\n", j);
//                         //         cond_resched();
//                         //     }
//                         // }
//                         ulist[ulist_idx[j]++] = (struct unshare_entry){unsharing_addr, new_provider, ATOMIC_INIT(hweight32(current_shareres))};
//                     }
//                 }
//                 spin_unlock(&sharing_map_lock[hash_idx]);
//                 break;
//             }
//             spin_unlock(&sharing_map_lock[hash_idx]);
//         }
//     }

//     for (int i=0; i<UNSHARE_QUEUE_NUM; i++) {
//         if (ulist_idx[i] == 0) continue;
//         data.payload.unmap2.unshare_num = ulist_idx[i];
//         if (i == owner) {
//             data.kind = Q_UNSHARE2;
//             data.payload.unmap2.head = head;
//             data.payload.unmap2.unshare_num = unshare_num;
//         }
//         queue = &unshare_queues[i];
//         while(true) {
//             currentTail = __atomic_load_n(queue->tail, __ATOMIC_SEQ_CST);
//             currentDone = __atomic_load_n(queue->done, __ATOMIC_SEQ_CST);

//             if ((currentTail - currentDone) >= queue->capacity) {
//                 pr_err("Unshare queue full, wait: %d\n", i);
//                 sync_queue_update_done(queue);
//                 cond_resched();
//                 continue;
//             }

//             if (__atomic_compare_exchange_n(queue->tail, &currentTail, currentTail + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
//                 indexes[i] = currentTail % queue->capacity;
//                 queue->data[indexes[i]] = data;

//                 if (__atomic_load_n(&queue->flags[indexes[i]], __ATOMIC_SEQ_CST) != 0) {
//                     pr_err("An in-progress event is overwritten\n");
//                     return -1;
//                 }

//                 __atomic_store_n(&queue->flags[indexes[i]], 1, __ATOMIC_SEQ_CST);
//                 // ret = 0;
//                 // do {
//                 //     cond_resched();
//                 //     ret = __atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST);
//                 // } while (ret <= 1);

//                 // __atomic_store_n(&queue->flags[index], 0, __ATOMIC_SEQ_CST);            
//                 break;
//             }
//         }
//     }

//     for (int i=0; i<UNSHARE_QUEUE_NUM; i++) {
//         if (ulist_idx[i] == 0) continue;
//         if (i == owner) continue;
//         queue = &unshare_queues[i];
//         ret = 0;
//         do {
//             cond_resched();
//             ret = __atomic_load_n(&queue->flags[indexes[i]], __ATOMIC_SEQ_CST);
//         } while (ret <= 1);

//         __atomic_store_n(&queue->flags[indexes[i]], 0, __ATOMIC_SEQ_CST);
//     }
//     queue = &unshare_queues[owner];
//     ret = 0;
//     do {
//         cond_resched();
//         ret = __atomic_load_n(&queue->flags[indexes[owner]], __ATOMIC_SEQ_CST);
//     } while (ret <= 1);
//     __atomic_store_n(&queue->flags[indexes[owner]], 0, __ATOMIC_SEQ_CST);

//     return 0;
// }


// do_unmap to move back pages to any node
/*
 * Stages up to unshare_num pages for unsharing. Pages whose status cannot
 * be transitioned 3->4 (a sharing migration is in flight, or the entry was
 * concurrently reset/reused) are skipped: unsharing them anyway would push
 * a still-live PFN back into the free queue (use-after-free).
 *
 * Returns the number of pages actually staged; only buf[0..ret-1] contain
 * valid PFNs that may be returned to the free queue by the caller.
 */
int do_unmap(int head, int unshare_num, int* buf, int option) {
    struct unshare_entry* ulist;
    int ulist_idx[NODE_NUM] = {0};
    int currentTail, currentDone;
    int indexes[NODE_NUM] = {0};
    struct SyncronousQueue *queue;
    struct queue_data data = {Q_UNMAP, .payload = {.unmap2 = {0, 0}}};
    int ret;
    int ok = 0;
    unsigned long* pfns_store = kmalloc_array(unshare_num, sizeof(unsigned long), GFP_KERNEL);

    for (int un=0; un<unshare_num; un++) {
        // struct shared_page_info* spi;
        unsigned long pfn, unsharing_addr, vpi;
        uint32_t hash, hash_idx;
        int new_provider;
        struct sharing_map_entry* entry;
        unsigned int current_shareres;
        bool staged = false;

        if (option==0)
            pfn = pfn_buffer[(head + un) % pfn_range] & 0x0FFFFFFFFFFFFFFF; // 4 bits reserved for accessors
        else
            pfn = pfn_buffer2[(head + un) % pfn_range] & 0x0FFFFFFFFFFFFFFF; // 4 bits reserved for accessors
        if (pfn < shared_page_start || pfn >= shared_page_start + pfn_range) {
            pr_err("do_unmap: Invalid pfn in pfn_buffer: %ld\n", pfn);
            BUG_ON(1);
        }

        unsharing_addr = atomic64_read(&rmap_list[NODE_NUM][pfn - shared_page_start]);
        if (unsharing_addr == 0) {
            /* concurrently unshared/reset since it was picked; skip */
            pr_warn_ratelimited("do_unmap: no rmap entry for pfn %ld, skipping\n", pfn);
            continue;
        }

        vpi = unsharing_addr >> PAGE_SHIFT;
        hash = hash_64(vpi, FIREWORK_HASH_BITS);
        hash_idx = hash % sharing_map_entry_capacity;
        if (option==0)
            new_provider = pfn_buffer[(head + un) % pfn_range] >> 60; // upper 4 bits for accessor idx
        else
            new_provider = pfn_buffer2[(head + un) % pfn_range] >> 60; // upper 4 bits for accessor idx
        // new_provider = pfn_buffer[(head + un) % pfn_range] >> 60; // upper 4 bits for accessor idx
        BUG_ON(new_provider < 0 || new_provider >= NODE_NUM);
        /*
         * Destination of the unshared page. The candidate pickers encode the
         * page's single recent accessor in the top 4 bits of pfn_buffer;
         * with unshare_to_accessor set we honor it (move the page to the node
         * that is now using it, as in the paper). Otherwise the page goes
         * back to the process that allocated it (its address partition).
         */
        if (!unshare_to_accessor)
            new_provider = specify_process_idx(unsharing_addr);
        else if (new_provider != specify_process_idx(unsharing_addr))
            unshare_to_accessor_count++;

        /*
         * 3->4 transition. Failure means the page is not in the steady
         * shared state (a migration is in flight or the entry is gone);
         * unsharing it now would free a PFN that is still in use.
         */
        if (set_status_flag(unsharing_addr, 0, -1) != 0) {
            pr_warn_ratelimited("do_unmap: cannot start unshare for addr %lx, skipping\n", unsharing_addr);
            continue;
        }

        // for (int i=0; i<NODE_NUM+1; i++) {
        //     atomic64_set(&rmap_list[i][pfn - shared_page_start], 0);
        // }

        for (int i=0; i < HASH_SEARCH_LIMIT; i++) {
            hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
            spin_lock(&sharing_map_lock[hash_idx]);
            entry = &sharing_map[hash_idx];
            if (entry->virtual_page_index == vpi) {
                if (entry->status != 4) pr_err("do_unmap: Unsharing unmap on entry with unexpected status: head=%d, addr=%lx, status=%d\n", head, unsharing_addr, entry->status);
                current_shareres = entry->sharing_bitmap;
                BUG_ON(is_power_of_2(current_shareres));
                BUG_ON(entry->pfn != pfn);
                buf[ok] = entry->pfn;
                pfns_store[ok] = pfn;

                ulist = unshare_list[0];
                ulist[ok] = (struct unshare_entry){unsharing_addr, new_provider, ATOMIC_INIT(current_shareres)};

                for (int j=0; j<UNSHARE_QUEUE_NUM; j++) {
                    if (1<<j & current_shareres)
                        ulist_idx[j]++;
                }
                staged = true;
                spin_unlock(&sharing_map_lock[hash_idx]);
                break;
            }
            spin_unlock(&sharing_map_lock[hash_idx]);
        }
        if (staged)
            ok++;
        else
            pr_warn_ratelimited("do_unmap: sharing map entry vanished for addr %lx, skipping\n", unsharing_addr);
        // pr_info("do_unmap: addr: %lx, pfn: %ld, new_provider: %d, current_shareres: %x\n", unsharing_addr, pfn, new_provider, current_shareres);
    }

    if (ok == 0) {
        kfree(pfns_store);
        return 0;
    }

    for (int i=0; i<UNSHARE_QUEUE_NUM; i++) {
        if (ulist_idx[i] == 0) continue;
        data.payload.unmap2.unshare_num = ok;
        queue = &unshare_queues[i];
        while(true) {
            currentTail = __atomic_load_n(queue->tail, __ATOMIC_SEQ_CST);
            currentDone = __atomic_load_n(queue->done, __ATOMIC_SEQ_CST);

            if ((currentTail - currentDone) >= queue->capacity) {
                pr_err("Unshare queue full, wait: %d\n", i);
                sync_queue_update_done(queue);
                cond_resched();
                continue;
            }

            if (__atomic_compare_exchange_n(queue->tail, &currentTail, currentTail + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                indexes[i] = currentTail % queue->capacity;
                queue->data[indexes[i]] = data;

                if (__atomic_load_n(&queue->flags[indexes[i]], __ATOMIC_SEQ_CST) != 0) {
                    pr_err("An in-progress event is overwritten\n");
                    return -1;
                }

                __atomic_store_n(&queue->flags[indexes[i]], 1, __ATOMIC_SEQ_CST);
                break;
            }
        }
    }

    for (int i=0; i<UNSHARE_QUEUE_NUM; i++) {
        if (ulist_idx[i] == 0) continue;
        queue = &unshare_queues[i];
        ret = 0;
        do {
            cond_resched();
            ret = __atomic_load_n(&queue->flags[indexes[i]], __ATOMIC_SEQ_CST);
        } while (ret <= 1);

        __atomic_store_n(&queue->flags[indexes[i]], 0, __ATOMIC_SEQ_CST);
    }

    for (int i=0; i<ok; i++) {
        unsigned long pfn = pfns_store[i];
        for (int j=0; j<NODE_NUM+1; j++) {
            atomic64_set(&rmap_list[j][pfn - shared_page_start], 0);
        }
    }
    kfree(pfns_store);

    return ok;
}

unsigned long check_rmap_addr(unsigned long pfn) {
    unsigned long addr = atomic64_read(&rmap_list[NODE_NUM][pfn - shared_page_start]);
    if (addr == 0) {
        pr_err("check_rmap_addr: No rmap entry for pfn: %ld\n", pfn);
        BUG_ON(1);
    }
    return addr;
}

int check_ongoing_unshare() {
    int ret = atomic_read(&unshare_count);
    return ret;
}

int unmap_queue_check() {
    struct SyncronousQueue *queue;
    int available_min;
    int currentTail, currentDone;
    queue=&unshare_queues[0];
    available_min = queue->capacity;
    for (int i=0; i<UNSHARE_QUEUE_NUM; i++) {
        queue = &unshare_queues[i];
        currentTail = __atomic_load_n(queue->tail, __ATOMIC_SEQ_CST);
        currentDone = __atomic_load_n(queue->done, __ATOMIC_SEQ_CST);
        available_min = (queue->capacity - (currentTail - currentDone) < available_min) ? (queue->capacity - (currentTail - currentDone)) : available_min;
    }
    return available_min;
}


int unmap_queue_produce(unsigned long addr) {
    int currentTail, currentDone, index;
    uint8_t ret;
    int new_provider = specify_process_idx(addr);
    struct queue_data data = {Q_UNMAP, .payload = {.unshare = {addr, new_provider}}};
    struct SyncronousQueue *queue;
    // Only used to ensure sync flag is clear after migration is done
    // TODO: Shi: May be removed later
    // uint32_t sharing_map_idx = find_sharing_map_idx(addr);

    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry* entry;
    unsigned int current_shareres;

    for (int i=0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        spin_lock(&sharing_map_lock[hash_idx]);
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi) {
            if (entry->status != 4) pr_err("Unsharing unmap on entry with unexpected status: %d\n", entry->status);
            current_shareres = entry->sharing_bitmap;
            for (int j=0; j<UNSHARE_QUEUE_NUM; j++) {
                if (j == new_provider) continue;
                if (1<<j & current_shareres) {
                    queue = &unshare_queues[j];

                    while(true) {
                        currentTail = __atomic_load_n(queue->tail, __ATOMIC_SEQ_CST);
                        currentDone = __atomic_load_n(queue->done, __ATOMIC_SEQ_CST);

                        if ((currentTail - currentDone) >= queue->capacity) {
                            pr_err("Unshare queue full, wait: %d\n", j);
                            sync_queue_update_done(queue);
                            cond_resched();
                            continue;
                        }

                        if (__atomic_compare_exchange_n(queue->tail, &currentTail, currentTail + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                            index = currentTail % queue->capacity;
                            queue->data[index] = data;

                            if (__atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST) != 0) {
                                pr_err("An in-progress event is overwritten\n");
                                spin_unlock(&sharing_map_lock[hash_idx]);
                                return -1;
                            }

                            __atomic_store_n(&queue->flags[index], 1, __ATOMIC_SEQ_CST);
                            // ret = 0;
                            // do {
                            //     cond_resched();
                            //     ret = __atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST);
                            // } while (ret <= 1);

                            // __atomic_store_n(&queue->flags[index], 0, __ATOMIC_SEQ_CST);            
                            break;
                        }
                    }
                }
            }
        }
        spin_unlock(&sharing_map_lock[hash_idx]);
    }
    return ret;
}

void finish_unshare() {
    atomic_sub(1, &unshare_count);
}

int unshare_send(unsigned long addr) {
    int owner = specify_process_idx(addr);
    struct queue_data newreq = {Q_UNSHARE, .payload = {.unshare = {addr, owner}}};
    struct SyncronousQueue *queue = &unshare_queues[owner];
    int currentTail, currentDone, index;
    int ret;
    
    while(true) {
        currentTail = __atomic_load_n(queue->tail, __ATOMIC_SEQ_CST);
        currentDone = __atomic_load_n(queue->done, __ATOMIC_SEQ_CST);

        if ((currentTail - currentDone) >= queue->capacity) {
            sync_queue_update_done(queue);
            cond_resched();
            continue;
        }

        if (__atomic_compare_exchange_n(queue->tail, &currentTail, currentTail + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            index = currentTail % queue->capacity;
            queue->data[index] = newreq;

            if (__atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST) != 0) {
                pr_err("An in-progress event is overwritten\n");
                return -1;
            }

            __atomic_store_n(&queue->flags[index], 1, __ATOMIC_SEQ_CST);
            ret = 0;
            do {
                cond_resched();
                ret = __atomic_load_n(&queue->flags[index], __ATOMIC_SEQ_CST);
            } while (ret <= 1);

            __atomic_store_n(&queue->flags[index], 0, __ATOMIC_SEQ_CST);            
            break;
        }
    }
    return ret;
}

void add_done_unmap(int num) {
    atomic_add(num, &unmap_done);
}

void clear_done_unmap() {
    atomic_set(&unmap_done, 0);
}

int check_done_unmap() {
    return atomic_read(&unmap_done);
}

void scan_shared_pages(void) {
    struct shared_page_info* current_spi;
    struct shared_page_info* next_spi;
    int cur_idx = get_process_idx(current->tgid);
    if (cur_idx == -1) return;
    spin_lock(&hwc_lock);
    list_for_each_entry_safe(current_spi, next_spi, &inactive_pages, list) {
        unsigned long pfn = current_spi->pfn;
        unsigned long vm_flags = 0;
        struct folio* folio = page_folio(pfn_to_page(pfn));
        struct mem_cgroup* mcg = folio_memcg(folio);
        int res = folio_referenced(folio, 0, mcg, &vm_flags);
        // unsigned long addr = atomic64_read(&rmap_list[current_spi->pfn - shared_page_start]);
        // unsigned long pfn = current_spi->pfn;
        // int res = firework_check_pte(addr, pfn);
        // pr_info("Scan inactive page %lu: referenced=%d, offset: %lu by %d\n", pfn, res, pfn - shared_page_start, cur_idx);
        if (res==0) continue;
        list_move_tail(&current_spi->list, &active_pages);
    }
    // pr_info("Active pages scanned. Now scanning inactive pages...\n");

    list_for_each_entry_safe(current_spi, next_spi, &active_pages, list) {
        unsigned long pfn = current_spi->pfn;
        unsigned long vm_flags = 0;
        struct folio* folio = page_folio(pfn_to_page(pfn));
        struct mem_cgroup* mcg = folio_memcg(folio);
        int res = folio_referenced(folio, 0, mcg, &vm_flags);
        // unsigned long addr = atomic64_read(&rmap_list[current_spi->pfn - shared_page_start]);
        // unsigned long pfn = current_spi->pfn;
        // int res = firework_check_pte(addr, pfn);
        // pr_info("Scan active page %lu: referenced=%d, offset: %lu by %d\n", pfn, res, pfn - shared_page_start, cur_idx);
        // if (res==0) {
        //     if (current_spi->yellow & (1<<cur_idx)) {
        //         current_spi->yellow &= ~(1<<cur_idx);
        //         list_move_tail(&current_spi->list, &inactive_pages);
        //     } else {
        //         current_spi->yellow |= (1<<cur_idx);
        //     }
        // }
        if (res==0) {
            if (current_spi->yellow) {
                current_spi->yellow = 0;
                list_move_tail(&current_spi->list, &inactive_pages);
            } else {
                current_spi->yellow = 1;
            }
        }
    }
    spin_unlock(&hwc_lock);
}

void shared_page_count_wrapper(void) {
    if (start_flag)
        scan_request();
}

void shared_page_count(void) {
    // int debug, res;
    // for (unsigned long i = 0; i < pfn_range; i++) {
    //     unsigned long pfn = shared_page_start + i;
    //     struct folio* folio = page_folio(pfn_to_page(pfn));
    //     struct mem_cgroup* mcg = folio_memcg(folio);
    //     unsigned long vm_flags = 0;
    //     unsigned long addr = rmap_list[i];
    //     unsigned long aligned_addr = addr & PAGE_MASK;
    //     if (addr == 0) continue;
    //     debug = (aligned_addr == 0x312dcd79000) ? 1 : 0;
    //     res = firework_folio_referenced(folio, 0, mcg, &vm_flags, debug, addr);
    //     // res = folio_referenced(folio, 0, mcg, &vm_flags);
    //     if (res) {
    //         atomic_add(1, &pg_access_count[i]);
    //     }
    // }
    int res = 0;
    int proc_idx = get_process_idx(current->tgid);
    BUG_ON(proc_idx < 0 || proc_idx >= NODE_NUM);

    for (unsigned long i = 0; i < pfn_range; i++) {
        unsigned long addr = atomic64_read(&rmap_list[proc_idx][i]);
        unsigned long pfn = shared_page_start + i;
        if (addr == 0) continue;
        if (atomic64_read(&rmap_list[NODE_NUM][i]) != addr) {
            pr_info_ratelimited("shared_page_count: addr mismatch at idx %lu: rmap_list[%d]=%llx, rmap_list[%d]=%llx\n", i, proc_idx, atomic64_read(&rmap_list[proc_idx][i]), NODE_NUM, atomic64_read(&rmap_list[NODE_NUM][i]));
            /* transient during a pfn's recycle; skip it this epoch instead of dying */
            continue;
        }
        
        res = firework_check_pte(addr, pfn);
        // atomic_add(res, &pg_access_count[i]);
        if (res) {
            atomic_add(res, &pg_access_info_list[i].count);
            atomic_or(1<<proc_idx, &pg_access_info_list[i].accessors);
        }
    }
}

// You must set migration batch size to 1 for now because of rmap_list problem
/*
 * FIFO eviction (Figure 8d baseline): walk the pool slots in order from a
 * persistent hand and take the first `num` mapped pages, ignoring access
 * history. Pool slots are handed out in order, so slot order approximates
 * allocation order. Returns the number picked (fewer than `num` if the
 * pool has fewer mapped pages); pages go back to their allocator.
 */
int pop_fifo(int num) {
    long head = atomic_fetch_add(num, &index_fifo);
    int accessor;
    unsigned long addr;
    int idx = 0;

    for (unsigned long i = 0; i < pfn_range && idx < num; i++) {
        unsigned long slot = (head + i) % pfn_range;
        addr = atomic64_read(&rmap_list[NODE_NUM][slot]);
        if (addr == 0)
            continue;
        accessor = specify_process_idx(addr);
        if (accessor < 0 || accessor >= NODE_NUM)
            continue;
        pfn_buffer[idx++] = (shared_page_start + slot) | ((unsigned long)accessor << 60);
    }
    return idx;
}

// Just for micro benchmarking!!
void do_unsharing_all() {
    int page_num = atomic_read(&addr_buffer_idx);
    int count = 0;
    int* buf = kvmalloc(sizeof(int)*UNSHARE_BATCH_SIZE, GFP_KERNEL);
    int accessor;
    int idx = 0;
    int sum = 0;
    BUG_ON(atomic_read(&addr_buffer_idx) == 0);
    while(sum < page_num) {
        unsigned long addr = atomic64_read(&rmap_list[NODE_NUM][idx++]);
        if (addr==0)
            continue;
        accessor = specify_process_idx(addr);
        BUG_ON(accessor < 0 || accessor >= NODE_NUM);
        pfn_buffer[sum] = ((shared_page_start + idx - 1) & 0x0FFFFFFFFFFFFFFF) | ((unsigned long)accessor << 60);
        sum++;
    }

    pr_info("do_unsharing_all: unsharing %d pages out of %d\n", sum, page_num);
    while (page_num > 0) {
        int bs = tmp_unshare_batch_size ? (int)tmp_unshare_batch_size : UNSHARE_BATCH_SIZE;
        int unshare_num;
        if (bs > UNSHARE_BATCH_SIZE) bs = UNSHARE_BATCH_SIZE;
        unshare_num = (page_num > bs) ? bs : page_num;
        page_num -= unshare_num;
        do_unmap(count, unshare_num, buf, 0);
        count += unshare_num;
    }
    kvfree(buf);
}

void pick_eviction_pages(int num) {
    int cur_ref = 0;
    int picked = 0;
    unsigned long start_pos = get_random_u32();
    int accessors = 0;
    int accessor = 0;
    int cur_count = 0;
    while (1) {
        for (unsigned long i = 0; i < pfn_range; i++) {
            unsigned long idx = (i + start_pos) % pfn_range;
            unsigned long addr = atomic64_read(&rmap_list[NODE_NUM][idx]);
            if (addr == 0) {
                /*
                 * No consistency check against rmap_list[0] here: the
                 * per-node entry is written before the global one by
                 * rmap_list_update2 and cleared before it by do_unmap, so a
                 * lockless reader legitimately sees them disagree. The
                 * BUG_ON that used to sit here killed the unsharing
                 * manager mid-run (kernel BUG at ...:2674), after which
                 * nothing ever reclaimed the pool and the app hung.
                 */
                continue;
            }
            // if (atomic_read(&pg_access_count[idx]) == cur_ref) {
            accessors = atomic_read(&pg_access_info_list[idx].accessors);    
            cur_count = atomic_read(&pg_access_info_list[idx].count);
            // if (cur_count >= 5 && is_power_of_2(accessors)) {
            //     pfn_buffer2[pbidx] = shared_page_start + idx;
            //     accessor = ffs(accessors) - 1;
            //     pfn_buffer2[pbidx] |= ((unsigned long)accessor << 60);
            //     pbidx++;
            // }

            if (cur_count == cur_ref) {
                // if (cur_ref >= 5) {
                //     if (is_power_of_2(accessors)) {
                //         continue;
                //     }
                // }
                pfn_buffer[picked] = shared_page_start + idx;
                // accessors = atomic_read(&pg_access_info_list[idx].accessors);
                if (is_power_of_2(accessors)) {
                    accessor = ffs(accessors) - 1;
                } else {
                    accessor = specify_process_idx(addr);
                }
                BUG_ON(accessor < 0 || accessor >= NODE_NUM);
                pfn_buffer[picked] |= ((unsigned long)accessor << 60);
                picked++;
                if (picked >= num) {
                    break;
                }
            }
        }
        if (picked >= num) {
            break;
        }
        cur_ref++;
    }

    // analyze();
    for (unsigned long i = 0; i < pfn_range; i++) {
        // atomic_set(&pg_access_count[i], 0);
        atomic_set(&pg_access_info_list[i].count, 0);
        atomic_set(&pg_access_info_list[i].accessors, 0);
    }
}

int active_pick_eviction_pages(int num) {
    int picked = 0;
    unsigned long start_pos = get_random_u32();
    int accessors = 0;
    int accessor = 0;
    int cur_count = 0;

    // memset(pfn_buffer, 0, sizeof(unsigned long) * pfn_range);

    for (unsigned long i = 0; i < pfn_range; i++) {
        unsigned long idx = (i + start_pos) % pfn_range;
        unsigned long addr = atomic64_read(&rmap_list[NODE_NUM][idx]);
        if (addr == 0)
            continue;   /* see pick_eviction_pages: lockless, may lag rmap_list[0] */
        accessors = atomic_read(&pg_access_info_list[idx].accessors);    
        cur_count = atomic_read(&pg_access_info_list[idx].count);
        // if (cur_count >= 1)
            // pr_info("active_pick_eviction_pages: addr=%lx, count=%d, accessors=%d\n", addr, cur_count, accessors);
        if (cur_count >= 1 && is_power_of_2(accessors)) {
            pfn_buffer[picked] = shared_page_start + idx;
            accessor = ffs(accessors) - 1;
            pfn_buffer[picked] |= ((unsigned long)accessor << 60);
            picked++;
            if (picked >= num) {
                break;
            }
        }
    }

    for (unsigned long i = 0; i < pfn_range; i++) {
        atomic_set(&pg_access_info_list[i].count, 0);
        atomic_set(&pg_access_info_list[i].accessors, 0);
    }

    return picked;
}

void analyze() {
    int max = 0;
    int max2 = 0;
    int max3 = 0;
    int max_idx = -1;
    int max2_idx = -1;
    int max3_idx = -1;

    int count0 = 0;
    int count1 = 0;
    int count2 = 0;
    int count3 = 0;
    int count4 = 0;

    int ex_count = 0;

    unsigned long buf = 0x312dcd79000;

    for (unsigned long i = 0; i < pfn_range; i++) {
        // unsigned long v = atomic_read(&pg_access_count[i]);
        unsigned long v = atomic_read(&pg_access_info_list[i].count);
        
        unsigned long addr = atomic64_read(&rmap_list[NODE_NUM][i]);
        if (addr>=buf && addr < buf+0x1000*(64*1024))
            ex_count++;

        if (addr == buf) {
            pr_info("analyze: addr: %llx, count: %ld\n", atomic64_read(&rmap_list[0][i]), v);
        }

        if (v == 0) count0++;
        else if (v < 4) count1++;
        else if (v < 8) count2++;
        else if (v < 16) count3++;
        else count4++;

        if (v > max) {
            // shift down
            max3 = max2; max3_idx = max2_idx;
            max2 = max;  max2_idx = max_idx;
            max  = v;    max_idx  = i;
        } else if (v > max2) {
            // shift down
            max3 = max2; max3_idx = max2_idx;
            max2 = v;    max2_idx = i;
        } else if (v > max3) {
            max3 = v;    max3_idx = i;
        }
    }
    pr_info("ex_count in range: %d\n", ex_count);

    pr_info("max_idx: %d, max2_idx: %d, max3_idx: %d\n", max_idx, max2_idx, max3_idx);
    pr_info("addr1: %llx (%d), addr2: %llx (%d), addr3: %llx (%d)\n", atomic64_read(&rmap_list[NODE_NUM][max_idx]), max, atomic64_read(&rmap_list[NODE_NUM][max2_idx]), max2, atomic64_read(&rmap_list[NODE_NUM][max3_idx]), max3);
    pr_info("Access distribution: 0:%d, 1~3:%d, 4~7:%d, 8~15:%d, 16+:%d\n", count0, count1, count2, count3, count4);
}

int firework_check_pte(unsigned long addr, unsigned long pfn) {
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pmd_t pmde;
    pte_t *pte;
    spinlock_t *ptl;
    int ref = 0;
    int process_idx = get_process_idx(current->tgid);


    mmap_read_lock(mm);
    vma = find_vma(mm, addr);
    if (!vma || addr < vma->vm_start || addr >= vma->vm_end || mm != vma->vm_mm) {
        pr_err("No VMA found for addr: %lx\n", addr);
        WARN_ONCE(1, "No VMA found for addr: %lx\n", addr);
        goto out;
    }

    pgd = pgd_offset(mm, addr);
    if (!pgd_present(*pgd)) {
        pr_info("PGD not present for addr: %lx\n", addr);
        goto out;
    }

    p4d = p4d_offset(pgd, addr);
    if (!p4d_present(*p4d)) {
        pr_info("P4D not present for addr: %lx\n", addr);
        goto out;
    }

    pud = pud_offset(p4d, addr);
    if (!pud_present(*pud)) {
        pr_info("PUD not present for addr: %lx\n", addr);   
        goto out;
    }

    pmd = pmd_offset(pud, addr);
    pmde = READ_ONCE(*pmd);

    if (!pmd_present(pmde)) {
        // find_shareres(addr);
        pr_info_ratelimited("PMD not present for addr: %lx\n", addr);
        goto out;
    }

    if (pmd_trans_huge(pmde)) {
        pr_info("Huge page detected for addr: %lx\n", addr);
        goto out;
    }

    pte = pte_offset_map_lock(mm, pmd, addr, &ptl);

    if (!pte) {
        pr_info_ratelimited("No PTE found for addr: %lx\n", addr);
        goto out_unlock;
    }

    if (!pte_present(*pte)) {
        pr_info_ratelimited("PTE not present for addr by %d: %lx\n", get_process_idx(current->tgid), addr);
        goto out_unlock;
    }

    if (pte_pfn(*pte) != pfn) {
        pr_info_ratelimited("PFN mismatch for addr: %lx, expected: %ld, actual: %ld. proc: %d\n", addr, pfn, pte_pfn(*pte), process_idx);
        BUG_ON(1);
        goto out_unlock;
    }

    if (ptep_clear_flush_young(vma, addr, pte))
        ref = 1;

out_unlock:
    pte_unmap_unlock(pte, ptl);

out:
    mmap_read_unlock(mm);
    return ref;
}

int firework_check_pfn(unsigned long addr, unsigned long pfn) {
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pmd_t pmde;
    pte_t *pte;
    spinlock_t *ptl;


    mmap_read_lock(mm);
    vma = find_vma(mm, addr);
    if (!vma || addr < vma->vm_start || addr >= vma->vm_end || mm != vma->vm_mm) {
        pr_err("No VMA found for addr: %lx\n", addr);
        WARN_ONCE(1, "No VMA found for addr: %lx\n", addr);
        goto out;
    }

    pgd = pgd_offset(mm, addr);
    if (!pgd_present(*pgd)) {
        pr_info("PGD not present for addr: %lx\n", addr);
        goto out;
    }

    p4d = p4d_offset(pgd, addr);
    if (!p4d_present(*p4d)) {
        pr_info("P4D not present for addr: %lx\n", addr);
        goto out;
    }

    pud = pud_offset(p4d, addr);
    if (!pud_present(*pud)) {
        pr_info("PUD not present for addr: %lx\n", addr);   
        goto out;
    }

    pmd = pmd_offset(pud, addr);
    pmde = READ_ONCE(*pmd);

    if (!pmd_present(pmde)) {
        // find_shareres(addr);
        pr_info("PMD not present for addr: %lx\n", addr);
        goto out;
    }

    if (pmd_trans_huge(pmde)) {
        pr_info("Huge page detected for addr: %lx\n", addr);
        goto out;
    }

    pte = pte_offset_map_lock(mm, pmd, addr, &ptl);

    if (!pte) {
        pr_info("No PTE found for addr: %lx\n", addr);
        goto out_unlock;
    }

    if (!pte_present(*pte)) {
        pr_info("PTE not present for addr by %d: %lx\n", get_process_idx(current->tgid), addr);
        goto out_unlock;
    }

    if (pfn == 0) {
        pr_info("mapping: addr: %lx, actual pfn: %ld. pmd: %lx (%p), pte: %lx by %d\n", addr, pte_pfn(*pte), pmd_val(*pmd), pmd, pte_val(*pte), get_process_idx(current->tgid));
        goto out_unlock;
    }

    if (pte_pfn(*pte) != pfn) {
        pr_info("PFN mismatch for addr: %lx, expected: %ld, actual: %ld\n", addr, pfn, pte_pfn(*pte));
        goto out_unlock;
    } else {
        pr_info("PFN match for addr: %lx, pfn: %ld\n", addr, pfn);
    }

out_unlock:
    pte_unmap_unlock(pte, ptl);

out:
    mmap_read_unlock(mm);
    return 0;
}

void rmap_list_update(unsigned long addr, unsigned long pfn) {
    int process_idx = get_process_idx(current->tgid);

    unsigned long addr1 = atomic64_xchg(&rmap_list[process_idx][pfn - shared_page_start], addr);
    unsigned long addr2 = atomic64_xchg(&rmap_list[NODE_NUM][pfn - shared_page_start], addr);

    if (addr1) 
        BUG_ON(addr1 != addr);

    if (addr2)
        BUG_ON(addr2 != addr);

    if (addr1 && addr2)
        BUG_ON(addr1 != addr2);
}

void rmap_list_update2(unsigned long addr, unsigned long pfn, int src_node) {
    unsigned long addr1, addr2, addr3;
    int process_idx = get_process_idx(current->tgid);

    if (src_node == -2) {
        addr1 = atomic64_xchg(&rmap_list[process_idx][pfn - shared_page_start], addr);
        if (addr1) {
            if (addr1 != addr) {
                pr_info("1. rmap_list_update error: addr1=%lx, addr=%lx, pfn=%ld, process_idx=%d\n", addr1, addr, pfn, process_idx);
                BUG_ON(1);
            }
        }
        // firework_check_pfn(addr, pfn);
        // pr_info("update2: addr: %lx, pfn: %ld, process_idx: %d\n", addr, pfn, process_idx);
    } else {
        BUG_ON(src_node < 0 || src_node >= NODE_NUM);
        if (src_node == process_idx) {
            pr_info("rmap_list_update2 error: src_node == process_idx (%d)\n", process_idx);
            pr_info("addr: %lx, pfn: %ld\n", addr, pfn);
            BUG_ON(1);
        }

        addr1 = atomic64_xchg(&rmap_list[process_idx][pfn - shared_page_start], addr);
        addr2 = atomic64_xchg(&rmap_list[NODE_NUM][pfn - shared_page_start], addr);
        addr3 = atomic64_xchg(&rmap_list[src_node][pfn - shared_page_start], addr);

        if (addr1 || addr2 || addr3) {
            pr_info("2. rmap_list_update error: addr1=%lx, addr2=%lx, addr3=%lx, addr=%lx, pfn=%ld, process_idx=%d, src_node=%d\n", addr1, addr2, addr3, addr, pfn, process_idx, src_node);
            BUG_ON(1);
        }
        // firework_check_pfn(addr, pfn);
        // pr_info("update: addr: %lx, pfn: %ld, process_idx: %d, src_node: %d\n", addr, pfn, process_idx, src_node);
    }
}

void find_shareres(unsigned long addr) {
    unsigned long vpi = addr >> PAGE_SHIFT;
    uint32_t hash = hash_64(vpi, FIREWORK_HASH_BITS);
    uint32_t hash_idx = hash % sharing_map_entry_capacity;
    struct sharing_map_entry* entry;
    unsigned int current_shareres;

    for (int i=0; i < HASH_SEARCH_LIMIT; i++) {
        hash_idx = (hash_idx + 1) % sharing_map_entry_capacity;
        spin_lock(&sharing_map_lock[hash_idx]);
        entry = &sharing_map[hash_idx];
        if (entry->virtual_page_index == vpi) {
            current_shareres = entry->sharing_bitmap;
            pr_info_ratelimited("find_shareres: addr: %lx, pfn: %ld, status: %d, shareres: %u\n", addr, entry->pfn, entry->status, current_shareres);
            spin_unlock(&sharing_map_lock[hash_idx]);
            return;
        }
        spin_unlock(&sharing_map_lock[hash_idx]);
    }
    pr_err("No sharing map entry found for addr: %lx\n", addr);
}

int scan_request() {
    int currentTail, currentDone;
    int indexes[NODE_NUM] = {0};
    uint8_t ret;
    struct queue_data data;
    struct SyncronousQueue *queue;
    data.kind = Q_SCAN;
    
    
    for (int i=0; i<firework_num_nodes; i++) {
        queue = &scan_queues[i];

        while(true) {
            currentTail = __atomic_load_n(queue->tail, __ATOMIC_SEQ_CST);
            currentDone = __atomic_load_n(queue->done, __ATOMIC_SEQ_CST);

            if ((currentTail - currentDone) >= queue->capacity) {
                sync_queue_update_done(queue);
                cond_resched();
                continue;
            }

            if (__atomic_compare_exchange_n(queue->tail, &currentTail, currentTail + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                indexes[i] = currentTail % queue->capacity;
                queue->data[indexes[i]] = data;

                if (__atomic_load_n(&queue->flags[indexes[i]], __ATOMIC_SEQ_CST) != 0) {
                    pr_err("An in-progress event is overwritten\n");
                    return -1;
                }

                __atomic_store_n(&queue->flags[indexes[i]], 1, __ATOMIC_SEQ_CST);          
                break;
            }
        }
    }

    for (int i=0; i<firework_num_nodes; i++) {
        /*
         * A node that has already exited (program_end is set by the
         * finishing rank, the others are then killed) will never answer;
         * without a bail-out this thread would spin in the kernel forever
         * and the killed process could not be reaped.
         */
        unsigned long deadline = jiffies + 30 * HZ;
        queue = &scan_queues[i];
        ret = 0;
        for (;;) {
            cond_resched();
            ret = __atomic_load_n(&queue->flags[indexes[i]], __ATOMIC_SEQ_CST);
            if (ret > 1)
                break;
            if (program_end || fatal_signal_pending(current) || time_after(jiffies, deadline)) {
                pr_warn_ratelimited("scan_request: giving up on node %d (program_end=%u)\n", i, program_end);
                return -1;
            }
        }
        __atomic_store_n(&queue->flags[indexes[i]], 0, __ATOMIC_SEQ_CST);
    }

    return 0;
}

void enable_unsharing() {
    scan_needed = 1;
    need_active_unshare = 1;
    wake_up_all(&scan_wq);
}

void free_pfn(unsigned long pfn) {
    int p = (int)pfn;

    pool_return_pfns(&p, 1);
}