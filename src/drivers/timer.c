#include "timer.h"
#include "io.h"
#include "task.h"

#define PIT_CHANNEL0_PORT  0x40
#define PIT_COMMAND_PORT   0x43
#define PIT_BASE_FREQUENCY 1193182U
#define PIT_MAX_DIVISOR    0xFFFFU

static volatile uint64_t ticks = 0;

void timer_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        return;
    }

    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;
    if (divisor > PIT_MAX_DIVISOR) {
        divisor = PIT_MAX_DIVISOR;
    }
    if (divisor == 0) {
        divisor = 1;
    }

    outb(PIT_COMMAND_PORT, 0x36);
    outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_irq_handler(void) {
    ticks++;
    task_timer_tick();
}

uint64_t timer_get_ticks(void) {
    uint64_t flags = save_irq_disable();
    uint64_t current_ticks = ticks;
    restore_irq(flags);
    return current_ticks;
}