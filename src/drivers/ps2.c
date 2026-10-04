#include "ps2.h"
#include "io.h"

#define PS2_TIMEOUT 100000

bool ps2_wait_write(void) {
    uint32_t timeout = PS2_TIMEOUT;
    while (timeout--) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_IBF) == 0) {
            return true;
        }
        io_wait();
    }
    return false;
}

bool ps2_wait_read(void) {
    uint32_t timeout = PS2_TIMEOUT;
    while (timeout--) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) != 0) {
            return true;
        }
        io_wait();
    }
    return false;
}

uint8_t ps2_read_data(void) {
    return inb(PS2_DATA_PORT);
}

void ps2_write_command(uint8_t cmd) {
    if (ps2_wait_write()) {
        outb(PS2_COMMAND_PORT, cmd);
    }
}

void ps2_write_data(uint8_t data) {
    if (ps2_wait_write()) {
        outb(PS2_DATA_PORT, data);
    }
}

bool ps2_send_mouse_cmd(uint8_t cmd) {
    ps2_write_command(0xD4); // Tell controller next byte goes to second PS/2 port
    ps2_write_data(cmd);

    if (ps2_wait_read()) {
        uint8_t resp = ps2_read_data();
        return (resp == PS2_ACK);
    }
    return false;
}

void ps2_init(void) {
    // Clear PS/2 output buffer of leftover hardware data
    uint32_t flush_count = 100;
    while ((inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) && flush_count--) {
        inb(PS2_DATA_PORT);
        io_wait();
    }

    // Enable second PS/2 port (mouse)
    ps2_write_command(0xA8);

    // Read Controller Configuration Byte
    ps2_write_command(0x20);
    if (ps2_wait_read()) {
        uint8_t config = ps2_read_data();
        
        config |= 0x03;  // Bit 0 = Keyboard IRQ enable, Bit 1 = Mouse IRQ enable
        config &= ~0x20; // Bit 5 = Enable Mouse Clock

        ps2_write_command(0x60);
        ps2_write_data(config);
    }
}