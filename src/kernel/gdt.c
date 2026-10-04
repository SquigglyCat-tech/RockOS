#include "gdt.h"

#include <stdbool.h>
#include <stddef.h>

#define GDT_ENTRY_COUNT 7U
#define GDT_TSS_SELECTOR 0x28U

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