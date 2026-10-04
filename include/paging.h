#ifndef ROCKOS_PAGING_H
#define ROCKOS_PAGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGING_KERNEL_MAP_LIMIT 0x40000000ULL
#define PAGING_PAGE_TABLE_ENTRIES 512U
#define PAGING_PAGE_TABLE_FLAGS 0x003ULL
#define PAGING_LARGE_PAGE_FLAGS 0x083ULL
#define PAGING_PAGE_SIZE_FLAG 0x080ULL
#define PAGING_PAGE_PRESENT 0x001ULL
#define PAGING_PAGE_WRITABLE 0x002ULL
#define PAGING_PAGE_USER 0x004ULL

uintptr_t paging_address_space_create(void);
bool paging_address_space_destroy(uintptr_t pml4_address);
bool paging_map_user_page(uintptr_t pml4_address, uintptr_t virtual_address,
    uintptr_t physical_address, bool writable);
bool paging_map_kernel_mmio(uintptr_t physical_address, size_t byte_count,
    uintptr_t* virtual_address);
bool paging_unmap_user_page(uintptr_t pml4_address, uintptr_t virtual_address,
    uintptr_t* physical_address);
void paging_init(void);
void paging_load_address_space(uintptr_t pml4_address);
uintptr_t paging_current_address_space(void);
uintptr_t paging_kernel_address_space(void);
void paging_restore_kernel_address_space(void);

#endif