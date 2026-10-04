#ifndef ROCKOS_PMM_H
#define ROCKOS_PMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PMM_PAGE_SIZE 4096ULL
#define PMM_IDENTITY_MAP_LIMIT 0x40000000ULL

typedef struct {
    uint64_t total_bytes;    /* Sum of all Multiboot memory-map regions. */
    uint64_t usable_bytes;   /* Sum of regions marked available by Multiboot. */
    uint64_t managed_bytes;  /* Fully usable frames below the 1 GiB map limit. */
    uint64_t free_bytes;     /* Managed frames currently available to pmm_alloc. */
    uint64_t used_bytes;     /* Managed frames currently returned by pmm_alloc. */
    /* total - free - used; includes firmware-reserved and unmanaged memory. */
    uint64_t reserved_bytes;
} pmm_stats_t;

bool pmm_init(uint32_t boot_magic, uintptr_t boot_info_address);
/* Allocations are identity-mapped physical addresses below 1 GiB. */
void* pmm_alloc(size_t page_count);
bool pmm_free(void* address, size_t page_count);
bool pmm_is_free(uintptr_t physical_address, size_t page_count);
void pmm_get_stats(pmm_stats_t* stats);

#endif