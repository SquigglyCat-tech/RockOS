#ifndef ROCKOS_LIVE_H
#define ROCKOS_LIVE_H

#include "block.h"
#include <stdbool.h>

/* The exact token is supplied by the live entries in the boot menu. */
bool live_cmdline_enabled(const char* cmdline);
const block_device_t* live_disk_init(void);

#endif
