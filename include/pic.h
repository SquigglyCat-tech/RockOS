#ifndef ROCKOS_PIC_H
#define ROCKOS_PIC_H

#include <stdint.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_unmask_irq(uint8_t irq);
uint16_t pic_get_masks(void);

#endif // ROCKOS_PIC_H