#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/firework.h>
#include <linux/atomic.h>

uint32_t firework_default32 = 0;
uint16_t firework_default16 = 0;
uint32_t firework_managed_process_pid_0 = 1000000;
uint32_t firework_managed_process_pid_1 = 1000000;
uint32_t firework_managed_process_pid_2 = 1000000;
uint32_t firework_managed_process_pid_3 = 1000000;
uint32_t firework_managed_process_pid_4 = 1000000;
uint32_t firework_managed_process_pid_5 = 1000000;
uint32_t firework_managed_process_pid_6 = 1000000;
uint32_t firework_managed_process_pid_7 = 1000000;
uint8_t firework_hint_zone_mode = 0; // 0: plain, 1: interleaved, 2: tiered
uint64_t firework_fast_tier_size_byte = 0; // This is a byte value, only read with tiered mode
uint8_t firework_dax_id = 0;
uint64_t dax_range_start_page = 0x0ul;
uint64_t dax_range_end_page = 0x0ul;
uint64_t dax_range_start_page_2 = 0x0ul;
uint64_t dax_range_end_page_2 = 0x0ul;
atomic_t pf_counter = ATOMIC_INIT(0);
atomic_t mig_counter1 = ATOMIC_INIT(0);
atomic_t mig_counter2 = ATOMIC_INIT(0);
atomic_t unshare_counter = ATOMIC_INIT(0);
atomic_t tmp1 = ATOMIC_INIT(0);
atomic_t tmp2 = ATOMIC_INIT(0);
uint8_t migration_batch_size = 1;
uint64_t hwc_size = 0;
uint32_t firework_num_nodes = 0;
uint32_t program_end = 0;
/* 1: unshare candidates go to their single recent accessor (paper §4.4);
 * 0 (default): back to the process that allocated them. debugfs knob. */
uint32_t unshare_to_accessor = 0;
/* pages unshared to a process other than their allocator (only with the
 * policy above); lets an experiment confirm the policy actually engaged. */
uint32_t unshare_to_accessor_count = 0;
uint32_t scan_needed = 0;
/* Epoch-based automatic unsharing (paper §4.4). 0 (default): unsharing is
 * driven only by pool pressure or the application's explicit hint syscall.
 * 1: the unsharing manager additionally wakes every epoch_interval_ms,
 * scans the accessed bits on every node and unshares pages whose recent
 * accessor set (two most recent epochs) is a single node. */
uint32_t epoch_unshare = 0;
uint32_t epoch_interval_ms = 1000;
/* minimum candidate count for a round to be worth an unmap protocol run */
uint32_t epoch_min_pages = 256;
/* pages unshared by epoch rounds / rounds that actually unshared */
uint32_t epoch_unshare_count = 0;
uint32_t epoch_round_count = 0;
/* 1: the pressure-driven eviction picks pages in pool-slot (allocation)
 * order instead of the clock-style cold-first policy; the FIFO baseline of
 * the paper's eviction-policy comparison (Figure 8d). debugfs knob. */
uint32_t eviction_fifo = 0;
uint32_t tmp_unshare_batch_size = 0;
// uint32_t firework_migration_count
// A safe guard to prevent the firework path taken by pid 0
// Only enable after setting up the pid and index
// Also disable once the firework process stops

static inline void firework_debugfs_init(void)
{
    struct dentry *dir = debugfs_create_dir("firework", NULL);
    if (!dir) {
        pr_err("Failed to create firework debugfs directory\n");
        return;
    }

    debugfs_create_u32("ipaddr", 0666, dir, &firework_default32);
    debugfs_create_u16("port", 0666, dir, &firework_default16);
    debugfs_create_u32("pid_0", 0666, dir, &firework_managed_process_pid_0);
    debugfs_create_u32("pid_1", 0666, dir, &firework_managed_process_pid_1);
    debugfs_create_u32("pid_2", 0666, dir, &firework_managed_process_pid_2);
    debugfs_create_u32("pid_3", 0666, dir, &firework_managed_process_pid_3);
    debugfs_create_u32("pid_4", 0666, dir, &firework_managed_process_pid_4);
    debugfs_create_u32("pid_5", 0666, dir, &firework_managed_process_pid_5);
    debugfs_create_u32("pid_6", 0666, dir, &firework_managed_process_pid_6);
    debugfs_create_u32("pid_7", 0666, dir, &firework_managed_process_pid_7);
    debugfs_create_u8("hint_zone_mode", 0666, dir, &firework_hint_zone_mode);
    debugfs_create_u64("fast_tier_size", 0666, dir, &firework_fast_tier_size_byte);
    debugfs_create_u8("dax_id", 0666, dir, &firework_dax_id);
    debugfs_create_u64("dax0_start", 0666, dir, &dax_range_start_page);
    debugfs_create_u64("dax0_end", 0666, dir, &dax_range_end_page);
    debugfs_create_u64("dax1_start", 0666, dir, &dax_range_start_page_2);
    debugfs_create_u64("dax1_end", 0666, dir, &dax_range_end_page_2);
    debugfs_create_atomic_t("pf_counter", 0666, dir, &pf_counter);
    debugfs_create_atomic_t("mig_counter1", 0666, dir, &mig_counter1);
    debugfs_create_atomic_t("mig_counter2", 0666, dir, &mig_counter2);
    debugfs_create_atomic_t("unshare_counter", 0666, dir, &unshare_counter);
    debugfs_create_u8("migration_batch_size", 0666, dir, &migration_batch_size);
    debugfs_create_u64("hwc_size", 0666, dir, &hwc_size);
    debugfs_create_atomic_t("tmp1", 0666, dir, &tmp1);
    debugfs_create_atomic_t("tmp2", 0666, dir, &tmp2);
    debugfs_create_u32("num_nodes", 0666, dir, &firework_num_nodes);
    debugfs_create_u32("program_end", 0666, dir, &program_end);
    debugfs_create_u32("unshare_to_accessor", 0666, dir, &unshare_to_accessor);
    debugfs_create_u32("unshare_to_accessor_count", 0666, dir, &unshare_to_accessor_count);
    debugfs_create_u32("scan_needed", 0666, dir, &scan_needed);
    debugfs_create_u32("epoch_unshare", 0666, dir, &epoch_unshare);
    debugfs_create_u32("epoch_interval_ms", 0666, dir, &epoch_interval_ms);
    debugfs_create_u32("epoch_min_pages", 0666, dir, &epoch_min_pages);
    debugfs_create_u32("epoch_unshare_count", 0666, dir, &epoch_unshare_count);
    debugfs_create_u32("epoch_round_count", 0666, dir, &epoch_round_count);
    debugfs_create_u32("eviction_fifo", 0666, dir, &eviction_fifo);
    debugfs_create_u32("tmp_unshare_batch_size", 0666, dir, &tmp_unshare_batch_size);
}

int __init firework_init(void)
{
    firework_debugfs_init();
    return 0;
}
__initcall(firework_init);