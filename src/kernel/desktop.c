#ifndef ROCKOS_DESKTOP_H
#define ROCKOS_DESKTOP_H

#include "input.h"

#include <stdbool.h>
#include <stdint.h>

/* RockOS V.2 desktop: icons, taskbar, start menu, draggable windows. */

void desktop_start(void);
void desktop_stop(void);
bool desktop_is_active(void);

/* Call once per main-loop pass while the desktop is active. */
void desktop_handle_mouse(int32_t x, int32_t y, bool left_down);
void desktop_handle_key(const input_key_event_t* event);
void desktop_update(uint64_t ticks);

/* True for the key combo that launches the desktop (Ctrl+Alt+D). */
bool desktop_is_launch_hotkey(const input_key_event_t* event);

#endif // ROCKOS_DESKTOP_H