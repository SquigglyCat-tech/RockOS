#ifndef ROCKOS_PROCESS_H
#define ROCKOS_PROCESS_H

#include "task.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_PROCESSES MAX_TASKS

typedef enum {
    PROCESS_STATE_UNUSED = 0,
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_SLEEPING,
    PROCESS_STATE_ZOMBIE
} process_state_t;

typedef struct {
    pid_t pid;
    const task_t* task;
    process_state_t state;
    uintptr_t address_space;
    uintptr_t user_stack;
    uintptr_t register_context;
    uint32_t resource_count;
} process_t;

void process_init(void);
void process_refresh(void);
bool process_activate(pid_t pid);
uintptr_t process_current_address_space(void);
void process_activate_kernel(void);
bool process_address_space_selftest(void);
process_t* process_get_by_pid(pid_t pid);
process_t* process_get_at(size_t index);
process_t* process_get_current(void);
uint32_t process_get_count(void);

#endif