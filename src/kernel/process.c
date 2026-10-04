#include "process.h"

#include "io.h"
#include "paging.h"

static process_t process_table[MAX_PROCESSES];

static process_state_t process_state_from_task(task_state_t state) {
    switch (state) {
        case TASK_STATE_READY: return PROCESS_STATE_READY;
        case TASK_STATE_RUNNING: return PROCESS_STATE_RUNNING;
        case TASK_STATE_SLEEPING: return PROCESS_STATE_SLEEPING;
        case TASK_STATE_ZOMBIE: return PROCESS_STATE_ZOMBIE;
        default: return PROCESS_STATE_UNUSED;
    }
}

void process_init(void) {
    paging_init();
    uint64_t flags = save_irq_disable();
    for (size_t index = 0; index < MAX_PROCESSES; index++) {
        process_table[index].pid = 0;
        process_table[index].task = NULL;
        process_table[index].state = PROCESS_STATE_UNUSED;
        process_table[index].address_space = 0;
        process_table[index].user_stack = 0;
        process_table[index].register_context = 0;
        process_table[index].resource_count = 0;
    }
    restore_irq(flags);
    process_refresh();
}

void process_refresh(void) {
    uint64_t flags = save_irq_disable();
    for (size_t index = 0; index < MAX_PROCESSES; index++) {
        const task_t* task = task_get_at(index);
        process_t* process = &process_table[index];
        if (!task) {
            if (process->address_space) {
                paging_address_space_destroy(process->address_space);
            }
            process->pid = 0;
            process->task = NULL;
            process->state = PROCESS_STATE_UNUSED;
            process->address_space = 0;
            process->user_stack = 0;
            process->register_context = 0;
            process->resource_count = 0;
            continue;
        }

        if (process->pid != task->pid && process->address_space) {
            paging_address_space_destroy(process->address_space);
            process->address_space = 0;
        }
        process->pid = task->pid;
        process->task = task;
        process->state = process_state_from_task(task->state);
        if (!process->address_space) {
            process->address_space = paging_address_space_create();
        }
        process->user_stack = 0;
        process->register_context = task->kernel_stack_pointer;
        process->resource_count = 0;
    }
    restore_irq(flags);
}

process_t* process_get_by_pid(pid_t pid) {
    process_refresh();
    for (size_t index = 0; index < MAX_PROCESSES; index++) {
        if (process_table[index].pid == pid) return &process_table[index];
    }
    return NULL;
}

process_t* process_get_at(size_t index) {
    if (index >= MAX_PROCESSES) return NULL;
    process_refresh();
    return process_table[index].state == PROCESS_STATE_UNUSED
        ? NULL : &process_table[index];
}

process_t* process_get_current(void) {
    task_t* task = task_get_current();
    return task ? process_get_by_pid(task->pid) : NULL;
}

uint32_t process_get_count(void) {
    uint32_t count = 0;
    process_refresh();
    for (size_t index = 0; index < MAX_PROCESSES; index++) {
        if (process_table[index].state != PROCESS_STATE_UNUSED) count++;
    }
    return count;
}

bool process_activate(pid_t pid) {
    process_t* process = process_get_by_pid(pid);
    if (!process || !process->address_space) {
        paging_restore_kernel_address_space();
        return false;
    }
    paging_load_address_space(process->address_space);
    return true;
}

uintptr_t process_current_address_space(void) {
    return paging_current_address_space();
}

void process_activate_kernel(void) {
    paging_restore_kernel_address_space();
}

bool process_address_space_selftest(void) {
    process_t* first = NULL;
    process_t* second = NULL;
    uintptr_t kernel_root = paging_kernel_address_space();

    process_refresh();
    for (size_t index = 0; index < MAX_PROCESSES; index++) {
        process_t* process = &process_table[index];
        if (process->state == PROCESS_STATE_UNUSED ||
            process->pid == 1 || !process->address_space) {
            continue;
        }
        if (!first) first = process;
        else {
            second = process;
            break;
        }
    }
    if (!first || !second || first->address_space == second->address_space) {
        paging_restore_kernel_address_space();
        return false;
    }

    paging_load_address_space(first->address_space);
    bool first_loaded = paging_current_address_space() == first->address_space;
    paging_load_address_space(second->address_space);
    bool second_loaded = paging_current_address_space() == second->address_space;
    paging_restore_kernel_address_space();
    return first_loaded && second_loaded &&
        paging_current_address_space() == kernel_root;
}