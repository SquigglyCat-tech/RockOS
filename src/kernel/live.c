#include "live.h"
#include "filesystem.h"
#include "pmm.h"

#include <stddef.h>
#include <stdint.h>

#define LIVE_DISK_BYTES ((size_t)FS_DISK_SECTORS * BLOCK_SECTOR_SIZE)
#define LIVE_DISK_PAGES ((LIVE_DISK_BYTES + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE)

static uint8_t* live_memory;

bool live_cmdline_enabled(const char* cmdline) {
    static const char token[] = "rockos.live";
    if (!cmdline) return false;
    while (*cmdline) {
        while (*cmdline == ' ' || *cmdline == '\t') cmdline++;
        const char* start = cmdline;
        while (*cmdline && *cmdline != ' ' && *cmdline != '\t') cmdline++;
        size_t length = (size_t)(cmdline - start);
        size_t i = 0;
        while (i < length && i < sizeof(token) - 1 && start[i] == token[i]) i++;
        if (length == sizeof(token) - 1 && i == length) return true;
    }
    return false;
}

static block_result_t live_transfer(uint64_t lba, uint32_t count,
    void* buffer, bool write) {
    if (!live_memory || !buffer || count == 0) return BLOCK_RESULT_INVALID_ARGUMENT;
    if (lba >= FS_DISK_SECTORS || count > FS_DISK_SECTORS - lba) {
        return BLOCK_RESULT_OUT_OF_RANGE;
    }
    uint8_t* disk = live_memory + (size_t)lba * BLOCK_SECTOR_SIZE;
    uint8_t* bytes = buffer;
    size_t length = (size_t)count * BLOCK_SECTOR_SIZE;
    for (size_t i = 0; i < length; i++) {
        if (write) disk[i] = bytes[i];
        else bytes[i] = disk[i];
    }
    return BLOCK_RESULT_OK;
}

static block_result_t live_read(void* context, uint64_t lba,
    uint32_t count, void* buffer) {
    (void)context;
    return live_transfer(lba, count, buffer, false);
}

static block_result_t live_write(void* context, uint64_t lba,
    uint32_t count, const void* buffer) {
    (void)context;
    return live_transfer(lba, count, (void*)buffer, true);
}

const block_device_t* live_disk_init(void) {
    static const block_device_t device = {
        "live-ram", BLOCK_SECTOR_SIZE, FS_DISK_SECTORS,
        NULL, live_read, live_write
    };
    live_memory = pmm_alloc((size_t)LIVE_DISK_PAGES);
    if (!live_memory) return NULL;
    for (size_t i = 0; i < LIVE_DISK_BYTES; i++) live_memory[i] = 0;
    return &device;
}
