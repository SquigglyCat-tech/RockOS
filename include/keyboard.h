#ifndef ROCKOS_KEYBOARD_H
#define ROCKOS_KEYBOARD_H

#include "input.h"

void keyboard_init(void);
void keyboard_irq_handler(void);
bool keyboard_get_event(input_key_event_t* out_event);

#endif // ROCKOS_KEYBOARD_H