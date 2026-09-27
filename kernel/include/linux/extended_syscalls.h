#ifndef _LINUX_EXTENDED_SYSCALLS_H
#define _LINUX_EXTENDED_SYSCALLS_H

#include <linux/net.h>

int read_sockinfo_from_debugfs(struct sockaddr_in *server_addr);

asmlinkage long sys_sharing_request(void);
asmlinkage long sys_shared_memory_init(size_t sharing_map_size);
asmlinkage long sys_shared_memory_put(int host_idx, uint64_t addr, unsigned long pfn);
asmlinkage long sys_shared_memory_get(int host_idx, uint64_t addr);
asmlinkage long sys_shared_memory_get_or_create(uint64_t addr);
asmlinkage long sys_shared_memory_exit(void);
asmlinkage long sys_firework_nw_init(void);

#endif // _LINUX_EXTENDED_SYSCALLS_H