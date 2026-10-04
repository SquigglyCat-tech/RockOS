#include "pmm.h"

#include "io.h"
#include "multiboot1.h"

#include <limits.h>
#include <stddef.h>

#define PMM_FRAME_COUNT (PMM_IDENTITY_MAP_LIMIT / PMM_PAGE_SIZE)
#define PMM_BITMAP_SIZE (PMM_FRAME_COUNT / 8)
#define PMM_MAX_BOOT_STRING 65536U

_Static_assert(offsetof(multiboot1_info_t, framebuffer_addr) == 88,
    "Unexpected Multiboot1 info layout");
_Static_assert(sizeof(multiboot1_mmap_entry_t) == 24,
    "Unexpected Multiboot1 memory-map entry layout");

static uint8_t frame_bitmap[PMM_BITMAP_SIZE] __attribute__((aligned(8)));
static uint8_t allocated_bitmap[PMM_BITMAP_SIZE] __attribute__((aligned(8)));
static uint64_t managed_frame_count;
static uint64_t free_frame_count;
static uint64_t used_frame_count;
static uint64_t total_memory_bytes;
static uint64_t usable_memory_bytes;
static bool initialized;

extern char __kernel_start[];
extern char __kernel_end[];

static bool range_is_mapped(uint64_t start, uint64_t length) {
    return start < PMM_IDENTITY_MAP_LIMIT &&
        length <= PMM_IDENTITY_MAP_LIMIT - start;
}

static uint64_t align_up_page(uint64_t address) {
    return (address + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
}

static bool bitmap_test(const uint8_t* bitmap, uint64_t frame) {
    return (bitmap[frame >> 3] & (uint8_t)(1U << (frame & 7))) != 0;
}

static void bitmap_set(uint8_t* bitmap, uint64_t frame) {
    bitmap[frame >> 3] |= (uint8_t)(1U << (frame & 7));
}

static void bitmap_clear(uint8_t* bitmap, uint64_t frame) {
    bitmap[frame >> 3] &= (uint8_t)~(1U << (frame & 7));
}

static bool validate_memory_map(const multiboot1_info_t* info) {
    if (!(info->flags & MULTIBOOT1_INFO_MMAP) || info->mmap_length == 0 ||
        !range_is_mapped(info->mmap_addr, info->mmap_length)) {
        return false;
    }

    uint64_t offset = 0;
    while (offset < info->mmap_length) {
        if (info->mmap_length - offset < sizeof(uint32_t)) {
            return false;
        }
        const multiboot1_mmap_entry_t* entry =
            (const multiboot1_mmap_entry_t*)(uintptr_t)(info->mmap_addr + offset);
        uint64_t entry_size = (uint64_t)entry->size + sizeof(entry->size);
        if (entry->size < sizeof(*entry) - sizeof(entry->size) ||
            entry_size > info->mmap_length - offset ||
            entry->length > UINT64_MAX - entry->base_addr) {
            return false;
        }
        offset += entry_size;
    }
    return offset == info->mmap_length;
}

static void reserve_range(uint64_t start, uint64_t end) {
    if (end <= start || start >= PMM_IDENTITY_MAP_LIMIT) {
        return;
    }
    if (end > PMM_IDENTITY_MAP_LIMIT) {
        end = PMM_IDENTITY_MAP_LIMIT;
    }

    uint64_t first_frame = start / PMM_PAGE_SIZE;
    uint64_t end_frame = align_up_page(end) / PMM_PAGE_SIZE;
    for (uint64_t frame = first_frame; frame < end_frame; frame++) {
        if (!bitmap_test(frame_bitmap, frame)) {
            bitmap_set(frame_bitmap, frame);
            free_frame_count--;
        }
    }
}

static bool reserve_string(uint32_t address) {
    if (address == 0) {
        return true;
    }
    if (address >= PMM_IDENTITY_MAP_LIMIT) {
        return false;
    }

    const char* string = (const char*)(uintptr_t)address;
    for (uint32_t length = 0; length < PMM_MAX_BOOT_STRING; length++) {
        if ((uint64_t)address + length >= PMM_IDENTITY_MAP_LIMIT) {
            return false;
        }
        if (string[length] == '\0') {
            reserve_range(address, (uint64_t)address + length + 1);
            return true;
        }
    }
    return false;
}

static bool reserve_boot_data(const multiboot1_info_t* info,
    uintptr_t info_address) {
    reserve_range(info_address, (uint64_t)info_address +
        offsetof(multiboot1_info_t, framebuffer_addr));
    reserve_range(info->mmap_addr, (uint64_t)info->mmap_addr + info->mmap_length);

    if ((info->flags & MULTIBOOT1_INFO_CMDLINE) && !reserve_string(info->cmdline)) {
        return false;
    }
    if ((info->flags & MULTIBOOT1_INFO_BOOT_LOADER) &&
        !reserve_string(info->boot_loader_name)) {
        return false;
    }

    if (info->flags & MULTIBOOT1_INFO_MODULES) {
        uint64_t module_bytes = (uint64_t)info->mods_count * sizeof(multiboot1_module_t);
        if (module_bytes > UINT32_MAX ||
            !range_is_mapped(info->mods_addr, module_bytes)) {
            return false;
        }
        reserve_range(info->mods_addr, (uint64_t)info->mods_addr + module_bytes);
        const multiboot1_module_t* modules =
            (const multiboot1_module_t*)(uintptr_t)info->mods_addr;
        for (uint32_t i = 0; i < info->mods_count; i++) {
            if (modules[i].mod_end < modules[i].mod_start) {
                return false;
            }
            reserve_range(modules[i].mod_start, modules[i].mod_end);
            if (!reserve_string(modules[i].string)) {
                return false;
            }
        }
    }

    if (info->flags & MULTIBOOT1_INFO_DRIVES) {
        if (!range_is_mapped(info->drives_addr, info->drives_length)) {
            return false;
        }
        reserve_range(info->drives_addr,
            (uint64_t)info->drives_addr + info->drives_length);
    }

    if (info->flags & MULTIBOOT1_INFO_APM) {
        reserve_range(info->apm_table, (uint64_t)info->apm_table + 20);
    }

    if (info->flags & MULTIBOOT1_INFO_AOUT) {
        uint64_t symbol_bytes = (uint64_t)info->symbols[0] + info->symbols[1];
        if (symbol_bytes > UINT32_MAX ||
            !range_is_mapped(info->symbols[2], symbol_bytes)) {
            return false;
        }
        reserve_range(info->symbols[2], (uint64_t)info->symbols[2] + symbol_bytes);
    } else if (info->flags & MULTIBOOT1_INFO_ELF) {
        uint64_t section_bytes = (uint64_t)info->symbols[0] * info->symbols[1];
        if (section_bytes > UINT32_MAX ||
            !range_is_mapped(info->symbols[2], section_bytes)) {
            return false;
        }
        reserve_range(info->symbols[2], (uint64_t)info->symbols[2] + section_bytes);
    }

    if (info->flags & MULTIBOOT1_INFO_VBE) {
        reserve_range(info->vbe_control_info,
            (uint64_t)info->vbe_control_info + 512);
        reserve_range(info->vbe_mode_info,
            (uint64_t)info->vbe_mode_info + 256);
    }

    if (info->flags & MULTIBOOT1_INFO_FRAMEBUFFER) {
        if (!range_is_mapped(info_address, sizeof(*info))) {
            return false;
        }
        reserve_range(info_address, (uint64_t)info_address + sizeof(*info));
        uint64_t framebuffer_bytes =
            (uint64_t)info->framebuffer_pitch * info->framebuffer_height;
        if (info->framebuffer_addr > UINT64_MAX - framebuffer_bytes) {
            return false;
        }
        reserve_range(info->framebuffer_addr,
            info->framebuffer_addr + framebuffer_bytes);
    }
    return true;
}

bool pmm_init(uint32_t boot_magic, uintptr_t boot_info_address) {
    uint64_t irq_flags = save_irq_disable();
    if (initialized || boot_magic != MULTIBOOT1_BOOTLOADER_MAGIC ||
        !range_is_mapped(boot_info_address,
            offsetof(multiboot1_info_t, framebuffer_addr))) {
        restore_irq(irq_flags);
        return false;
    }

    const multiboot1_info_t* info = (const multiboot1_info_t*)boot_info_address;
    if (!validate_memory_map(info)) {
        restore_irq(irq_flags);
        return false;
    }

    for (uint64_t i = 0; i < PMM_BITMAP_SIZE; i++) {
        frame_bitmap[i] = 0xFF;
        allocated_bitmap[i] = 0;
    }
    managed_frame_count = 0;
    free_frame_count = 0;
    used_frame_count = 0;
    total_memory_bytes = 0;
    usable_memory_bytes = 0;

    uint64_t offset = 0;
    while (offset < info->mmap_length) {
        const multiboot1_mmap_entry_t* entry =
            (const multiboot1_mmap_entry_t*)(uintptr_t)(info->mmap_addr + offset);
        uint64_t entry_size = (uint64_t)entry->size + sizeof(entry->size);
        if (entry->length > UINT64_MAX - total_memory_bytes) {
            restore_irq(irq_flags);
            return false;
        }
        total_memory_bytes += entry->length;

        if (entry->type == MULTIBOOT1_MMAP_AVAILABLE) {
            if (entry->length > UINT64_MAX - usable_memory_bytes) {
                restore_irq(irq_flags);
                return false;
            }
            usable_memory_bytes += entry->length;

            uint64_t region_end = entry->base_addr + entry->length;
            if (entry->base_addr < PMM_IDENTITY_MAP_LIMIT) {
                uint64_t start = align_up_page(entry->base_addr);
                uint64_t end = region_end & ~(PMM_PAGE_SIZE - 1);
                if (end > PMM_IDENTITY_MAP_LIMIT) {
                    end = PMM_IDENTITY_MAP_LIMIT;
                }
                if (start < PMM_IDENTITY_MAP_LIMIT && end > start) {
                    for (uint64_t address = start; address < end;
                         address += PMM_PAGE_SIZE) {
                        uint64_t frame = address / PMM_PAGE_SIZE;
                        if (bitmap_test(frame_bitmap, frame)) {
                            bitmap_clear(frame_bitmap, frame);
                            managed_frame_count++;
                            free_frame_count++;
                        }
                    }
                }
            }
        }
        offset += entry_size;
    }

    reserve_range(0, 0x100000);
    reserve_range((uintptr_t)__kernel_start, (uintptr_t)__kernel_end);
    if (!reserve_boot_data(info, boot_info_address)) {
        restore_irq(irq_flags);
        return false;
    }

    if (managed_frame_count == 0) {
        restore_irq(irq_flags);
        return false;
    }
    initialized = true;
    restore_irq(irq_flags);
    return true;
}

void* pmm_alloc(size_t page_count) {
    if (page_count == 0) {
        return NULL;
    }

    uint64_t irq_flags = save_irq_disable();
    if (!initialized || page_count > free_frame_count) {
        restore_irq(irq_flags);
        return NULL;
    }

    uint64_t run_start = 0;
    size_t run_length = 0;
    for (uint64_t frame = 0; frame < PMM_FRAME_COUNT; frame++) {
        if (bitmap_test(frame_bitmap, frame)) {
            run_length = 0;
            continue;
        }
        if (run_length == 0) {
            run_start = frame;
        }
        run_length++;
        if (run_length == page_count) {
            for (uint64_t allocated = run_start;
                 allocated < run_start + page_count; allocated++) {
                bitmap_set(frame_bitmap, allocated);
                bitmap_set(allocated_bitmap, allocated);
            }
            free_frame_count -= page_count;
            used_frame_count += page_count;
            restore_irq(irq_flags);
            return (void*)(uintptr_t)(run_start * PMM_PAGE_SIZE);
        }
    }

    restore_irq(irq_flags);
    return NULL;
}

bool pmm_free(void* address, size_t page_count) {
    uintptr_t physical_address = (uintptr_t)address;
    if (!address || page_count == 0 ||
        (physical_address & (PMM_PAGE_SIZE - 1)) != 0 ||
        physical_address >= PMM_IDENTITY_MAP_LIMIT ||
        page_count > (PMM_IDENTITY_MAP_LIMIT - physical_address) / PMM_PAGE_SIZE) {
        return false;
    }

    uint64_t irq_flags = save_irq_disable();
    uint64_t first_frame = physical_address / PMM_PAGE_SIZE;
    for (uint64_t i = 0; i < page_count; i++) {
        if (!initialized || !bitmap_test(allocated_bitmap, first_frame + i)) {
            restore_irq(irq_flags);
            return false;
        }
    }
    for (uint64_t i = 0; i < page_count; i++) {
        bitmap_clear(allocated_bitmap, first_frame + i);
        bitmap_clear(frame_bitmap, first_frame + i);
    }
    free_frame_count += page_count;
    used_frame_count -= page_count;
    restore_irq(irq_flags);
    return true;
}

bool pmm_is_free(uintptr_t physical_address, size_t page_count) {
    if (page_count == 0 ||
        (physical_address & (PMM_PAGE_SIZE - 1)) != 0 ||
        physical_address >= PMM_IDENTITY_MAP_LIMIT ||
        page_count > (PMM_IDENTITY_MAP_LIMIT - physical_address) / PMM_PAGE_SIZE) {
        return false;
    }

    uint64_t irq_flags = save_irq_disable();
    uint64_t first_frame = physical_address / PMM_PAGE_SIZE;
    for (uint64_t i = 0; i < page_count; i++) {
        if (!initialized || bitmap_test(frame_bitmap, first_frame + i)) {
            restore_irq(irq_flags);
            return false;
        }
    }
    restore_irq(irq_flags);
    return true;
}

void pmm_get_stats(pmm_stats_t* stats) {
    if (!stats) {
        return;
    }

    uint64_t irq_flags = save_irq_disable();
    stats->total_bytes = total_memory_bytes;
    stats->usable_bytes = usable_memory_bytes;
    stats->managed_bytes = managed_frame_count * PMM_PAGE_SIZE;
    stats->free_bytes = free_frame_count * PMM_PAGE_SIZE;
    stats->used_bytes = used_frame_count * PMM_PAGE_SIZE;
    uint64_t accounted_bytes = stats->free_bytes + stats->used_bytes;
    stats->reserved_bytes = total_memory_bytes >= accounted_bytes
        ? total_memory_bytes - accounted_bytes : 0;
    restore_irq(irq_flags);
}