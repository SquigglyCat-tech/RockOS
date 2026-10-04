#include "block.h"

static block_result_t validate_request(const block_device_t* device,
    uint64_t lba, uint32_t sector_count, const void* buffer) {
    if (!device || !buffer || sector_count == 0 ||
        sector_count > BLOCK_MAX_TRANSFER_SECTORS) {
        return BLOCK_RESULT_INVALID_ARGUMENT;
    }
    if (device->sector_size != BLOCK_SECTOR_SIZE ||
        !device->read_sectors || !device->write_sectors) {
        return BLOCK_RESULT_NOT_INITIALIZED;
    }
    if (lba >= device->sector_count ||
        (uint64_t)sector_count > device->sector_count - lba) {
        return BLOCK_RESULT_OUT_OF_RANGE;
    }
    return BLOCK_RESULT_OK;
}

block_result_t block_read(const block_device_t* device, uint64_t lba,
    uint32_t sector_count, void* buffer) {
    block_result_t result = validate_request(device, lba, sector_count, buffer);
    if (result != BLOCK_RESULT_OK) return result;
    return device->read_sectors(device->context, lba, sector_count, buffer);
}

block_result_t block_write(const block_device_t* device, uint64_t lba,
    uint32_t sector_count, const void* buffer) {
    block_result_t result = validate_request(device, lba, sector_count, buffer);
    if (result != BLOCK_RESULT_OK) return result;
    return device->write_sectors(device->context, lba, sector_count, buffer);
}

const char* block_result_string(block_result_t result) {
    switch (result) {
        case BLOCK_RESULT_OK: return "OK";
        case BLOCK_RESULT_INVALID_ARGUMENT: return "invalid request";
        case BLOCK_RESULT_NOT_INITIALIZED: return "not initialized";
        case BLOCK_RESULT_NO_DEVICE: return "no device/controller found";
        case BLOCK_RESULT_UNSUPPORTED: return "unsupported IDE mode or disk";
        case BLOCK_RESULT_OUT_OF_RANGE: return "LBA outside device capacity";
        case BLOCK_RESULT_BUSY: return "device busy";
        case BLOCK_RESULT_TIMEOUT: return "device timeout";
        case BLOCK_RESULT_DEVICE_ERROR: return "device reported an error";
        case BLOCK_RESULT_TEST_REGION_DIRTY: return "reserved test sector contains unexpected data";
        case BLOCK_RESULT_VERIFY_FAILED: return "read-back verification failed";
        default: return "unknown storage error";
    }
}