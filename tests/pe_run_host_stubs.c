/*
 * Kernel-free stubs for pe_run_host_test: the stage/launch path needs
 * ring3 + task creation, which the host test does not exercise. */
#include "../include/ring3.h"
#include "../include/task.h"

bool ring3_pe_stage(const uint8_t* payload, uint64_t payload_size,
    uintptr_t code_base_address, uint64_t entry_offset) {
    (void)payload; (void)payload_size; (void)code_base_address;
    (void)entry_offset;
    return true;
}

void ring3_pe_task(void) {}

task_t* task_create(const char* name, void (*entry_point)(void)) {
    (void)name; (void)entry_point;
    return (task_t*)1;
}
