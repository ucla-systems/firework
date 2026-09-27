#include <stddef.h>

void *sh_queues_start;
void *sh_lock_start;
size_t queue_size_bytes = 64 * 1024; // 64KB = 2000 32-byte elements + 64 bytes metadata
