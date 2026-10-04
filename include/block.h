#ifndef ROCKOS_BLOCK_H
#define ROCKOS_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512U
#define BLOCK_MAX_TRANSFER_SECTORS 128U

typedef enum {
    BLOCK_RESULT_OK = 0,
    BLOCK_RESULT_INVALID_ARGUMENT,
    BLOCK_RESULT_NOT_INITIALIZED,
    BLOCK_RESULT_NO_DEVICE,
    BLOCK_RESULT_UNSUPPORTED,
    BLOCK_RESULT_OUT_OF_RANGE,
    BLOCK_RESULT_BUSY,
    BLOCK_RESULT_TIMEOUT,
    BLOCK_RESULT_DEVICE_ERROR,
    BLOCK_RESULT_TEST_REGION_DIRTY,
    BLOCK_RESULT_VERIFY_FAILED
} block_result_t;

typedef block_result_t (*block_read_fn)(void* context, uint64_t lba,
    uint32_t sector_count, void* buffer);
typedef block_result_t (*block_write_fn)(void* context, uint64_t lba,
    uint32_t sector_count, const void* buffer);

typedef struct {
    const char* name;
    uint32_t sector_size;
    uint64_t sector_count;
    void* context;
    block_read_fn read_sectors;
    block_write_fn write_sectors;
} block_device_t;

block_result_t block_read(const block_device_t* device, uint64_t lba,
    uint32_t sector_count, void* buffer);
block_result_t block_write(const block_device_t* device, uint64_t lba,
    uint32_t sector_count, const void* buffer);
const char* block_result_string(block_result_t result);

#endif