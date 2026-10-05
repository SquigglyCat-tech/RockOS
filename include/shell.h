#ifndef ROCKOS_SHELL_H
#define ROCKOS_SHELL_H

#include "input.h"

#include <stdint.h>

void shell_init(void);
void shell_handle_key_event(const input_key_event_t* event);
void shell_handle_mouse_event(uint32_t x, uint32_t y, bool left_click);
bool shell_is_fullscreen(void);
bool shell_is_installer_boot(void);
void shell_set_boot_info(uintptr_t boot_info_address);
void shell_redraw(void);

#endif