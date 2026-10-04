#include "ring3.h"

#include "display.h"
#include "gdt.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>

#define RING3_TEST_CODE_ADDRESS 0x40000000ULL
#define RING3_TEST_STACK_ADDRESS 0x40001000ULL
#define RING3_TEST_STACK_TOP (RING3_TEST_STACK_ADDRESS + PMM_PAGE_SIZE)

extern const uint8_t ring3_user_test_program_start[];
extern const uint8_t ring3_user_test_program_end[];
extern const uint8_t ring3_fault_program_start[];
extern const uint8_t ring3_fault_program_end[];
extern void ring3_enter(uintptr_t user_rip, uintptr_t user_rsp)
    __attribute__((noreturn));

static uintptr_t active_address_space;
static uintptr_t active_code_page;
static uintptr_t active_stack_page;

static void ring3_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    display_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        display_putchar(digits[(value >> shift) & 0xF]);
    }
}

static void clear_page(void* page) {
    uint8_t* bytes = (uint8_t*)page;
    for (size_t index = 0; index < PMM_PAGE_SIZE; index++) {
        bytes[index] = 0;
    }
}

static void release_user_mapping(uintptr_t address_space,
    uintptr_t virtual_address, uintptr_t expected_page) {
    uintptr_t physical_address = 0;
    if (!paging_unmap_user_page(address_space, virtual_address,
            &physical_address) ||
        physical_address != expected_page ||
        !pmm_free((void*)physical_address, 1)) {
        display_puts("[-] Ring 3 test cleanup failed; halting.\n");
        for (;;) asm volatile ("cli; hlt" : : : "memory");
    }
}

static void ring3_test_fail(process_t* process, const char* reason) {
    display_puts("[-] Ring 3 test setup failed: ");
    display_puts(reason);
    display_putchar('\n');
    if (active_address_space) {
        if (active_stack_page) {
            release_user_mapping(active_address_space,
                RING3_TEST_STACK_ADDRESS, active_stack_page);
            active_stack_page = 0;
        }
        if (active_code_page) {
            release_user_mapping(active_address_space,
                RING3_TEST_CODE_ADDRESS, active_code_page);
            active_code_page = 0;
        }
        active_address_space = 0;
    }
    if (process) process->user_stack = 0;
    task_exit();
}

static void ring3_enter_test_program(const uint8_t* program_start,
    const uint8_t* program_end, bool fault_test) __attribute__((noreturn));

static void ring3_enter_test_program(const uint8_t* program_start,
    const uint8_t* program_end, bool fault_test) {
    task_t* task = task_get_current();
    process_t* process = process_get_current();
    if (!task || !process || !process->address_space ||
        task->kernel_stack_base == 0 || task->kernel_stack_size == 0) {
        ring3_test_fail(process, "no current task address space or kernel stack");
    }

    uintptr_t kernel_stack_top =
        (task->kernel_stack_base + task->kernel_stack_size) & ~(uintptr_t)0xF;
    if (!gdt_set_kernel_stack(kernel_stack_top)) {
        ring3_test_fail(process, "unable to synchronize TSS RSP0");
    }

    active_code_page = (uintptr_t)pmm_alloc(1);
    active_stack_page = (uintptr_t)pmm_alloc(1);
    if (!active_code_page || !active_stack_page) {
        if (active_code_page) pmm_free((void*)active_code_page, 1);
        if (active_stack_page) pmm_free((void*)active_stack_page, 1);
        active_code_page = 0;
        active_stack_page = 0;
        ring3_test_fail(process, "unable to allocate user pages");
    }

    clear_page((void*)active_code_page);
    clear_page((void*)active_stack_page);
    size_t program_size = (size_t)(program_end - program_start);
    if (program_size == 0 || program_size > PMM_PAGE_SIZE) {
        pmm_free((void*)active_code_page, 1);
        pmm_free((void*)active_stack_page, 1);
        active_code_page = 0;
        active_stack_page = 0;
        ring3_test_fail(process, "invalid user test program size");
    }
    for (size_t index = 0; index < program_size; index++) {
        ((uint8_t*)active_code_page)[index] = program_start[index];
    }

    active_address_space = process->address_space;
    if (!paging_map_user_page(active_address_space, RING3_TEST_CODE_ADDRESS,
            active_code_page, false)) {
        pmm_free((void*)active_code_page, 1);
        pmm_free((void*)active_stack_page, 1);
        active_code_page = 0;
        active_stack_page = 0;
        active_address_space = 0;
        ring3_test_fail(process, "unable to map user code page");
    }
    if (!paging_map_user_page(active_address_space, RING3_TEST_STACK_ADDRESS,
            active_stack_page, true)) {
        release_user_mapping(active_address_space, RING3_TEST_CODE_ADDRESS,
            active_code_page);
        active_code_page = 0;
        pmm_free((void*)active_stack_page, 1);
        active_stack_page = 0;
        active_address_space = 0;
        ring3_test_fail(process, "unable to map user stack page");
    }

    process->user_stack = RING3_TEST_STACK_TOP;
    display_puts("[+] Ring 3 test: entering user mode...\n");
    if (!fault_test) {
        display_puts("[+] Ring 3 syscall output: ");
    }
    paging_load_address_space(active_address_space);
    ring3_enter(RING3_TEST_CODE_ADDRESS, RING3_TEST_STACK_TOP);
}

void ring3_test_task(void) {
    ring3_enter_test_program(ring3_user_test_program_start,
        ring3_user_test_program_end, false);
}

void ring3_fault_test_task(void) {
    ring3_enter_test_program(ring3_fault_program_start,
        ring3_fault_program_end, true);
}

uint64_t ring3_syscall_dispatch(uint64_t syscall_number, uint64_t argument) {
    if (syscall_number == RING3_SYSCALL_PUTCHAR) {
        process_t* process = process_get_current();
        if (!process || process->address_space != active_address_space ||
            !active_address_space || !active_code_page || !active_stack_page) {
            display_puts("[-] Ring 3 syscall from invalid process state; halting.\n");
            for (;;) asm volatile ("cli; hlt" : : : "memory");
        }
        if (argument != '\n' && (argument < 0x20 || argument > 0x7E)) {
            return UINT64_MAX;
        }
        paging_restore_kernel_address_space();
        display_putchar((char)argument);
        paging_load_address_space(active_address_space);
        return 0;
    }
    if (syscall_number != RING3_SYSCALL_EXIT) {
        display_puts("[-] Ring 3 test made an unsupported syscall.\n");
        return UINT64_MAX;
    }

    paging_restore_kernel_address_space();
    process_t* process = process_get_current();
    if (!process || process->address_space != active_address_space ||
        !active_address_space || !active_code_page || !active_stack_page) {
        display_puts("[-] Ring 3 test returned with invalid process state; halting.\n");
        for (;;) asm volatile ("cli; hlt" : : : "memory");
    }
    display_puts("[+] Ring 3 test: returned to kernel\n");

    release_user_mapping(active_address_space, RING3_TEST_STACK_ADDRESS,
        active_stack_page);
    release_user_mapping(active_address_space, RING3_TEST_CODE_ADDRESS,
        active_code_page);
    process->user_stack = 0;
    active_address_space = 0;
    active_code_page = 0;
    active_stack_page = 0;
    task_exit();
}

void ring3_fault_dispatch(uint64_t vector, uint64_t error_code,
    uint64_t instruction_pointer, uint64_t fault_address) {
    paging_restore_kernel_address_space();

    process_t* process = process_get_current();
    if (!process || process->address_space != active_address_space ||
        !active_address_space || !active_code_page || !active_stack_page) {
        display_puts("[-] Ring 3 exception outside the isolated test task; halting.\n");
        for (;;) asm volatile ("cli; hlt" : : : "memory");
    }

    display_puts("[+] Ring 3 test: caught user-mode exception vector ");
    ring3_write_hex(vector);
    display_puts(" at RIP ");
    ring3_write_hex(instruction_pointer);
    display_puts(" error ");
    ring3_write_hex(error_code);
    if (vector == 14) {
        display_puts(" CR2 ");
        ring3_write_hex(fault_address);
    }
    display_putchar('\n');

    release_user_mapping(active_address_space, RING3_TEST_STACK_ADDRESS,
        active_stack_page);
    release_user_mapping(active_address_space, RING3_TEST_CODE_ADDRESS,
        active_code_page);
    process->user_stack = 0;
    active_address_space = 0;
    active_code_page = 0;
    active_stack_page = 0;
    display_puts("[+] Ring 3 test: faulting task terminated; kernel continues\n");
    task_exit();
}
