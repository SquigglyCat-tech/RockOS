#include "gdt.h"

#include <stdbool.h>
#include <stddef.h>

#define GDT_ENTRY_COUNT 7U
#define GDT_TSS_SELECTOR 0x28U

#ifdef ROCKOS_ARCH_32
typedef struct __attribute__((packed)) {
    uint16_t link;
    uint16_t reserved0;
    uint32_t esp0;
    uint16_t ss0;
    uint16_t reserved1;
    uint32_t esp1;
    uint16_t ss1;
    uint16_t reserved2;
    uint32_t esp2;
    uint16_t ss2;
    uint16_t reserved3;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint16_t es;
    uint16_t reserved4;
    uint16_t cs;
    uint16_t reserved5;
    uint16_t ss;
    uint16_t reserved6;
    uint16_t ds;
    uint16_t reserved7;
    uint16_t fs;
    uint16_t reserved8;
    uint16_t gs;
    uint16_t reserved9;
    uint16_t ldt_selector;
    uint16_t reserved10;
    uint16_t debug_trap;
    uint16_t iomap_base;
} tss32_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} gdt_pointer_t;

_Static_assert(sizeof(tss32_t) == 104, "Unexpected x86 TSS layout");

static uint64_t gdt[GDT_ENTRY_COUNT] __attribute__((aligned(16)));
static tss32_t tss;
static bool gdt_ready;

extern void gdt_load(const gdt_pointer_t* pointer);

static void set_tss_descriptor(uintptr_t base, uint32_t limit) {
    uint64_t low = (limit & 0xFFFFULL) |
        ((uint64_t)(base & 0xFFFFFFULL) << 16) |
        (0x89ULL << 40) |
        ((uint64_t)(limit & 0xF0000ULL) << 32) |
        ((uint64_t)(base & 0xFF000000ULL) << 32);

    gdt[5] = low;
    /* 32-bit TSS descriptor is a single 8-byte entry. */
    gdt[6] = 0;
}

static void set_segment_descriptor(int index, uint32_t base, uint32_t limit,
    uint8_t access, uint8_t granularity) {
    gdt[index] = ((uint64_t)(limit & 0xFFFFULL)) |
        ((uint64_t)(base & 0xFFFFFFULL) << 16) |
        ((uint64_t)access << 40) |
        ((uint64_t)(limit & 0xF0000ULL) << 32) |
        ((uint64_t)(granularity & 0xF0ULL) << 48) |
        ((uint64_t)((base >> 24) & 0xFFULL) << 56);
}

bool gdt_set_kernel_stack(uintptr_t kernel_stack_top) {
    if (!gdt_ready || kernel_stack_top == 0) return false;
    tss.esp0 = (uint32_t)kernel_stack_top;
    return true;
}

bool gdt_init(uintptr_t kernel_stack_top) {
    if (kernel_stack_top == 0) return false;

    gdt[0] = 0;
    /* 32-bit flat segments: base 0, limit 4GiB, G=1, D/B=1. */
    set_segment_descriptor(1, 0, 0xFFFFF, 0x9A, 0xCF); /* kernel code 0x08 */
    set_segment_descriptor(2, 0, 0xFFFFF, 0x92, 0xCF); /* kernel data 0x10 */
    set_segment_descriptor(3, 0, 0xFFFFF, 0xFA, 0xCF); /* user code   0x1B */
    set_segment_descriptor(4, 0, 0xFFFFF, 0xF2, 0xCF); /* user data   0x23 */

    tss = (tss32_t){0};
    tss.ss0 = 0x10;
    tss.esp0 = (uint32_t)kernel_stack_top;
    tss.iomap_base = sizeof(tss32_t);
    set_tss_descriptor((uintptr_t)&tss, sizeof(tss32_t) - 1);

    gdt_pointer_t pointer = {
        .limit = sizeof(gdt) - 1,
        .base = (uint32_t)(uintptr_t)gdt
    };
    gdt_load(&pointer);
    gdt_ready = true;
    return true;
}
#else
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss64_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_pointer_t;

_Static_assert(sizeof(tss64_t) == 104, "Unexpected x86-64 TSS layout");

static uint64_t gdt[GDT_ENTRY_COUNT] __attribute__((aligned(16)));
static tss64_t tss;
static bool gdt_ready;

extern void gdt_load(const gdt_pointer_t* pointer);

static void set_tss_descriptor(uintptr_t base, uint32_t limit) {
    uint64_t low = (limit & 0xFFFFULL) |
        ((uint64_t)(base & 0xFFFFFFULL) << 16) |
        (0x89ULL << 40) |
        ((uint64_t)(limit & 0xF0000ULL) << 32) |
        ((uint64_t)(base & 0xFF000000ULL) << 32);

    gdt[5] = low;
    gdt[6] = (uint64_t)(base >> 32);
}

bool gdt_set_kernel_stack(uintptr_t kernel_stack_top) {
    if (!gdt_ready || kernel_stack_top == 0) return false;
    tss.rsp0 = kernel_stack_top;
    return true;
}

bool gdt_init(uintptr_t kernel_stack_top) {
    if (kernel_stack_top == 0) return false;

    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;
    gdt[2] = 0x00CF92000000FFFFULL;
    gdt[3] = 0x00CFF2000000FFFFULL;
    gdt[4] = 0x00AFFA000000FFFFULL;

    tss = (tss64_t){0};
    tss.rsp0 = kernel_stack_top;
    tss.iomap_base = sizeof(tss64_t);
    set_tss_descriptor((uintptr_t)&tss, sizeof(tss64_t) - 1);

    gdt_pointer_t pointer = {
        .limit = sizeof(gdt) - 1,
        .base = (uintptr_t)gdt
    };
    gdt_load(&pointer);
    gdt_ready = true;
    return true;
}
#endif