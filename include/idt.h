#ifndef ROCKOS_IDT_H
#define ROCKOS_IDT_H

#include <stdint.h>

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

#endif // ROCKOS_IDT_H