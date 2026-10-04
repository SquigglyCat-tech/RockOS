#ifndef ROCKOS_INPUT_H
#define ROCKOS_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#define MOD_SHIFT (1 << 0)
#define MOD_CTRL  (1 << 1)
#define MOD_ALT   (1 << 2)

typedef struct {
    uint8_t scancode;
    char ascii;          // Mapped ASCII character (0 if non-printable)
    bool is_pressed;
    bool is_extended;    // True if key was preceded by 0xE0 prefix
    uint8_t modifiers;   // Bitmask of current Shift/Ctrl/Alt state
} input_key_event_t;

typedef struct {
    int32_t x;           // Relative / Accumulated X displacement
    int32_t y;           // Relative / Accumulated Y displacement
    bool left_btn;
    bool right_btn;
    bool middle_btn;
} input_mouse_state_t;

#endif // ROCKOS_INPUT_H