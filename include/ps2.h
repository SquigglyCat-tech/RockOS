#ifndef ROCKOS_PS2_H
#define ROCKOS_PS2_H

#include <stdint.h>
#include <stdbool.h>

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_STATUS_OBF   0x01 // Output Buffer Full
#define PS2_STATUS_IBF   0x02 // Input Buffer Full
#define PS2_STATUS_AUX   0x20 // Mouse / Auxiliary Data

#define PS2_ACK          0xFA

bool ps2_wait_write(void);
bool ps2_wait_read(void);
uint8_t ps2_read_data(void);
void ps2_write_command(uint8_t cmd);
void ps2_write_data(uint8_t data);
bool ps2_send_mouse_cmd(uint8_t cmd);
void ps2_init(void);

#endif // ROCKOS_PS2_H