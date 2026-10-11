/*
 * src/arch/i386/paging32.c
 *
 * 32-bit (non-PAE) paging for the i386 RockOS build. Same public API as
 * src/kernel/paging.c (x86-64, 4-level), so the rest of the kernel does not
 * care which one it links.
 *
 * Layout of every address space:
 *   0x00000000 - 0x3FFFFFFF  identity map, supervisor, 4 MiB large pages
 *                            (needs CR4.PSE, enabled in boot32.S)
 *   0x40000000 - 0xBFFFFFFF  free for user mappings (4 KiB pages)
 *   0xE0000000 - 0xFFFFFFFF  kernel MMIO window (framebuffer, PCI BARs);
 *                            page tables shared with the kernel directory
 *
 * The "pml4_address" parameters in paging.h are simply the page-directory
 * physical address here.
 */

#include "paging.h"

#include "pmm.h"

#include <stddef.h>
#include <stdint.h>

#define P32_PRESENT      0x001U
#define P32_WRITABLE     0x002U
#define P32_USER         0x004U
#define P32_PWT          0x008U
#define P32_PCD          0x010U
#define P32_LARGE        0x080U
#define P32_ADDR_MASK    0xFFFFF000U
#define P32_LARGE_FLAGS  (P32_PRESENT | P32_WRITABLE | P32_LARGE)
#define P32_TABLE_FLAGS  (P32_PRESENT | P32_WRITABLE)

#define P32_ENTRIES         1024U
#define P32_KERNEL_LARGE    256U            /* 256 x 4 MiB = 1 GiB */
#define P32_USER_LIMIT      0xC0000000U
#define P32_MMIO_BASE       0xE0000000U
#define P32_MMIO_FIRST_PDE  (P32_MMIO_BASE >> 22)

static uintptr_t kernel_address_space;
static uint32_t kernel_mmio_next_virtual = P32_MMIO_BASE;

static void clear_table(uint32_t* table) {
    for (size_t index = 0; index < P32_ENTRIES; index++) {
        table[index] = 0;
    }
}

static bool table_is_empty(const uint32_t* table) {
    for (size_t index = 0; index < P32_ENTRIES; index++) {
        if (table[index] & P32_PRESENT) return false;
    }
    return true;
}

static bool valid_page_address(uintptr_t address) {
    return (address & (PMM_PAGE_SIZE - 1)) == 0;
}

static inline void invalidate_page(uintptr_t address) {
    asm volatile("invlpg (%0)" : : "r"(address) : "memory");
}

uintptr_t paging_current_address_space(void) {
    uintptr_t address;
    asm volatile("mov %%cr3, %0" : "=r"(address));
    return address;
}

void paging_init(void) {
    if (!kernel_address_space) {
        kernel_address_space = paging_current_address_space() & P32_ADDR_MASK;
    }
}

uintptr_t paging_address_space_create(void) {
    uint32_t* pd = (uint32_t*)pmm_alloc(1);
    if (!pd) return 0;

    paging_init();
    clear_table(pd);
    for (uint32_t index = 0; index < P32_KERNEL_LARGE; index++) {
        pd[index] = (index << 22) | P32_LARGE_FLAGS;
    }

    /* Share the kernel MMIO window (framebuffer etc.) with every address
     * space so drawing keeps working if a task with a user directory is
     * running when the scheduler switches to the UI loop. */
    if (kernel_address_space) {
        const uint32_t* kernel_pd = (const uint32_t*)kernel_address_space;
        for (uint32_t index = P32_MMIO_FIRST_PDE; index < P32_ENTRIES; index++) {
            pd[index] = kernel_pd[index];
        }
    }
    return (uintptr_t)pd;
}

bool paging_map_user_page(uintptr_t pd_address, uintptr_t virtual_address,
    uintptr_t physical_address, bool writable) {
    if (!pd_address || !valid_page_address(pd_address) ||
        !valid_page_address(virtual_address) ||
        !valid_page_address(physical_address) ||
        physical_address >= PMM_IDENTITY_MAP_LIMIT ||
        virtual_address >= P32_USER_LIMIT) {
        return false;
    }

    uint32_t* pd = (uint32_t*)pd_address;
    size_t pd_index = virtual_address >> 22;
    size_t pt_index = (virtual_address >> 12) & 0x3FF;
    uint32_t* pt;
    bool allocated_pt = false;

    if (!(pd[pd_index] & P32_PRESENT)) {
        pt = (uint32_t*)pmm_alloc(1);
        if (!pt) return false;
        clear_table(pt);
        pd[pd_index] = (uint32_t)(uintptr_t)pt |
            P32_PRESENT | P32_WRITABLE | P32_USER;
        allocated_pt = true;
    } else {
        if (pd[pd_index] & P32_LARGE) return false;
        pt = (uint32_t*)(uintptr_t)(pd[pd_index] & P32_ADDR_MASK);
    }

    if (pt[pt_index] & P32_PRESENT) {
        if (allocated_pt) {
            pd[pd_index] = 0;
            pmm_free(pt, 1);
        }
        return false;
    }

    pd[pd_index] |= P32_USER | P32_WRITABLE;
    pt[pt_index] = (uint32_t)physical_address | P32_PRESENT | P32_USER |
        (writable ? P32_WRITABLE : 0U);
    return true;
}

bool paging_unmap_user_page(uintptr_t pd_address, uintptr_t virtual_address,
    uintptr_t* physical_address) {
    if (!pd_address || !valid_page_address(pd_address) ||
        !valid_page_address(virtual_address) ||
        virtual_address >= P32_USER_LIMIT || !physical_address) {
        return false;
    }

    uint32_t* pd = (uint32_t*)pd_address;
    size_t pd_index = virtual_address >> 22;
    size_t pt_index = (virtual_address >> 12) & 0x3FF;
    if (!(pd[pd_index] & P32_PRESENT) || (pd[pd_index] & P32_LARGE)) {
        return false;
    }

    uint32_t* pt = (uint32_t*)(uintptr_t)(pd[pd_index] & P32_ADDR_MASK);
    uint32_t entry = pt[pt_index];
    if (!(entry & P32_PRESENT) || !(entry & P32_USER)) {
        return false;
    }

    *physical_address = (uintptr_t)(entry & P32_ADDR_MASK);
    pt[pt_index] = 0;
    invalidate_page(virtual_address);
    if (table_is_empty(pt)) {
        pd[pd_index] = 0;
        if (!pmm_free(pt, 1)) return false;
    }
    return true;
}

bool paging_address_space_destroy(uintptr_t pd_address) {
    if (!pd_address || (pd_address & (PMM_PAGE_SIZE - 1)) != 0) {
        return false;
    }

    uint32_t* pd = (uint32_t*)pd_address;
    if ((pd[0] & P32_LARGE_FLAGS) != P32_LARGE_FLAGS) return false;
    /* User page tables are freed by paging_unmap_user_page; the shared
     * kernel MMIO tables must NOT be freed here. */
    return pmm_free(pd, 1);
}

void paging_load_address_space(uintptr_t pd_address) {
    if (!pd_address || (pd_address & (PMM_PAGE_SIZE - 1)) != 0) return;
    asm volatile("mov %0, %%cr3" : : "r"(pd_address) : "memory");
}

bool paging_map_kernel_mmio(uintptr_t physical_address, size_t byte_count,
    uintptr_t* virtual_address) {
    if (!physical_address || !byte_count || !virtual_address) return false;

    uint32_t page_offset = (uint32_t)(physical_address & (PMM_PAGE_SIZE - 1));
    uint32_t physical_page = (uint32_t)physical_address & P32_ADDR_MASK;
    uint64_t span = (uint64_t)page_offset + byte_count;
    uint64_t page_count = (span + (PMM_PAGE_SIZE - 1)) >> 12;
    uint64_t map_size = page_count << 12;

    if ((uint64_t)physical_page + map_size > 0x100000000ULL) return false;
    if ((uint64_t)kernel_mmio_next_virtual + map_size > 0x100000000ULL) {
        return false;
    }

    paging_init();
    uint32_t* pd = (uint32_t*)kernel_address_space;
    uint32_t map_start = kernel_mmio_next_virtual;

    for (uint64_t page = 0; page < page_count; page++) {
        uint32_t va = map_start + (uint32_t)(page << 12);
        size_t pd_index = va >> 22;
        size_t pt_index = (va >> 12) & 0x3FF;
        uint32_t* pt;

        if (!(pd[pd_index] & P32_PRESENT)) {
            pt = (uint32_t*)pmm_alloc(1);
            if (!pt) return false;
            clear_table(pt);
            pd[pd_index] = (uint32_t)(uintptr_t)pt | P32_TABLE_FLAGS;
        } else {
            if (pd[pd_index] & P32_LARGE) return false;
            pt = (uint32_t*)(uintptr_t)(pd[pd_index] & P32_ADDR_MASK);
        }
        if (pt[pt_index] & P32_PRESENT) return false;

        /* Uncached (PWT|PCD), same as the x86-64 mapping. */
        pt[pt_index] = (physical_page + (uint32_t)(page << 12)) |
            P32_PRESENT | P32_WRITABLE | P32_PWT | P32_PCD;
        invalidate_page(va);
    }

    kernel_mmio_next_virtual += (uint32_t)map_size;
    *virtual_address = (uintptr_t)(map_start + page_offset);
    return true;
}

uintptr_t paging_kernel_address_space(void) {
    paging_init();
    return kernel_address_space;
}

void paging_restore_kernel_address_space(void) {
    paging_load_address_space(paging_kernel_address_space());
}
