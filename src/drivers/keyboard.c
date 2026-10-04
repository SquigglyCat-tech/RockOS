#include "keyboard.h"
#include "ps2.h"
#include "io.h"

#define KBD_BUFFER_SIZE 256

static const char kbd_us_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,
    '*',  0, ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0, '-',   0,   0,   0, '+',   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0
};

static const char kbd_us_shift_map[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0,
    '*',  0, ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0, '-',   0,   0,   0, '+',   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0
};

static input_key_event_t event_buffer[KBD_BUFFER_SIZE];
static uint16_t buffer_head = 0;
static uint16_t buffer_tail = 0;

static uint8_t current_modifiers = 0;
static bool extended_prefix = false;

void keyboard_init(void) {
    uint64_t flags = save_irq_disable();
    buffer_head = 0;
    buffer_tail = 0;
    current_modifiers = 0;
    extended_prefix = false;
    restore_irq(flags);
}

void keyboard_irq_handler(void) {
    uint8_t status = inb(PS2_STATUS_PORT);

    // 1. Verify buffer contains data
    if ((status & PS2_STATUS_OBF) == 0) {
        return;
    }

    // 2. Reject data if it originates from mouse (AUX)
    if ((status & PS2_STATUS_AUX) != 0) {
        return;
    }

    // 3. Read scan code
    uint8_t scancode = inb(PS2_DATA_PORT);

    // 4. Handle 0xE0 prefix
    if (scancode == 0xE0) {
        extended_prefix = true;
        return;
    }

    bool is_extended = extended_prefix;
    extended_prefix = false;

    bool is_release = (scancode & 0x80) != 0;
    uint8_t raw_code = scancode & 0x7F;

    // 5. Track modifier key state
    if (!is_extended) {
        if (raw_code == 0x2A || raw_code == 0x36) { // Left/Right Shift
            if (is_release) current_modifiers &= ~MOD_SHIFT;
            else current_modifiers |= MOD_SHIFT;
        } else if (raw_code == 0x1D) { // Ctrl
            if (is_release) current_modifiers &= ~MOD_CTRL;
            else current_modifiers |= MOD_CTRL;
        } else if (raw_code == 0x38) { // Alt
            if (is_release) current_modifiers &= ~MOD_ALT;
            else current_modifiers |= MOD_ALT;
        }
    }

    // 6. Map ASCII
    char ascii = 0;
    if (!is_extended && raw_code < 128) {
        if (current_modifiers & MOD_SHIFT) {
            ascii = kbd_us_shift_map[raw_code];
        } else {
            ascii = kbd_us_map[raw_code];
        }
    }

    // 7. Enqueue event
    uint16_t next_head = (buffer_head + 1) % KBD_BUFFER_SIZE;
    if (next_head != buffer_tail) {
        event_buffer[buffer_head].scancode = raw_code;
        event_buffer[buffer_head].ascii = ascii;
        event_buffer[buffer_head].is_pressed = !is_release;
        event_buffer[buffer_head].is_extended = is_extended;
        event_buffer[buffer_head].modifiers = current_modifiers;
        buffer_head = next_head;
    }
}

bool keyboard_get_event(input_key_event_t* out_event) {
    if (!out_event) return false;

    uint64_t flags = save_irq_disable();

    if (buffer_head == buffer_tail) {
        restore_irq(flags);
        return false;
    }

    *out_event = event_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % KBD_BUFFER_SIZE;

    restore_irq(flags);
    return true;
}