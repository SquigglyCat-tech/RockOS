#include "ring3.h"

#include "rok.h"

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
#define RING3_MEMORY_CODE_ADDRESS 0x40003000ULL
#define RING3_MEMORY_STACK_ADDRESS 0x40004000ULL
#define RING3_MEMORY_MMAP_BASE 0x50000000ULL
#define RING3_MEMORY_MAPPED_MAX 64ULL

/* Wine layer 0: pages handed out through RING3_SYSCALL_MMAP are tracked
 * here so munmap can only release what this kernel itself mapped. */
typedef struct {
    uintptr_t valid_address;
    uintptr_t physical_address;
} ring3_memory_mapping_t;

static ring3_memory_mapping_t ring3_memory_mappings[RING3_MEMORY_MAPPED_MAX];
static uintptr_t ring3_mmap_next_address = RING3_MEMORY_MMAP_BASE;

extern const uint8_t ring3_user_test_program_start[];
extern const uint8_t ring3_user_test_program_end[];
extern const uint8_t ring3_memory_program_start[];
extern const uint8_t ring3_memory_program_end[];
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

/* Same enter/cleanup path as the syscall test, but at dedicated memory
 * addresses so the two tests never collide. */
static void ring3_enter_memory_test_program(const uint8_t* program_start,
    const uint8_t* program_end) __attribute__((noreturn));

static void ring3_enter_memory_test_program(const uint8_t* program_start,
    const uint8_t* program_end) {
    task_t* task = task_get_current();
    process_t* process = process_get_current();
    if (!task || !process || !process->address_space ||
        task->kernel_stack_base == 0) {
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
        ring3_test_fail(process, "invalid memory test program size");
    }
    for (size_t index = 0; index < program_size; index++) {
        ((uint8_t*)active_code_page)[index] = program_start[index];
    }
    if (!gdt_set_kernel_stack(kernel_stack_top)) {
        ring3_test_fail(process, "unable to synchronize TSS RSP0");
    }
    active_address_space = process->address_space;
    if (!paging_map_user_page(active_address_space, RING3_MEMORY_CODE_ADDRESS,
            active_code_page, false) ||
        !paging_map_user_page(active_address_space,
            RING3_MEMORY_STACK_ADDRESS, active_stack_page, true)) {
        if (active_code_page) {
            release_user_mapping(active_address_space,
                RING3_MEMORY_CODE_ADDRESS, active_code_page);
        }
        if (active_stack_page) {
            release_user_mapping(active_address_space,
                RING3_MEMORY_STACK_ADDRESS, active_stack_page);
        }
        active_address_space = 0;
        active_code_page = 0;
        active_stack_page = 0;
        ring3_test_fail(process, "unable to map memory test pages");
    }
    process->user_stack =
        RING3_MEMORY_STACK_ADDRESS + PMM_PAGE_SIZE;
    display_puts("[+] Ring 3 memory test: entering user mode...\n");
    display_puts("[+] Ring 3 memory test output: ");
    paging_load_address_space(active_address_space);
    ring3_enter(RING3_MEMORY_CODE_ADDRESS,
        RING3_MEMORY_STACK_ADDRESS + PMM_PAGE_SIZE);
}

void ring3_memory_test_task(void) {
    ring3_enter_memory_test_program(ring3_memory_program_start,
        ring3_memory_program_end);
}

/* ---- RockOS native executable (.rok) runs ----
 * The shell stages a parsed payload through ring3_rok_stage and then
 * task_create("rok_run", ring3_rok_task). Payload lives in a fixed
 * kernel buffer so the parsed image survives until the user task runs.
 * Nothing about this path is Wine-specific: it is the same layer 0
 * syscall surface (putchar/exit/mmap/munmap) that .rok programs use. */
#define RING3_ROK_CODE_ADDRESS 0x40005000ULL
#define RING3_ROK_STACK_ADDRESS 0x40006000ULL
static uint8_t ring3_rok_payload[ROK_CODE_SIZE_MAX];
static uint64_t ring3_rok_payload_size;
static uint64_t ring3_rok_entry_offset;

/* ---- PE32 (.exe) sessions (Wine layer 1b) ----
 * Same layer-0 syscall surface as .rok runs. The loader (pe_run.c)
 * stages a single section payload at its image_base+section-rva with
 * the entry offset inside the section; the task maps the code page
 * there and enters ring 3. */
#define RING3_PE_STACK_ADDRESS 0x40006000ULL
#define RING3_PE_PAYLOAD_MAX 4096U
static uint8_t ring3_pe_payload[RING3_PE_PAYLOAD_MAX];
static uint64_t ring3_pe_payload_size;
static uintptr_t ring3_pe_code_address;
static uint64_t ring3_pe_entry_offset;

bool ring3_pe_stage(const uint8_t* payload, uint64_t payload_size,
    uintptr_t code_base_address, uint64_t entry_offset) {
    if (!payload || payload_size == 0 || payload_size > RING3_PE_PAYLOAD_MAX ||
        entry_offset >= payload_size || code_base_address == 0) {
        return false;
    }
    for (uint64_t index = 0; index < payload_size; index++) {
        ring3_pe_payload[index] = payload[index];
    }
    ring3_pe_payload_size = payload_size;
    ring3_pe_code_address = code_base_address;
    ring3_pe_entry_offset = entry_offset;
    return true;
}

void ring3_pe_task(void) {
    task_t* task = task_get_current();
    process_t* process = process_get_current();
    if (!task || !process || !process->address_space ||
        task->kernel_stack_base == 0) {
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
        ring3_test_fail(process, "unable to allocate .exe user pages");
    }
    clear_page((void*)active_code_page);
    clear_page((void*)active_stack_page);
    if (ring3_pe_payload_size == 0 ||
        ring3_pe_payload_size > PMM_PAGE_SIZE) {
        ring3_test_fail(process, "invalid staged .exe payload");
    }
    for (uint64_t index = 0; index < ring3_pe_payload_size; index++) {
        ((uint8_t*)active_code_page)[index] = ring3_pe_payload[index];
    }
    active_address_space = process->address_space;
    if (!paging_map_user_page(active_address_space,
            ring3_pe_code_address, active_code_page, false) ||
        !paging_map_user_page(active_address_space,
            RING3_PE_STACK_ADDRESS, active_stack_page, true)) {
        if (active_code_page) {
            release_user_mapping(active_address_space,
                ring3_pe_code_address, active_code_page);
        }
        if (active_stack_page) {
            release_user_mapping(active_address_space,
                RING3_PE_STACK_ADDRESS, active_stack_page);
        }
        active_address_space = 0;
        active_code_page = 0;
        active_stack_page = 0;
        ring3_test_fail(process, "unable to map .exe user pages");
    }
    process->user_stack = RING3_PE_STACK_ADDRESS + PMM_PAGE_SIZE;
    display_puts("[+] .exe run: entering user mode...\n");
    display_puts("[+] .exe run output: ");
    paging_load_address_space(active_address_space);
    ring3_enter(ring3_pe_code_address + ring3_pe_entry_offset,
        RING3_PE_STACK_ADDRESS + PMM_PAGE_SIZE);
}

bool ring3_rok_stage(const uint8_t* payload, uint64_t payload_size,
    uint64_t entry_offset, bool payload_is_64bit) {
    if (!payload || payload_size == 0 ||
        payload_size > ROK_CODE_SIZE_MAX || entry_offset >= payload_size) {
        return false;
    }
    (void)payload_is_64bit;
    for (uint64_t index = 0; index < payload_size; index++) {
        ring3_rok_payload[index] = payload[index];
    }
    ring3_rok_payload_size = payload_size;
    ring3_rok_entry_offset = entry_offset;
    return true;
}

void ring3_rok_task(void) {
    task_t* task = task_get_current();
    process_t* process = process_get_current();
    if (!task || !process || !process->address_space ||
        task->kernel_stack_base == 0) {
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
        ring3_test_fail(process, "unable to allocate .rok user pages");
    }
    clear_page((void*)active_code_page);
    clear_page((void*)active_stack_page);
    if (ring3_rok_payload_size == 0 ||
        ring3_rok_payload_size > PMM_PAGE_SIZE) {
        ring3_test_fail(process, "invalid staged .rok payload");
    }
    for (uint64_t index = 0; index < ring3_rok_payload_size; index++) {
        ((uint8_t*)active_code_page)[index] = ring3_rok_payload[index];
    }
    active_address_space = process->address_space;
    if (!paging_map_user_page(active_address_space, RING3_ROK_CODE_ADDRESS,
            active_code_page, false) ||
        !paging_map_user_page(active_address_space,
            RING3_ROK_STACK_ADDRESS, active_stack_page, true)) {
        if (active_code_page) {
            release_user_mapping(active_address_space,
                RING3_ROK_CODE_ADDRESS, active_code_page);
        }
        if (active_stack_page) {
            release_user_mapping(active_address_space,
                RING3_ROK_STACK_ADDRESS, active_stack_page);
        }
        active_address_space = 0;
        active_code_page = 0;
        active_stack_page = 0;
        ring3_test_fail(process, "unable to map .rok user pages");
    }
    process->user_stack = RING3_ROK_STACK_ADDRESS + PMM_PAGE_SIZE;
    display_puts("[+] .rok run: entering user mode...\n");
    display_puts("[+] .rok run output: ");
    paging_load_address_space(active_address_space);
    ring3_enter(RING3_ROK_CODE_ADDRESS + ring3_rok_entry_offset,
        RING3_ROK_STACK_ADDRESS + PMM_PAGE_SIZE);
}

void ring3_fault_test_task(void) {
    ring3_enter_test_program(ring3_fault_program_start,
        ring3_fault_program_end, true);
}

/* Wine layer 0 memory: every page the kernel hands out through mmap is
 * tracked here so munmap can only release ranges this kernel mapped, and
 * so an exiting process never leaks its mmap'ed pages. */
static void ring3_memory_reset_proc_span(uintptr_t span_base,
    uint64_t mapped_count);

static uintptr_t ring3_mmap(uint64_t page_count) {
    if (page_count == 0 ||
        page_count > RING3_MEMORY_MAPPED_MAX) {
        return UINT64_MAX;
    }
    process_t* process = process_get_current();
    if (!process || process->address_space != active_address_space ||
        !active_address_space) {
        return UINT64_MAX;
    }
    size_t free_slots = 0;
    for (size_t index = 0; index < RING3_MEMORY_MAPPED_MAX; index++) {
        if (ring3_memory_mappings[index].valid_address == 0) free_slots++;
    }
    if (free_slots < page_count) return UINT64_MAX;

    uintptr_t span_base = ring3_mmap_next_address;
    for (uint64_t index = 0; index < page_count; index++) {
        uintptr_t page = (uintptr_t)pmm_alloc(1);
        if (!page) {
            /* pages already committed are recorded in the table; undo
             * them all and report failure. */
            ring3_mmap_next_address = span_base;
            ring3_memory_reset_proc_span(span_base, index);
            return UINT64_MAX;
        }
        clear_page((void*)page);
        uintptr_t virtual_address =
            ring3_mmap_next_address + index * PMM_PAGE_SIZE;
        if (!paging_map_user_page(active_address_space, virtual_address,
            page, true)) {
            pmm_free((void*)page, 1);
            ring3_mmap_next_address = span_base;
            ring3_memory_reset_proc_span(span_base, index);
            return UINT64_MAX;
        }
        /* record the page (va, phys) in a free slot */
        for (size_t probe = 0; probe < RING3_MEMORY_MAPPED_MAX; probe++) {
            if (ring3_memory_mappings[probe].valid_address == 0) {
                ring3_memory_mappings[probe].valid_address = virtual_address;
                ring3_memory_mappings[probe].physical_address = page;
                break;
            }
        }
        ring3_mmap_next_address = virtual_address + PMM_PAGE_SIZE;
    }
    return span_base;
}

static int ring3_munmap(uintptr_t virtual_address) {
    /* must be page aligned and inside our mmap region */
    if (virtual_address < RING3_MEMORY_MMAP_BASE ||
        virtual_address % PMM_PAGE_SIZE != 0) {
        return -1;
    }
    process_t* process = process_get_current();
    if (!process || process->address_space != active_address_space ||
        !active_address_space) return -1;
    for (size_t index = 0; index < RING3_MEMORY_MAPPED_MAX; index++) {
        if (ring3_memory_mappings[index].valid_address == virtual_address) {
            release_user_mapping(active_address_space, virtual_address,
                ring3_memory_mappings[index].physical_address);
            ring3_memory_mappings[index].valid_address = 0;
            ring3_memory_mappings[index].physical_address = 0;
            return 0;
        }
    }
    return -1;
}

/* Undo any partially-committed span: called with the pages that were
 * successfully mapped before the failure (count = index of failure). */
static void ring3_memory_reset_proc_span(uintptr_t span_base,
    uint64_t mapped_count) {
    for (size_t index = 0; index < RING3_MEMORY_MAPPED_MAX; index++) {
        uintptr_t valid = ring3_memory_mappings[index].valid_address;
        if (valid >= span_base && valid < span_base + mapped_count *
                PMM_PAGE_SIZE) {
            release_user_mapping(active_address_space, valid,
                ring3_memory_mappings[index].physical_address);
            ring3_memory_mappings[index].valid_address = 0;
            ring3_memory_mappings[index].physical_address = 0;
        }
    }
}

/* Free every mmap'ed page owned by the current process (called on exit
 * or on a fatal user fault so nothing in the region leaks). */
static void ring3_memory_cleanup(void) {
    for (size_t index = 0; index < RING3_MEMORY_MAPPED_MAX; index++) {
        if (ring3_memory_mappings[index].valid_address) {
            release_user_mapping(active_address_space,
                ring3_memory_mappings[index].valid_address,
                ring3_memory_mappings[index].physical_address);
            ring3_memory_mappings[index].valid_address = 0;
            ring3_memory_mappings[index].physical_address = 0;
        }
    }
    ring3_mmap_next_address = RING3_MEMORY_MMAP_BASE;
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
    if (syscall_number == RING3_SYSCALL_MMAP) {
        /* mmap(len) - argument is a page count; kernel picks the base */
        return ring3_mmap(argument);
    }
    if (syscall_number == RING3_SYSCALL_MUNMAP) {
        return ring3_munmap((uintptr_t)argument) == 0 ? 0 : UINT64_MAX;
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

    ring3_memory_cleanup();
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
