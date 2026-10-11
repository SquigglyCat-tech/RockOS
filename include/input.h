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
    /* Latched left-button events, so a click that starts and ends between two
     * polls of the main loop is never lost. The counters only ever increase;
     * compare against the value you saw last time. press_x/press_y is where
     * the pointer was when the latest press happened. */
    uint32_t left_press_count;
    uint32_t left_release_count;
    int32_t press_x;
    int32_t press_y;
} input_mouse_state_t;

#endif // ROCKOS_INPUT_H