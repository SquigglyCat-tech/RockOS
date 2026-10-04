#ifndef ROCKOS_TOUR_H
#define ROCKOS_TOUR_H

#include "input.h"

#include <stdbool.h>

void tour_start(void);
bool tour_is_active(void);
void tour_handle_key_event(const input_key_event_t* event);

#endif