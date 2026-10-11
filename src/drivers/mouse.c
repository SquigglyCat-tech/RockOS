#include "mouse.h"
#include "ps2.h"
#include "io.h"
#include "serial.h"

#include <stdint.h>

/* Debug logging of mouse init + the first raw bytes to COM1 (off by default;
 * build with -DMOUSE_DEBUG_SERIAL=1 or change this to 1 to bring it back). */
#ifndef MOUSE_DEBUG_SERIAL
#define MOUSE_DEBUG_SERIAL 0
#endif

static input_mouse_state_t current_state = {0, 0, false, false, false};
static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];
/* True once we have seen a 4-byte (wheel) packet stream. v86 sends the extra
 * z byte even though the guest never switched the mouse to wheel mode. */
static bool mouse_wheel_mode = false;

/* ---- Pointer speed ------------------------------------------------------
 * MOUSE_GAIN_PERCENT: 100 = raw PS/2 counts, 150 = 1.5x faster, etc.
 * Acceleration: quick movements get extra gain so you can sweep across a
 * 1024x768 screen without dragging the mouse across the whole desk.
 * Tune these three numbers to taste. */
#ifndef MOUSE_GAIN_PERCENT
#define MOUSE_GAIN_PERCENT 150
#endif
#define MOUSE_ACCEL_MEDIUM_COUNTS 5   /* |delta| >= this: 1.5x extra */
#define MOUSE_ACCEL_FAST_COUNTS 12    /* |delta| >= this: 2x extra   */

/* Sub-pixel leftovers so slow movement is not lost to rounding. */
static int32_t mouse_x_remainder = 0;
static int32_t mouse_y_remainder = 0;

static int32_t mouse_scale(int32_t delta, int32_t* remainder) {
    int32_t magnitude = delta < 0 ? -delta : delta;
    int32_t gain = MOUSE_GAIN_PERCENT;
    if (magnitude >= MOUSE_ACCEL_FAST_COUNTS) {
        gain = gain * 2;
    } else if (magnitude >= MOUSE_ACCEL_MEDIUM_COUNTS) {
        gain = (gain * 3) / 2;
    }
    int32_t scaled = delta * gain + *remainder;
    int32_t step = scaled / 100;
    *remainder = scaled - step * 100;
    return step;
}
static uint32_t mouse_max_x = 639;
static uint32_t mouse_max_y = 159;

void mouse_init(void) {
    uint64_t flags = save_irq_disable();
    current_state.x = 0;
    current_state.y = 0;
    current_state.left_btn = false;
    current_state.right_btn = false;
    current_state.middle_btn = false;
    mouse_cycle = 0;
    mouse_wheel_mode = false;
    mouse_x_remainder = 0;
    mouse_y_remainder = 0;
    restore_irq(flags);

    // Command 0xF6: Set Defaults
    bool defaults_ok = false;
    uint32_t retries = 3;
    while (retries-- && !(defaults_ok = ps2_send_mouse_cmd(0xF6))) {
        io_wait();
    }

    // Command 0xF4: Enable Data Reporting
    bool enable_ok = false;
    retries = 3;
    while (retries-- && !(enable_ok = ps2_send_mouse_cmd(0xF4))) {
        io_wait();
    }
#if MOUSE_DEBUG_SERIAL
    serial_write_string(defaults_ok ? "[mouse] set-defaults ACK ok\n"
                                    : "[mouse] set-defaults NO ACK\n");
    serial_write_string(enable_ok ? "[mouse] enable-reporting ACK ok\n"
                                  : "[mouse] enable-reporting NO ACK\n");
#endif
}

void mouse_irq_handler(void) {
    uint8_t status = inb(PS2_STATUS_PORT);

    if ((status & PS2_STATUS_OBF) == 0) return;
    if ((status & PS2_STATUS_AUX) == 0) return; // Ignore keyboard data

    uint8_t data = inb(PS2_DATA_PORT);
#if MOUSE_DEBUG_SERIAL
    {
        static uint32_t logged_bytes;
        if (logged_bytes < 12) {
            logged_bytes++;
            serial_write_string("[mouse] irq byte=");
            serial_write_hex(data);
            serial_write_string("\n");
        }
    }
#endif

    /* After a 3-byte packet we do not know yet if a z byte follows. A byte
     * without bit 3 can never start a packet, so it must be the z byte. */
    if (mouse_cycle == 4) {
        mouse_cycle = 0;
        if (!(data & 0x08)) {
            mouse_wheel_mode = true;
            return;
        }
    }

    switch (mouse_cycle) {
        case 0:
            if (data & 0x08) { // Packet sync bit
                mouse_packet[0] = data;
                mouse_cycle = 1;
            }
            break;

        case 1:
            mouse_packet[1] = data;
            mouse_cycle = 2;
            break;

        case 2:
            mouse_packet[2] = data;

            bool x_overflow = (mouse_packet[0] & 0x40) != 0;
            bool y_overflow = (mouse_packet[0] & 0x80) != 0;

            bool left = (mouse_packet[0] & 0x01) != 0;
            bool right = (mouse_packet[0] & 0x02) != 0;
            bool middle = (mouse_packet[0] & 0x04) != 0;

            int32_t x_mov = 0;
            int32_t y_mov = 0;

            if (!x_overflow) {
                x_mov = mouse_packet[1];
                if (mouse_packet[0] & 0x10) x_mov |= 0xFFFFFF00;
            }

            if (!y_overflow) {
                y_mov = mouse_packet[2];
                if (mouse_packet[0] & 0x20) y_mov |= 0xFFFFFF00;
            }

            x_mov = mouse_scale(x_mov, &mouse_x_remainder);
            y_mov = mouse_scale(y_mov, &mouse_y_remainder);

            uint64_t flags = save_irq_disable();
            bool was_left = current_state.left_btn;
            current_state.left_btn = left;
            current_state.right_btn = right;
            current_state.middle_btn = middle;
            int64_t next_x = (int64_t)current_state.x + x_mov;
            int64_t next_y = (int64_t)current_state.y - y_mov;
            if (next_x < 0) next_x = 0;
            if ((uint64_t)next_x > mouse_max_x) next_x = mouse_max_x;
            if (next_y < 0) next_y = 0;
            if ((uint64_t)next_y > mouse_max_y) next_y = mouse_max_y;
            current_state.x = (int32_t)next_x;
            current_state.y = (int32_t)next_y;
            if (left && !was_left) {
                current_state.left_press_count++;
                current_state.press_x = current_state.x;
                current_state.press_y = current_state.y;
            } else if (!left && was_left) {
                current_state.left_release_count++;
            }
            restore_irq(flags);

            /* Wheel mice send a 4th (z) byte. If we already know that,
             * consume it next; otherwise probe for it (see above). */
            mouse_cycle = mouse_wheel_mode ? 3 : 4;
            break;

        case 3: /* z byte of a known 4-byte packet: ignore */
            mouse_cycle = 0;
            break;

        default:
            mouse_cycle = 0;
            break;
    }
}

void mouse_get_state(input_mouse_state_t* out_state) {
    if (!out_state) return;

    uint64_t flags = save_irq_disable();
    *out_state = current_state;
    restore_irq(flags);
}

void mouse_set_bounds(uint32_t width, uint32_t height) {
    if (!width || !height) return;
    uint64_t flags = save_irq_disable();
    mouse_max_x = width - 1;
    mouse_max_y = height - 1;
    if ((uint32_t)current_state.x > mouse_max_x) {
        current_state.x = (int32_t)mouse_max_x;
    }
    if ((uint32_t)current_state.y > mouse_max_y) {
        current_state.y = (int32_t)mouse_max_y;
    }
    restore_irq(flags);
}