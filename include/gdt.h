#ifndef ROCKOS_GDT_H
#define ROCKOS_GDT_H

#include <stdbool.h>

#include <stdint.h>

bool gdt_init(uintptr_t kernel_stack_top);
bool gdt_set_kernel_stack(uintptr_t kernel_stack_top);

#endif