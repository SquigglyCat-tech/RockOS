#ifndef ROCKOS_TASK_H
#define ROCKOS_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_TASKS 32
#define TASK_NAME_LEN 32
#define TASK_KERNEL_STACK_SIZE 16384

typedef uint32_t pid_t;

typedef enum {
    TASK_STATE_UNUSED = 0,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_SLEEPING,
    TASK_STATE_ZOMBIE,
    TASK_STATE_REAPED,
    TASK_STATE_BLOCKED = TASK_STATE_SLEEPING,
    TASK_STATE_TERMINATED = TASK_STATE_ZOMBIE
} task_state_t;

typedef struct task {
    pid_t pid;
    char name[TASK_NAME_LEN];
    task_state_t state;
    uint64_t creation_tick;
    uint64_t wake_tick;
    union {
        uint64_t ticks_used;
        uint64_t runtime_ticks;
    };
    uint64_t context_switches;
    uint32_t slice_remaining;
    uint32_t priority;
    void (*entry_point)(void);
    uintptr_t kernel_stack_pointer;
    uintptr_t kernel_stack_base;
    size_t kernel_stack_size;
    struct task* run_queue_next;
    bool on_run_queue;
    bool is_idle;
} task_t;

void task_init(void);
task_t* task_create(const char* name, void (*entry_point)(void));
task_t* task_create_idle(void);
task_t* task_get_by_pid(pid_t pid);
task_t* task_get_at(size_t index);
task_t* task_get_current(void);
task_t* task_get_idle(void);
bool task_set_state(pid_t pid, task_state_t state);
bool task_terminate(pid_t pid);
void task_exit(void) __attribute__((noreturn));
void task_yield(void);
bool task_sleep_ticks(uint64_t ticks);
void task_timer_tick(void);
void task_interrupt_exit(void);
uint32_t task_get_count(void);
const char* task_state_to_string(task_state_t state);

#endif // ROCKOS_TASK_H