#include "task.h"
#include "gdt.h"
#include "timer.h"
#include "io.h"

#include <stddef.h>

static task_t task_table[MAX_TASKS];
static task_t* current_task = NULL;
static task_t* idle_task = NULL;
static task_t* run_queue_head = NULL;
static task_t* run_queue_tail = NULL;
static uint8_t task_kernel_stacks[MAX_TASKS][TASK_KERNEL_STACK_SIZE] __attribute__((aligned(16)));
static pid_t next_pid = 1;
static bool need_resched = false;

#define TASK_TIME_SLICE_TICKS 5U

extern char boot_stack_bottom[];
extern char boot_stack_top[];
extern void context_switch(uintptr_t* old_stack_pointer, uintptr_t new_stack_pointer);

static void task_bootstrap(void) __attribute__((noreturn));
static void idle_task_entry(void) __attribute__((noreturn));
static void task_reap_zombies_locked(void);

static void task_update_tss_stack(const task_t* task) {
    if (!task || task->kernel_stack_base == 0 || task->kernel_stack_size == 0 ||
        task->kernel_stack_size > UINTPTR_MAX - task->kernel_stack_base) {
        return;
    }

    uintptr_t stack_top = task->kernel_stack_base + task->kernel_stack_size;
    stack_top &= ~(uintptr_t)0xF;
    gdt_set_kernel_stack(stack_top);
}

static void copy_name(char* destination, const char* source, uint32_t capacity) {
    uint32_t index = 0;

    if (!destination || !source || capacity == 0) {
        return;
    }

    while (index + 1 < capacity && source[index] != '\0') {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static pid_t allocate_pid(void) {
    for (uint32_t attempt = 0; attempt <= MAX_TASKS; attempt++) {
        pid_t candidate = next_pid++;
        bool in_use = false;

        if (next_pid == 0) {
            next_pid = 1;
        }
        if (candidate == 0) {
            continue;
        }

        for (uint32_t i = 0; i < MAX_TASKS; i++) {
            if (task_table[i].state != TASK_STATE_UNUSED &&
                task_table[i].state != TASK_STATE_REAPED &&
                task_table[i].pid == candidate) {
                in_use = true;
                break;
            }
        }

        if (!in_use) {
            return candidate;
        }
    }

    return 0;
}

static void run_queue_remove(task_t* task) {
    if (!task->on_run_queue) {
        return;
    }

    task_t* previous = NULL;
    task_t* current = run_queue_head;
    while (current && current != task) {
        previous = current;
        current = current->run_queue_next;
    }

    if (current) {
        if (previous) {
            previous->run_queue_next = current->run_queue_next;
        } else {
            run_queue_head = current->run_queue_next;
        }
        if (run_queue_tail == current) {
            run_queue_tail = previous;
        }
    }

    task->run_queue_next = NULL;
    task->on_run_queue = false;
}

static void run_queue_enqueue(task_t* task) {
    if (task->on_run_queue || task->is_idle || task->state != TASK_STATE_READY) {
        return;
    }

    task->run_queue_next = NULL;
    if (run_queue_tail) {
        run_queue_tail->run_queue_next = task;
    } else {
        run_queue_head = task;
    }
    run_queue_tail = task;
    task->on_run_queue = true;
}

static task_t* run_queue_dequeue(void) {
    while (run_queue_head && run_queue_head->state != TASK_STATE_READY) {
        task_t* invalid = run_queue_head;
        run_queue_head = invalid->run_queue_next;
        invalid->run_queue_next = NULL;
        invalid->on_run_queue = false;
    }

    task_t* task = run_queue_head;
    if (!task) {
        run_queue_tail = NULL;
        return NULL;
    }

    run_queue_head = task->run_queue_next;
    if (!run_queue_head) run_queue_tail = NULL;
    task->run_queue_next = NULL;
    task->on_run_queue = false;
    return task;
}

static void initialize_task_slot(task_t* task, uint32_t slot) {
    task->pid = 0;
    task->name[0] = '\0';
    task->state = TASK_STATE_UNUSED;
    task->creation_tick = 0;
    task->wake_tick = 0;
    task->ticks_used = 0;
    task->context_switches = 0;
    task->slice_remaining = TASK_TIME_SLICE_TICKS;
    task->priority = 0;
    task->entry_point = NULL;
    task->kernel_stack_base = (uintptr_t)&task_kernel_stacks[slot][0];
    task->kernel_stack_size = sizeof(task_kernel_stacks[slot]);
    uintptr_t stack_top = (task->kernel_stack_base + task->kernel_stack_size) & ~(uintptr_t)0xF;
    uintptr_t* initial_stack = (uintptr_t*)(stack_top - 8 * sizeof(uintptr_t));
    initial_stack[0] = 0;
    initial_stack[1] = 0;
    initial_stack[2] = 0;
    initial_stack[3] = 0;
    initial_stack[4] = 0;
    initial_stack[5] = 0;
    initial_stack[6] = (uintptr_t)task_bootstrap;
    initial_stack[7] = 0;
    task->kernel_stack_pointer = (uintptr_t)initial_stack;
    task->run_queue_next = NULL;
    task->on_run_queue = false;
    task->is_idle = false;
}

static task_t* create_task_locked(const char* name, void (*entry_point)(void), bool is_idle) {
    int32_t free_slot = -1;

    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_UNUSED ||
            task_table[i].state == TASK_STATE_REAPED) {
            free_slot = (int32_t)i;
            break;
        }
    }

    if (free_slot < 0 || (is_idle && idle_task)) {
        return NULL;
    }

    pid_t pid = allocate_pid();
    if (pid == 0) {
        return NULL;
    }

    task_t* task = &task_table[free_slot];
    initialize_task_slot(task, (uint32_t)free_slot);
    task->pid = pid;
    copy_name(task->name, name ? name : (is_idle ? "idle" : "unnamed"), TASK_NAME_LEN);
    task->state = TASK_STATE_READY;
    task->creation_tick = timer_get_ticks();
    task->slice_remaining = TASK_TIME_SLICE_TICKS;
    task->priority = 1;
    task->entry_point = entry_point;
    task->is_idle = is_idle;
    if (is_idle) {
        task->entry_point = idle_task_entry;
    }

    if (is_idle) {
        idle_task = task;
    } else {
        run_queue_enqueue(task);
    }
    return task;
}

static bool set_task_state_locked(task_t* task, task_state_t state) {
    if (!task || task->state == TASK_STATE_UNUSED || task->state == TASK_STATE_ZOMBIE) {
        return false;
    }
    if (task == &task_table[0] && state == TASK_STATE_ZOMBIE) {
        return false;
    }
    if (state != TASK_STATE_READY && state != TASK_STATE_RUNNING &&
        state != TASK_STATE_SLEEPING && state != TASK_STATE_ZOMBIE) {
        return false;
    }
    if (task->is_idle && state != TASK_STATE_READY) {
        return false;
    }
    if ((task == current_task && state != TASK_STATE_RUNNING) ||
        (task != current_task && state == TASK_STATE_RUNNING)) {
        return false;
    }

    if (state == task->state) {
        if (state == TASK_STATE_READY) {
            run_queue_enqueue(task);
        }
        return true;
    }

    run_queue_remove(task);
    task->state = state;
    if (state == TASK_STATE_SLEEPING) {
        task->wake_tick = timer_get_ticks() + 1;
    }
    if (state == TASK_STATE_READY) {
        task->wake_tick = 0;
        run_queue_enqueue(task);
    }
    return true;
}

const char* task_state_to_string(task_state_t state) {
    switch (state) {
        case TASK_STATE_READY:      return "READY";
        case TASK_STATE_RUNNING:    return "RUNNING";
        case TASK_STATE_SLEEPING:   return "SLEEPING";
        case TASK_STATE_ZOMBIE:     return "ZOMBIE";
        case TASK_STATE_REAPED:     return "REAPED";
        default:                    return "UNUSED";
    }
}

void task_init(void) {
    uint64_t flags = save_irq_disable();

    next_pid = 1;
    current_task = NULL;
    idle_task = NULL;
    run_queue_head = NULL;
    run_queue_tail = NULL;
    need_resched = false;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        initialize_task_slot(&task_table[i], i);
    }

    task_t* kernel_task = &task_table[0];
    kernel_task->pid = allocate_pid();
    copy_name(kernel_task->name, "kernel", TASK_NAME_LEN);
    kernel_task->state = TASK_STATE_RUNNING;
    kernel_task->creation_tick = timer_get_ticks();
    kernel_task->priority = 1;
    kernel_task->kernel_stack_base = (uintptr_t)boot_stack_bottom;
    kernel_task->kernel_stack_size = (size_t)((uintptr_t)boot_stack_top -
        (uintptr_t)boot_stack_bottom);
    kernel_task->kernel_stack_pointer = 0;
    current_task = kernel_task;

    restore_irq(flags);
}

task_t* task_create(const char* name, void (*entry_point)(void)) {
    uint64_t flags = save_irq_disable();
    task_t* task = create_task_locked(name, entry_point, false);
    restore_irq(flags);
    return task;
}

task_t* task_create_idle(void) {
    uint64_t flags = save_irq_disable();
    task_t* task = create_task_locked("idle", NULL, true);
    restore_irq(flags);
    return task;
}

task_t* task_get_by_pid(pid_t pid) {
    uint64_t flags = save_irq_disable();

    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state != TASK_STATE_UNUSED &&
            task_table[i].state != TASK_STATE_REAPED &&
            task_table[i].pid == pid) {
            task_t* task = &task_table[i];
            restore_irq(flags);
            return task;
        }
    }

    restore_irq(flags);
    return NULL;
}

task_t* task_get_at(size_t index) {
    if (index >= MAX_TASKS) {
        return NULL;
    }

    uint64_t flags = save_irq_disable();
    task_t* task = &task_table[index];
    if (task->state == TASK_STATE_UNUSED || task->state == TASK_STATE_REAPED) {
        task = NULL;
    }
    restore_irq(flags);
    return task;
}

task_t* task_get_current(void) {
    uint64_t flags = save_irq_disable();
    task_t* task = current_task;
    restore_irq(flags);
    return task;
}

task_t* task_get_idle(void) {
    uint64_t flags = save_irq_disable();
    task_t* task = idle_task;
    restore_irq(flags);
    return task;
}

bool task_set_state(pid_t pid, task_state_t state) {
    uint64_t flags = save_irq_disable();

    if (pid == 0) {
        restore_irq(flags);
        return false;
    }

    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state != TASK_STATE_UNUSED &&
            task_table[i].state != TASK_STATE_REAPED &&
            task_table[i].pid == pid) {
            bool updated = set_task_state_locked(&task_table[i], state);
            restore_irq(flags);
            return updated;
        }
    }

    restore_irq(flags);
    return false;
}

bool task_terminate(pid_t pid) {
    return task_set_state(pid, TASK_STATE_ZOMBIE);
}

static void task_reap_zombies_locked(void) {
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        task_t* task = &task_table[i];
        if (task == current_task || task == &task_table[0] ||
            task->state != TASK_STATE_ZOMBIE || task->is_idle) {
            continue;
        }

        run_queue_remove(task);
        task->pid = 0;
        task->name[0] = '\0';
        task->creation_tick = 0;
        task->wake_tick = 0;
        task->ticks_used = 0;
        task->context_switches = 0;
        task->slice_remaining = 0;
        task->priority = 0;
        task->entry_point = NULL;
        task->kernel_stack_pointer = 0;
        task->kernel_stack_base = 0;
        task->kernel_stack_size = 0;
        task->run_queue_next = NULL;
        task->on_run_queue = false;
        task->state = TASK_STATE_REAPED;
    }
}

static void task_reap_zombies(void) {
    uint64_t flags = save_irq_disable();
    task_reap_zombies_locked();
    restore_irq(flags);
}

void task_timer_tick(void) {
    uint64_t now = timer_get_ticks();

    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        task_t* task = &task_table[i];
        if (task->state == TASK_STATE_SLEEPING && now >= task->wake_tick) {
            task->state = TASK_STATE_READY;
            task->wake_tick = 0;
            run_queue_enqueue(task);
        }
    }

    if (!current_task || current_task->state != TASK_STATE_RUNNING) {
        return;
    }

    current_task->ticks_used++;
    if (current_task->slice_remaining > 0) {
        current_task->slice_remaining--;
    }
    if ((current_task->is_idle && run_queue_head) ||
        (current_task->slice_remaining == 0 && run_queue_head)) {
        need_resched = true;
    }
}

void task_interrupt_exit(void) {
    uint64_t flags = save_irq_disable();
    task_reap_zombies_locked();

    if (!need_resched || !current_task ||
        current_task->state != TASK_STATE_RUNNING) {
        restore_irq(flags);
        return;
    }

    need_resched = false;
    task_t* previous = current_task;
    previous->state = TASK_STATE_READY;
    if (!previous->is_idle) {
        run_queue_enqueue(previous);
    }

    task_t* next = run_queue_dequeue();
    if (!next && idle_task && idle_task != previous &&
        idle_task->state == TASK_STATE_READY) {
        next = idle_task;
    }
    if (!next || next == previous) {
        previous->state = TASK_STATE_RUNNING;
        previous->slice_remaining = TASK_TIME_SLICE_TICKS;
        restore_irq(flags);
        return;
    }

    next->state = TASK_STATE_RUNNING;
    next->slice_remaining = TASK_TIME_SLICE_TICKS;
    current_task = next;
    task_update_tss_stack(next);
    previous->context_switches++;
    next->context_switches++;
    context_switch(&previous->kernel_stack_pointer, next->kernel_stack_pointer);
    restore_irq(flags);
}

void task_exit(void) {
    uint64_t flags = save_irq_disable();
    task_t* exiting_task = current_task;

    if (!exiting_task || exiting_task == &task_table[0] || exiting_task->is_idle) {
        restore_irq(flags);
        for (;;) {
            asm volatile ("cli; hlt" : : : "memory");
        }
    }

    exiting_task->state = TASK_STATE_ZOMBIE;
    exiting_task->wake_tick = 0;
    run_queue_remove(exiting_task);

    task_t* next = run_queue_dequeue();
    if (!next && idle_task && idle_task != exiting_task &&
        idle_task->state == TASK_STATE_READY) {
        next = idle_task;
    }

    if (!next) {
        restore_irq(flags);
        for (;;) {
            asm volatile ("cli; hlt" : : : "memory");
        }
    }

    next->state = TASK_STATE_RUNNING;
    current_task = next;
    task_update_tss_stack(next);
    exiting_task->context_switches++;
    next->context_switches++;
    context_switch(&exiting_task->kernel_stack_pointer, next->kernel_stack_pointer);

    for (;;) {
        asm volatile ("cli; hlt" : : : "memory");
    }
}

void task_yield(void) {
    uint64_t flags = save_irq_disable();
    task_t* previous = current_task;

    if (!previous || previous->state != TASK_STATE_RUNNING) {
        restore_irq(flags);
        return;
    }

    previous->state = TASK_STATE_READY;
    previous->slice_remaining = TASK_TIME_SLICE_TICKS;
    need_resched = false;
    if (!previous->is_idle) {
        run_queue_enqueue(previous);
    }

    task_t* next = run_queue_dequeue();
    if (!next && idle_task && idle_task != previous &&
        idle_task->state == TASK_STATE_READY) {
        next = idle_task;
    }

    if (!next) {
        previous->state = TASK_STATE_RUNNING;
        restore_irq(flags);
        return;
    }

    if (next == previous) {
        previous->state = TASK_STATE_RUNNING;
        restore_irq(flags);
        return;
    }

    next->state = TASK_STATE_RUNNING;
    next->slice_remaining = TASK_TIME_SLICE_TICKS;
    current_task = next;
    task_update_tss_stack(next);
    previous->context_switches++;
    next->context_switches++;
    context_switch(&previous->kernel_stack_pointer, next->kernel_stack_pointer);
    restore_irq(flags);
    task_reap_zombies();
}

bool task_sleep_ticks(uint64_t ticks) {
    if (ticks == 0) {
        task_yield();
        return true;
    }

    uint64_t flags = save_irq_disable();
    task_t* previous = current_task;
    if (!previous || previous == &task_table[0] || previous->is_idle ||
        previous->state != TASK_STATE_RUNNING) {
        restore_irq(flags);
        return false;
    }

    previous->state = TASK_STATE_SLEEPING;
    previous->wake_tick = timer_get_ticks() + ticks;
    task_t* next = run_queue_dequeue();
    if (!next && idle_task && idle_task != previous &&
        idle_task->state == TASK_STATE_READY) {
        next = idle_task;
    }
    if (!next) {
        previous->state = TASK_STATE_RUNNING;
        previous->wake_tick = 0;
        restore_irq(flags);
        return false;
    }

    next->state = TASK_STATE_RUNNING;
    next->slice_remaining = TASK_TIME_SLICE_TICKS;
    current_task = next;
    task_update_tss_stack(next);
    previous->context_switches++;
    next->context_switches++;
    context_switch(&previous->kernel_stack_pointer, next->kernel_stack_pointer);
    restore_irq(flags);
    task_reap_zombies();
    return true;
}

uint32_t task_get_count(void) {
    uint64_t flags = save_irq_disable();
    uint32_t count = 0;

    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_READY ||
            task_table[i].state == TASK_STATE_RUNNING ||
            task_table[i].state == TASK_STATE_SLEEPING) {
            count++;
        }
    }

    restore_irq(flags);
    return count;
}

static void task_bootstrap(void) {
    asm volatile ("sti" : : : "memory");
    task_reap_zombies();
    task_t* task = task_get_current();
    if (task && task->entry_point) {
        task->entry_point();
    }
    task_exit();
}

static void idle_task_entry(void) {
    for (;;) {
        asm volatile ("sti; hlt" : : : "memory");
        task_yield();
    }
}