#include "block.h"
#include "filesystem.h"
#include "live.h"
#include "pmm.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int fail_allocation;

void* pmm_alloc(size_t pages) {
    if (fail_allocation) return NULL;
    return malloc(pages * (size_t)PMM_PAGE_SIZE);
}

int main(void) {
    assert(!live_cmdline_enabled(NULL));
    assert(!live_cmdline_enabled("rockos.live=1 notrockos.live"));
    assert(live_cmdline_enabled("kernel.bin\trockos.live other"));
    assert(live_cmdline_enabled("rockos.live"));

    fail_allocation = 1;
    assert(live_disk_init() == NULL);
    fail_allocation = 0;
    const block_device_t* disk = live_disk_init();
    assert(disk && disk->sector_count == FS_DISK_SECTORS);
    uint8_t input[BLOCK_SECTOR_SIZE];
    uint8_t output[BLOCK_SECTOR_SIZE];
    memset(input, 0xa5, sizeof(input));
    memset(output, 0xff, sizeof(output));
    assert(block_read(disk, 42, 1, output) == BLOCK_RESULT_OK);
    for (size_t i = 0; i < sizeof(output); i++) assert(output[i] == 0);
    assert(block_write(disk, 42, 1, input) == BLOCK_RESULT_OK);
    assert(block_read(disk, 42, 1, output) == BLOCK_RESULT_OK);
    assert(memcmp(input, output, sizeof(input)) == 0);
    assert(block_read(disk, FS_DISK_SECTORS, 1, output) == BLOCK_RESULT_OUT_OF_RANGE);
    assert(block_write(disk, FS_DISK_SECTORS - 1, 2, input) == BLOCK_RESULT_OUT_OF_RANGE);
    return 0;
}
