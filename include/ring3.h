#ifndef ROCKOS_RING3_H
#define ROCKOS_RING3_H

#include <stdint.h>

#define RING3_SYSCALL_VECTOR 0x80
#define RING3_SYSCALL_EXIT 0
#define RING3_SYSCALL_PUTCHAR 1

/* int 0x80: syscall number in RAX, argument in RDI, status returned in RAX. */
void ring3_test_task(void);
void ring3_fault_test_task(void);
uint64_t ring3_syscall_dispatch(uint64_t syscall_number, uint64_t argument);
void ring3_fault_dispatch(uint64_t vector, uint64_t error_code,
    uint64_t instruction_pointer, uint64_t fault_address)
    __attribute__((noreturn));

#endif
