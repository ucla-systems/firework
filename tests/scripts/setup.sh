#!/usr/bin/bash
# This script set up the OS environment for Firework

# grant permission to the debugfs
sudo chmod 777 -R /sys/kernel/debug/firework

echo 2 | sudo tee /sys/kernel/debug/firework/hint_zone_mode
# 4GB
echo 4294967296 | sudo tee /sys/kernel/debug/firework/fast_tier_size
# 2GB
#echo 2147483648 | sudo tee /sys/kernel/debug/firework/fast_tier_size

# Unsharing destination: 0 = back to the allocating process (default),
# 1 = to the page's single recent accessor. eval.sh enables it for CF.
echo 0 | sudo tee /sys/kernel/debug/firework/unshare_to_accessor

# Epoch-based automatic unsharing (paper Sec. 4.4): 0 = off (unsharing only
# under pool pressure or an explicit hint), 1 = the unsharing manager scans
# accessed bits every epoch_interval_ms and unshares single-accessor pages.
# Off by default; eval.sh enables it per application. Kernels without the
# knob simply ignore these lines.
echo 0    | sudo tee /sys/kernel/debug/firework/epoch_unshare 2>/dev/null
echo 1000 | sudo tee /sys/kernel/debug/firework/epoch_interval_ms 2>/dev/null
echo 256  | sudo tee /sys/kernel/debug/firework/epoch_min_pages 2>/dev/null

# Figure 8 knobs: eviction policy (0 = Firework's clock-style pick, 1 = FIFO
# baseline) and the batch size used by the unshare-all microbenchmark.
echo 0   | sudo tee /sys/kernel/debug/firework/eviction_fifo 2>/dev/null
echo 128 | sudo tee /sys/kernel/debug/firework/tmp_unshare_batch_size 2>/dev/null

# Default migration batch size; eval.sh overrides this per application
# (e.g. FG=1). See the tuning table in README.md.
echo 8 | sudo tee /sys/kernel/debug/firework/migration_batch_size

# echo 128 | sudo tee /sys/kernel/debug/firework/tmp_unshare_batch_size

# echo 1073741824 | sudo tee /sys/kernel/debug/firework/hwc_size # 1GB
echo 536870912 | sudo tee /sys/kernel/debug/firework/hwc_size # 512MB
# echo 805306368 | sudo tee /sys/kernel/debug/firework/hwc_size # 768MB
# echo 268435456 | sudo tee /sys/kernel/debug/firework/hwc_size # 256MB
# echo 3221225472 | sudo tee /sys/kernel/debug/firework/hwc_size # 3GB

# Set up the dax device
# The dax device start and end address is updated during each dax device setup
echo 0 | sudo tee /sys/kernel/debug/firework/dax_id
sudo ndctl disable-namespace namespace0.0
sudo ndctl create-namespace --reconfig=namespace0.0 --mode=devdax --map=mem  --align=4K
echo 1 | sudo tee /sys/kernel/debug/firework/dax_id
sudo ndctl disable-namespace namespace1.0
sudo ndctl create-namespace --reconfig=namespace1.0 --mode=devdax --map=mem  --align=4K

# The unified address space requires overcommiting memory in glibc mmap
sudo sysctl vm.overcommit_memory=1
