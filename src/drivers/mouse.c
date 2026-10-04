#include "mouse.h"
#include "ps2.h"
#include "io.h"

#include <stdint.h>

static input_mouse_state_t current_state = {0, 0, false, false, false};
static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];
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
    restore_irq(flags);

    // Command 0xF6: Set Defaults
    uint32_t retries = 3;
    while (retries-- && !ps2_send_mouse_cmd(0xF6)) {
        io_wait();
    }

    // Command 0xF4: Enable Data Reporting
    retries = 3;
    while (retries-- && !ps2_send_mouse_cmd(0xF4)) {
        io_wait();
    }
}

void mouse_irq_handler(void) {
    uint8_t status = inb(PS2_STATUS_PORT);

    if ((status & PS2_STATUS_OBF) == 0) return;
    if ((status & PS2_STATUS_AUX) == 0) return; // Ignore keyboard data

    uint8_t data = inb(PS2_DATA_PORT);

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

            uint64_t flags = save_irq_disable();
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
            restore_irq(flags);

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