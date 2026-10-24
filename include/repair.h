#ifndef ROCKOS_REPAIR_H
#define ROCKOS_REPAIR_H

#include <stdbool.h>
#include <stdint.h>

/* Snapshot of a fatal exception, handed to the repair screen. */
typedef struct {
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t cr2;
    bool has_cr2;
    const char* stop_code;   /* e.g. "GENERAL_PROTECTION_FAULT" */
    const char* name;        /* e.g. "General Protection Fault" */
} repair_crash_info_t;

/* "RockOS Repair could not repair your computer" screen.
 *
 * Called from the fatal-exception handler AFTER the "!!!" panic screen has
 * been drawn. Interrupts stay off the whole time: keyboard and mouse are
 * polled straight from the PS/2 controller, so nothing here depends on the
 * scheduler, timer IRQ or any task state. Never returns (the only ways out
 * are a machine restart or an infinite halt). */
void repair_screen_run(const repair_crash_info_t* info)
    __attribute__((noreturn));

#endif
