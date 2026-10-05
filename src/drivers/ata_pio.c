#include "storage.h"

#include "io.h"
#include "pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATA_DATA_PORT       0x1F0u
#define ATA_SECTOR_COUNT    0x1F2u
#define ATA_LBA_LOW         0x1F3u
#define ATA_LBA_MID         0x1F4u
#define ATA_LBA_HIGH        0x1F5u
#define ATA_DRIVE_SELECT    0x1F6u
#define ATA_STATUS_COMMAND  0x1F7u
#define ATA_ALT_STATUS_CTRL 0x3F6u

#define ATA_STATUS_ERROR 0x01u
#define ATA_STATUS_DRQ   0x08u
#define ATA_STATUS_FAULT 0x20u
#define ATA_STATUS_BUSY  0x80u
#define ATA_CONTROL_NIEN 0x02u

#define ATA_COMMAND_IDENTIFY 0xECu
#define ATA_COMMAND_READ     0x20u
#define ATA_COMMAND_WRITE    0x30u
#define ATA_COMMAND_FLUSH    0xE7u

#define ATA_POLL_LIMIT 1000000U
#define ATA_WORDS_PER_SECTOR (BLOCK_SECTOR_SIZE / sizeof(uint16_t))
#define ATA_LBA28_SECTOR_LIMIT 0x10000000ULL

typedef struct {
    block_device_t block;
    uint8_t drive_select;
    bool ready;
} ata_drive_t;

static ata_drive_t ata_drives[2];
static uint16_t identify_words[256];
static uint8_t test_write_buffer[BLOCK_SECTOR_SIZE];
static uint8_t test_read_buffer[BLOCK_SECTOR_SIZE];
static size_t ata_device_count;
static bool ata_busy;
static block_result_t initialization_result = BLOCK_RESULT_NOT_INITIALIZED;

static void ata_delay_400ns(void) {
    (void)inb(ATA_ALT_STATUS_CTRL);
    (void)inb(ATA_ALT_STATUS_CTRL);
    (void)inb(ATA_ALT_STATUS_CTRL);
    (void)inb(ATA_ALT_STATUS_CTRL);
}

static block_result_t ata_wait_status(bool require_data) {
    for (uint32_t attempt = 0; attempt < ATA_POLL_LIMIT; attempt++) {
        uint8_t status = inb(ATA_STATUS_COMMAND);
        if (status == 0 || status == 0xFFu) return BLOCK_RESULT_NO_DEVICE;
        if (status & (ATA_STATUS_ERROR | ATA_STATUS_FAULT)) {
            return BLOCK_RESULT_DEVICE_ERROR;
        }
        if (!(status & ATA_STATUS_BUSY) &&
            (!require_data || (status & ATA_STATUS_DRQ))) {
            return BLOCK_RESULT_OK;
        }
    }
    return BLOCK_RESULT_TIMEOUT;
}

static bool ata_acquire(void) {
    uint64_t flags = save_irq_disable();
    bool acquired = !ata_busy;
    if (acquired) ata_busy = true;
    restore_irq(flags);
    return acquired;
}

static void ata_release(void) {
    uint64_t flags = save_irq_disable();
    ata_busy = false;
    restore_irq(flags);
}

static block_result_t ata_select_lba(const ata_drive_t* drive, uint32_t lba,
    uint32_t sector_count) {
    if (sector_count == 0 || sector_count > BLOCK_MAX_TRANSFER_SECTORS) {
        return BLOCK_RESULT_INVALID_ARGUMENT;
    }

    outb(ATA_DRIVE_SELECT,
        (uint8_t)(drive->drive_select | ((lba >> 24) & 0x0Fu)));
    ata_delay_400ns();
    block_result_t result = ata_wait_status(false);
    if (result != BLOCK_RESULT_OK) return result;

    outb(ATA_SECTOR_COUNT, (uint8_t)sector_count);
    outb(ATA_LBA_LOW, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    return BLOCK_RESULT_OK;
}

static block_result_t ata_read(void* context, uint64_t lba,
    uint32_t sector_count, void* buffer) {
    ata_drive_t* drive = (ata_drive_t*)context;
    if (!drive || !drive->ready) return BLOCK_RESULT_NOT_INITIALIZED;
    if (!ata_acquire()) return BLOCK_RESULT_BUSY;

    uint8_t* bytes = (uint8_t*)buffer;
    block_result_t result = BLOCK_RESULT_OK;
    result = ata_select_lba(drive, (uint32_t)lba, sector_count);
    if (result != BLOCK_RESULT_OK) {
        ata_release();
        return result;
    }

    outb(ATA_STATUS_COMMAND, ATA_COMMAND_READ);
    for (uint32_t sector = 0; sector < sector_count; sector++) {
        result = ata_wait_status(true);
        if (result != BLOCK_RESULT_OK) break;

        size_t byte_offset = (size_t)sector * BLOCK_SECTOR_SIZE;
        for (uint32_t word = 0; word < ATA_WORDS_PER_SECTOR; word++) {
            uint16_t value = inw(ATA_DATA_PORT);
            bytes[byte_offset + word * 2] = (uint8_t)value;
            bytes[byte_offset + word * 2 + 1] = (uint8_t)(value >> 8);
        }

    }

    if (result == BLOCK_RESULT_OK) {
        result = ata_wait_status(false);
    }
    ata_release();
    return result;
}

static block_result_t ata_write(void* context, uint64_t lba,
    uint32_t sector_count, const void* buffer) {
    ata_drive_t* drive = (ata_drive_t*)context;
    if (!drive || !drive->ready) return BLOCK_RESULT_NOT_INITIALIZED;
    if (!ata_acquire()) return BLOCK_RESULT_BUSY;

    const uint8_t* bytes = (const uint8_t*)buffer;
    block_result_t result = BLOCK_RESULT_OK;
    result = ata_select_lba(drive, (uint32_t)lba, sector_count);
    if (result != BLOCK_RESULT_OK) {
        ata_release();
        return result;
    }

    outb(ATA_STATUS_COMMAND, ATA_COMMAND_WRITE);
    for (uint32_t sector = 0; sector < sector_count; sector++) {
        result = ata_wait_status(true);
        if (result != BLOCK_RESULT_OK) break;

        size_t byte_offset = (size_t)sector * BLOCK_SECTOR_SIZE;
        for (uint32_t word = 0; word < ATA_WORDS_PER_SECTOR; word++) {
            uint16_t value = (uint16_t)bytes[byte_offset + word * 2] |
                ((uint16_t)bytes[byte_offset + word * 2 + 1] << 8);
            outw(ATA_DATA_PORT, value);
        }

    }

    if (result == BLOCK_RESULT_OK) {
        result = ata_wait_status(false);
        if (result == BLOCK_RESULT_OK) {
            outb(ATA_STATUS_COMMAND, ATA_COMMAND_FLUSH);
            result = ata_wait_status(false);
        }
    }

    ata_release();
    return result;
}

static block_result_t ata_identify(ata_drive_t* drive, uint8_t drive_number) {
    drive->ready = false;
    drive->block.sector_count = 0;
    drive->drive_select = (uint8_t)(0xE0u | (drive_number << 4));

    outb(ATA_ALT_STATUS_CTRL, ATA_CONTROL_NIEN);
    outb(ATA_DRIVE_SELECT, (uint8_t)(0xA0u | (drive_number << 4)));
    ata_delay_400ns();

    uint8_t status = inb(ATA_STATUS_COMMAND);
    if (status == 0 || status == 0xFFu) return BLOCK_RESULT_NO_DEVICE;
    block_result_t result = ata_wait_status(false);
    if (result != BLOCK_RESULT_OK) return result;

    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_STATUS_COMMAND, ATA_COMMAND_IDENTIFY);

    bool data_ready = false;
    for (uint32_t attempt = 0; attempt < ATA_POLL_LIMIT; attempt++) {
        status = inb(ATA_STATUS_COMMAND);
        if (status == 0 || status == 0xFFu) return BLOCK_RESULT_NO_DEVICE;
        if (status & ATA_STATUS_BUSY) continue;
        if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0) {
            return BLOCK_RESULT_UNSUPPORTED;
        }
        if (status & (ATA_STATUS_ERROR | ATA_STATUS_FAULT)) {
            return BLOCK_RESULT_DEVICE_ERROR;
        }
        if (status & ATA_STATUS_DRQ) {
            data_ready = true;
            break;
        }
    }
    if (!data_ready) return BLOCK_RESULT_TIMEOUT;

    for (uint32_t word = 0; word < 256; word++) {
        identify_words[word] = inw(ATA_DATA_PORT);
    }

    if (!(identify_words[49] & (1u << 9))) return BLOCK_RESULT_UNSUPPORTED;
    uint64_t sectors = (uint64_t)identify_words[60] |
        ((uint64_t)identify_words[61] << 16);
    if (sectors == 0 || sectors > ATA_LBA28_SECTOR_LIMIT) {
        return BLOCK_RESULT_UNSUPPORTED;
    }

    drive->block.name = drive_number == 0
        ? "ata0-primary-master" : "ata0-primary-slave";
    drive->block.sector_size = BLOCK_SECTOR_SIZE;
    drive->block.sector_count = sectors;
    drive->block.context = drive;
    drive->block.read_sectors = ata_read;
    drive->block.write_sectors = ata_write;
    drive->ready = true;
    return BLOCK_RESULT_OK;
}

block_result_t storage_init(void) {
    ata_busy = false;
    ata_device_count = 0;
    ata_drives[0].ready = false;
    ata_drives[0].block.sector_count = 0;
    ata_drives[1].ready = false;
    ata_drives[1].block.sector_count = 0;
    initialization_result = BLOCK_RESULT_NOT_INITIALIZED;

    if (!pci_was_scanned()) {
        initialization_result = BLOCK_RESULT_NOT_INITIALIZED;
        return initialization_result;
    }

    const pci_device_t* ide = pci_find_by_class(0x01u, 0x01u);
    if (!ide) {
        initialization_result = BLOCK_RESULT_NO_DEVICE;
        return initialization_result;
    }
    if (ide->prog_if & 0x01u) {
        initialization_result = BLOCK_RESULT_UNSUPPORTED;
        return initialization_result;
    }

    initialization_result = ata_identify(&ata_drives[0], 0);
    if (initialization_result == BLOCK_RESULT_OK) ata_device_count++;
    block_result_t slave_result = ata_identify(&ata_drives[1], 1);
    if (slave_result == BLOCK_RESULT_OK) ata_device_count++;
    return initialization_result;
}

block_result_t storage_initialization_result(void) {
    return initialization_result;
}

const block_device_t* storage_get_device(void) {
    return ata_drives[0].ready ? &ata_drives[0].block : NULL;
}

size_t storage_get_device_count(void) {
    return ata_device_count;
}

const block_device_t* storage_get_device_at(size_t index) {
    for (size_t drive_index = 0; drive_index < 2; drive_index++) {
        if (!ata_drives[drive_index].ready) continue;
        if (index == 0) return &ata_drives[drive_index].block;
        index--;
    }
    return NULL;
}

block_result_t storage_self_test(void) {
    const block_device_t* device = storage_get_device();
    if (!device) return BLOCK_RESULT_NOT_INITIALIZED;
    if (device->sector_count <= STORAGE_TEST_RESERVED_SECTORS) {
        return BLOCK_RESULT_OUT_OF_RANGE;
    }

    for (uint32_t index = 0; index < BLOCK_SECTOR_SIZE; index++) {
        test_write_buffer[index] = (uint8_t)(index ^ (index >> 8) ^ 0xA5u);
        test_read_buffer[index] = 0;
    }

    uint64_t test_lba = device->sector_count - STORAGE_TEST_RESERVED_SECTORS;
    block_result_t result = block_read(device, test_lba, 1, test_read_buffer);
    if (result != BLOCK_RESULT_OK) return result;

    bool empty_region = true;
    bool previous_test_pattern = true;
    for (uint32_t index = 0; index < BLOCK_SECTOR_SIZE; index++) {
        if (test_read_buffer[index] != 0) empty_region = false;
        if (test_read_buffer[index] != test_write_buffer[index]) {
            previous_test_pattern = false;
        }
    }
    if (!empty_region && !previous_test_pattern) {
        return BLOCK_RESULT_TEST_REGION_DIRTY;
    }

    result = block_write(device, test_lba, 1, test_write_buffer);
    if (result != BLOCK_RESULT_OK) return result;
    result = block_read(device, test_lba, 1, test_read_buffer);
    if (result != BLOCK_RESULT_OK) return result;

    for (uint32_t index = 0; index < BLOCK_SECTOR_SIZE; index++) {
        if (test_read_buffer[index] != test_write_buffer[index]) {
            return BLOCK_RESULT_VERIFY_FAILED;
        }
    }
    return BLOCK_RESULT_OK;
}