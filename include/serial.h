#ifndef ROCKOS_SERIAL_H
#define ROCKOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_write_string(const char* string);
void serial_write_hex(uint64_t value);
void serial_write_dec(uint64_t value);

#endif
