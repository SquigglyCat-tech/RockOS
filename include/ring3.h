#ifndef ROCKOS_RING3_H
#define ROCKOS_RING3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RING3_SYSCALL_VECTOR 0x80
#define RING3_SYSCALL_EXIT 0
#define RING3_SYSCALL_PUTCHAR 1
#define RING3_SYSCALL_MMAP 5
#define RING3_SYSCALL_MUNMAP 6
/* Win32 import surface (layer 1d): the IAT stubs enter with the
 * function id plus its arguments in the arch-specific registers. */
#define RING3_SYSCALL_WIN32 7
#define RING3_WIN32_EXITPROCESS 0
#define RING3_WIN32_GETSTDHANDLE 1
#define RING3_WIN32_WRITEFILE 2
#define RING3_WIN32_WRITECONSOLEA 3

/* int 0x80: syscall number in RAX, argument in RDI, status returned in RAX. */
void ring3_test_task(void);
void ring3_memory_test_task(void);
bool ring3_rok_stage(const uint8_t* payload, uint64_t payload_size,
    uint64_t entry_offset, bool payload_is_64bit);
void ring3_rok_task(void);
bool ring3_pe_stage(const uint8_t* payload, uint64_t payload_size,
    uintptr_t code_base_address, uint64_t entry_offset);
void ring3_pe_task(void);
void ring3_fault_test_task(void);
uint64_t ring3_syscall_dispatch(uint64_t syscall_number, uint64_t argument);
uint64_t ring3_win32_dispatch(uint64_t fn, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4);
void ring3_fault_dispatch(uint64_t vector, uint64_t error_code,
    uint64_t instruction_pointer, uint64_t fault_address)
    __attribute__((noreturn));

#endif
