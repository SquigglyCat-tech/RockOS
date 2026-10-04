#include "serial.h"

#include "io.h"

#define SERIAL_COM1 0x3F8

static void serial_write_char(char character) {
    for (uint32_t poll = 0; poll < 100000; poll++) {
        uint8_t line_status = inb(SERIAL_COM1 + 5);
        if (line_status == 0 || line_status == 0xFF) {
            return;
        }
        if (line_status & 0x20) {
            outb(SERIAL_COM1, (uint8_t)character);
            return;
        }
    }
}

void serial_init(void) {
    outb(SERIAL_COM1 + 1, 0x00);
    outb(SERIAL_COM1 + 3, 0x80);
    outb(SERIAL_COM1 + 0, 0x03);
    outb(SERIAL_COM1 + 1, 0x00);
    outb(SERIAL_COM1 + 3, 0x03);
    outb(SERIAL_COM1 + 2, 0xC7);
    outb(SERIAL_COM1 + 4, 0x0B);
}

void serial_write_string(const char* string) {
    while (*string) {
        if (*string == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(*string++);
    }
}

void serial_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    serial_write_string("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_write_char(digits[(value >> shift) & 0x0F]);
    }
}

void serial_write_dec(uint64_t value) {
    char digits[20];
    uint8_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));

    while (count) {
        serial_write_char(digits[--count]);
    }
}
