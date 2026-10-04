#ifndef ROCKOS_MOUSE_H
#define ROCKOS_MOUSE_H

#include "input.h"

#include <stdint.h>

void mouse_init(void);
void mouse_irq_handler(void);
void mouse_get_state(input_mouse_state_t* out_state);
void mouse_set_bounds(uint32_t width, uint32_t height);

#endif // ROCKOS_MOUSE_H