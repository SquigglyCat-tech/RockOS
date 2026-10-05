#include "storage.h"

#include "io.h"
#include "pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* v86 / QEMU compatible: both channels, legacy probe fallback. */
#define ATA_PRIMARY_DATA      0x1F0u
#define ATA_PRIMARY_SECTORS   0x1F2u
#define ATA_PRIMARY_LBA_LO    0x1F3u
#define ATA_PRIMARY_LBA_MID   0x1F4u
#define ATA_PRIMARY_LBA_HI    0x1F5u
#define ATA_PRIMARY_DRIVE     0x1F6u
#define ATA_PRIMARY_CMD       0x1F7u
#define ATA_PRIMARY_CTRL      0x3F6u

#define ATA_SECONDARY_DATA    0x170u
#define ATA_SECONDARY_SECTORS 0x172u
#define ATA_SECONDARY_LBA_LO  0x173u
#define ATA_SECONDARY_LBA_MID 0x174u
#define ATA_SECONDARY_LBA_HI  0x175u
#define ATA_SECONDARY_DRIVE   0x176u
#define ATA_SECONDARY_CMD     0x177u
#define ATA_SECONDARY_CTRL    0x376u

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
    uint16_t data;
    uint16_t sectors;
    uint16_t lba_lo;
    uint16_t lba_mid;
    uint16_t lba_hi;
    uint16_t drive;
    uint16_t cmd;
    uint16_t ctrl;
} ata_channel_t;

static const ata_channel_t ata_channels[2] = {
    { ATA_PRIMARY_DATA, ATA_PRIMARY_SECTORS, ATA_PRIMARY_LBA_LO,
      ATA_PRIMARY_LBA_MID, ATA_PRIMARY_LBA_HI, ATA_PRIMARY_DRIVE,
      ATA_PRIMARY_CMD, ATA_PRIMARY_CTRL },
    { ATA_SECONDARY_DATA, ATA_SECONDARY_SECTORS, ATA_SECONDARY_LBA_LO,
      ATA_SECONDARY_LBA_MID, ATA_SECONDARY_LBA_HI, ATA_SECONDARY_DRIVE,
      ATA_SECONDARY_CMD, ATA_SECONDARY_CTRL },
};

static const char* const ata_drive_names[4] = {
    "ata0-primary-master", "ata0-primary-slave",
    "ata1-secondary-master", "ata1-secondary-slave",
};

typedef struct {
    block_device_t block;
    uint8_t channel;
    uint8_t drive_select;
    bool ready;
} ata_drive_t;

static ata_drive_t ata_drives[4];
static uint16_t identify_words[256];
static uint8_t test_write_buffer[BLOCK_SECTOR_SIZE];
static uint8_t test_read_buffer[BLOCK_SECTOR_SIZE];
static size_t ata_device_count;
static bool ata_busy;
static block_result_t initialization_result = BLOCK_RESULT_NOT_INITIALIZED;

static void ata_delay_400ns(uint8_t channel) {
    uint16_t ctrl = ata_channels[channel & 1u].ctrl;
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
}

static block_result_t ata_wait_status(uint8_t channel, bool require_data) {
    uint16_t cmd = ata_channels[channel & 1u].cmd;
    for (uint32_t attempt = 0; attempt < ATA_POLL_LIMIT; attempt++) {
        uint8_t status = inb(cmd);
        /* Floating bus (no controller/drive): 0xFF. v86 secondary with
           nothing attached can also read 0x00. Both mean "no device". */
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

    const ata_channel_t* ch = &ata_channels[drive->channel & 1u];
    outb(ch->drive,
        (uint8_t)(drive->drive_select | ((lba >> 24) & 0x0Fu)));
    ata_delay_400ns(drive->channel);
    block_result_t result = ata_wait_status(drive->channel, false);
    if (result != BLOCK_RESULT_OK) return result;

    outb(ch->sectors, (uint8_t)sector_count);
    outb(ch->lba_lo, (uint8_t)lba);
    outb(ch->lba_mid, (uint8_t)(lba >> 8));
    outb(ch->lba_hi, (uint8_t)(lba >> 16));
    return BLOCK_RESULT_OK;
}

static block_result_t ata_read(void* context, uint64_t lba,
    uint32_t sector_count, void* buffer) {
    ata_drive_t* drive = (ata_drive_t*)context;
    if (!drive || !drive->ready) return BLOCK_RESULT_NOT_INITIALIZED;
    if (!ata_acquire()) return BLOCK_RESULT_BUSY;

    const ata_channel_t* ch = &ata_channels[drive->channel & 1u];
    uint8_t* bytes = (uint8_t*)buffer;
    block_result_t result = BLOCK_RESULT_OK;
    result = ata_select_lba(drive, (uint32_t)lba, sector_count);
    if (result != BLOCK_RESULT_OK) {
        ata_release();
        return result;
    }

    outb(ch->cmd, ATA_COMMAND_READ);
    for (uint32_t sector = 0; sector < sector_count; sector++) {
        result = ata_wait_status(drive->channel, true);
        if (result != BLOCK_RESULT_OK) break;

        size_t byte_offset = (size_t)sector * BLOCK_SECTOR_SIZE;
        for (uint32_t word = 0; word < ATA_WORDS_PER_SECTOR; word++) {
            uint16_t value = inw(ch->data);
            bytes[byte_offset + word * 2] = (uint8_t)value;
            bytes[byte_offset + word * 2 + 1] = (uint8_t)(value >> 8);
        }

    }

    if (result == BLOCK_RESULT_OK) {
        result = ata_wait_status(drive->channel, false);
    }
    ata_release();
    return result;
}

static block_result_t ata_write(void* context, uint64_t lba,
    uint32_t sector_count, const void* buffer) {
    ata_drive_t* drive = (ata_drive_t*)context;
    if (!drive || !drive->ready) return BLOCK_RESULT_NOT_INITIALIZED;
    if (!ata_acquire()) return BLOCK_RESULT_BUSY;

    const ata_channel_t* ch = &ata_channels[drive->channel & 1u];
    const uint8_t* bytes = (const uint8_t*)buffer;
    block_result_t result = BLOCK_RESULT_OK;
    result = ata_select_lba(drive, (uint32_t)lba, sector_count);
    if (result != BLOCK_RESULT_OK) {
        ata_release();
        return result;
    }

    outb(ch->cmd, ATA_COMMAND_WRITE);
    for (uint32_t sector = 0; sector < sector_count; sector++) {
        result = ata_wait_status(drive->channel, true);
        if (result != BLOCK_RESULT_OK) break;

        size_t byte_offset = (size_t)sector * BLOCK_SECTOR_SIZE;
        for (uint32_t word = 0; word < ATA_WORDS_PER_SECTOR; word++) {
            uint16_t value = (uint16_t)bytes[byte_offset + word * 2] |
                ((uint16_t)bytes[byte_offset + word * 2 + 1] << 8);
            outw(ch->data, value);
        }

    }

    if (result == BLOCK_RESULT_OK) {
        /* Wait for completion, then best-effort flush. Some emulators
           (including v86) are picky about FLUSH timing, so never fail
           an otherwise good write because the flush handshake is odd. */
        result = ata_wait_status(drive->channel, false);
        if (result == BLOCK_RESULT_OK) {
            outb(ch->cmd, ATA_COMMAND_FLUSH);
            block_result_t flush = ata_wait_status(drive->channel, false);
            if (flush == BLOCK_RESULT_NO_DEVICE) flush = BLOCK_RESULT_OK;
            if (flush != BLOCK_RESULT_TIMEOUT) result = flush;
            else result = BLOCK_RESULT_OK;
        }
    }

    ata_release();
    return result;
}

static block_result_t ata_identify(ata_drive_t* drive, uint8_t channel,
    uint8_t drive_number) {
    drive->ready = false;
    drive->block.sector_count = 0;
    drive->channel = (uint8_t)(channel & 1u);
    drive->drive_select = (uint8_t)(0xE0u | (drive_number << 4));
    const ata_channel_t* ch = &ata_channels[drive->channel];

    outb(ch->ctrl, ATA_CONTROL_NIEN);
    outb(ch->drive, (uint8_t)(0xA0u | (drive_number << 4)));
    ata_delay_400ns(drive->channel);

    uint8_t status = inb(ch->cmd);
    if (status == 0 || status == 0xFFu) return BLOCK_RESULT_NO_DEVICE;

    /* Tolerate a stale BSY from a previous select on slow emulators:
       wait for not-busy, but treat float/no-device as empty. */
    block_result_t result = ata_wait_status(drive->channel, false);
    if (result == BLOCK_RESULT_NO_DEVICE) return BLOCK_RESULT_NO_DEVICE;
    if (result == BLOCK_RESULT_TIMEOUT) return BLOCK_RESULT_NO_DEVICE;
    if (result != BLOCK_RESULT_OK) return result;

    outb(ch->sectors, 0);
    outb(ch->lba_lo, 0);
    outb(ch->lba_mid, 0);
    outb(ch->lba_hi, 0);
    outb(ch->cmd, ATA_COMMAND_IDENTIFY);

    bool data_ready = false;
    for (uint32_t attempt = 0; attempt < ATA_POLL_LIMIT; attempt++) {
        status = inb(ch->cmd);
        if (status == 0 || status == 0xFFu) return BLOCK_RESULT_NO_DEVICE;
        if (status & ATA_STATUS_BUSY) continue;
        /* Nonzero LBA mid/high signature after IDENTIFY = ATAPI (packet)
           device, typically the CD-ROM. Skip it, keep probing others. */
        if (inb(ch->lba_mid) != 0 || inb(ch->lba_hi) != 0) {
            return BLOCK_RESULT_NO_DEVICE;
        }
        if (status & (ATA_STATUS_ERROR | ATA_STATUS_FAULT)) {
            return BLOCK_RESULT_NO_DEVICE;
        }
        if (status & ATA_STATUS_DRQ) {
            data_ready = true;
            break;
        }
    }
    if (!data_ready) return BLOCK_RESULT_NO_DEVICE;

    for (uint32_t word = 0; word < 256; word++) {
        identify_words[word] = inw(ch->data);
    }

    if (!(identify_words[49] & (1u << 9))) return BLOCK_RESULT_UNSUPPORTED;
    uint64_t sectors = (uint64_t)identify_words[60] |
        ((uint64_t)identify_words[61] << 16);
    if (sectors == 0 || sectors > ATA_LBA28_SECTOR_LIMIT) {
        return BLOCK_RESULT_UNSUPPORTED;
    }

    size_t slot = (size_t)drive->channel * 2 + (drive_number & 1u);
    drive->block.name = ata_drive_names[slot & 3u];
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
    for (size_t i = 0; i < 4; i++) {
        ata_drives[i].ready = false;
        ata_drives[i].block.sector_count = 0;
    }
    initialization_result = BLOCK_RESULT_NOT_INITIALIZED;

    /* v86 presents a PIIX3 IDE controller (class 01/01, prog_if 0x80+),
       QEMU does too. Never gate on prog_if: always try the legacy
       0x1F0/0x170 ports directly. PCI scan is only a hint now. */
    if (pci_was_scanned()) {
        (void)pci_find_by_class(0x01u, 0x01u);
    }

    /* Probe order keeps old names stable: primary master first, then
       primary slave, then secondary master/slave. */
    static const uint8_t probe_order[4][2] = {
        { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 },
    };
    initialization_result = BLOCK_RESULT_NO_DEVICE;
    for (size_t i = 0; i < 4; i++) {
        uint8_t channel = probe_order[i][0];
        uint8_t slave = probe_order[i][1];
        size_t slot = (size_t)channel * 2 + slave;
        block_result_t r = ata_identify(&ata_drives[slot], channel, slave);
        if (r == BLOCK_RESULT_OK) {
            ata_device_count++;
            if (initialization_result != BLOCK_RESULT_OK) {
                initialization_result = BLOCK_RESULT_OK;
            }
        }
    }
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
    for (size_t drive_index = 0; drive_index < 4; drive_index++) {
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