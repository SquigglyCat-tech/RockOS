#ifndef ROCKOS_TIMER_H
#define ROCKOS_TIMER_H

#include <stdint.h>

void timer_init(uint32_t frequency_hz);
void timer_irq_handler(void);
uint64_t timer_get_ticks(void);

#endif // ROCKOS_TIMER_H