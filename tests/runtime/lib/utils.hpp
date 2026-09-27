#pragma once
#include <stddef.h>

// The start address of the shared memory
// Used to find start address of queues
// Initialized in firework_init
extern void *sh_queues_start;
extern void* sh_lock_start;
extern size_t queue_size_bytes;
