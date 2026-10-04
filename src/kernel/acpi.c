#include "acpi.h"

#include "io.h"

#include <stddef.h>
#include <stdint.h>

#define ACPI_IDENTITY_MAP_LIMIT 0x40000000ULL
#define ACPI_RSDP_SCAN_START 0x000E0000ULL
#define ACPI_RSDP_SCAN_END 0x00100000ULL
#define ACPI_SMI_ENABLE_TIMEOUT 1000000U

typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct {
    uint16_t pm1a_control;
    uint16_t pm1b_control;
    uint8_t control_length;
    uint16_t smi_command;
    uint8_t acpi_enable;
    uint8_t sleep_type_a;
    uint8_t sleep_type_b;
} acpi_power_info_t;

static uint16_t read_u16(const uint8_t* bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64(const uint8_t* bytes) {
    return (uint64_t)read_u32(bytes) |
        ((uint64_t)read_u32(bytes + 4) << 32);
}

static bool range_is_mapped(uint64_t address, uint64_t length) {
    return length != 0 && address < ACPI_IDENTITY_MAP_LIMIT &&
        length <= ACPI_IDENTITY_MAP_LIMIT - address;
}

static bool signature_is(const char* actual, const char* expected,
    size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (actual[index] != expected[index]) return false;
    }
    return true;
}

static bool checksum_is_valid(const uint8_t* bytes, size_t length) {
    uint8_t checksum = 0;
    for (size_t index = 0; index < length; index++) {
        checksum = (uint8_t)(checksum + bytes[index]);
    }
    return checksum == 0;
}

static const uint8_t* find_rsdp_in_range(uint64_t start, uint64_t end) {
    start = (start + 15) & ~15ULL;
    if (end > ACPI_IDENTITY_MAP_LIMIT) end = ACPI_IDENTITY_MAP_LIMIT;
    for (uint64_t address = start; address <= end - 20; address += 16) {
        const uint8_t* rsdp = (const uint8_t*)(uintptr_t)address;
        if (!signature_is((const char*)rsdp, "RSD PTR ", 8) ||
            !checksum_is_valid(rsdp, 20)) {
            continue;
        }
        if (rsdp[15] >= 2) {
            uint32_t length = read_u32(rsdp + 20);
            if (length < 36 || length > 4096 ||
                !range_is_mapped(address, length) ||
                !checksum_is_valid(rsdp, length)) {
                continue;
            }
        }
        return rsdp;
    }
    return NULL;
}

static const uint8_t* find_rsdp(void) {
    uint16_t ebda_segment = read_u16((const uint8_t*)(uintptr_t)0x40E);
    uint64_t ebda_address = (uint64_t)ebda_segment << 4;
    if (ebda_address >= 0x400 && ebda_address < 0xA0000) {
        const uint8_t* rsdp = find_rsdp_in_range(ebda_address,
            ebda_address + 1024);
        if (rsdp) return rsdp;
    }
    return find_rsdp_in_range(ACPI_RSDP_SCAN_START, ACPI_RSDP_SCAN_END);
}

static const acpi_sdt_header_t* get_valid_sdt(uint64_t address,
    const char* expected_signature) {
    if (!range_is_mapped(address, sizeof(acpi_sdt_header_t))) return NULL;
    const acpi_sdt_header_t* header =
        (const acpi_sdt_header_t*)(uintptr_t)address;
    if (header->length < sizeof(*header) || header->length > 0x100000 ||
        !range_is_mapped(address, header->length) ||
        !checksum_is_valid((const uint8_t*)header, header->length) ||
        (expected_signature &&
            !signature_is(header->signature, expected_signature, 4))) {
        return NULL;
    }
    return header;
}

static const acpi_sdt_header_t* find_fadt(void) {
    const uint8_t* rsdp = find_rsdp();
    if (!rsdp) return NULL;

    uint64_t root_address = rsdp[15] >= 2 ? read_u64(rsdp + 24) : 0;
    const acpi_sdt_header_t* root = root_address
        ? get_valid_sdt(root_address, "XSDT") : NULL;
    size_t entry_size = sizeof(uint64_t);
    if (!root) {
        root_address = read_u32(rsdp + 16);
        root = get_valid_sdt(root_address, "RSDT");
        entry_size = sizeof(uint32_t);
    }
    if (!root || root->length < sizeof(*root)) return NULL;

    size_t entries_length = root->length - sizeof(*root);
    if (entries_length % entry_size != 0) return NULL;
    const uint8_t* entries = (const uint8_t*)root + sizeof(*root);
    for (size_t offset = 0; offset < entries_length; offset += entry_size) {
        uint64_t address = entry_size == sizeof(uint64_t)
            ? read_u64(entries + offset) : read_u32(entries + offset);
        const acpi_sdt_header_t* table = get_valid_sdt(address, "FACP");
        if (table) return table;
    }
    return NULL;
}

static bool parse_aml_integer(const uint8_t* aml, size_t end,
    size_t* offset, uint64_t* value) {
    if (*offset >= end) return false;
    uint8_t opcode = aml[(*offset)++];
    if (opcode == 0x00 || opcode == 0x01 || opcode == 0xFF) {
        *value = opcode == 0x00 ? 0 : opcode == 0x01 ? 1 : UINT64_MAX;
        return true;
    }

    size_t width = opcode == 0x0A ? 1 :
        opcode == 0x0B ? 2 :
        opcode == 0x0C ? 4 :
        opcode == 0x0E ? 8 : 0;
    if (width == 0 || width > end - *offset) return false;
    *value = width == 1 ? aml[*offset] :
        width == 2 ? read_u16(aml + *offset) :
        width == 4 ? read_u32(aml + *offset) :
        read_u64(aml + *offset);
    *offset += width;
    return true;
}

static bool parse_s5_package(const uint8_t* aml, size_t aml_length,
    size_t package_offset, uint8_t* sleep_type_a, uint8_t* sleep_type_b) {
    if (package_offset >= aml_length || aml[package_offset] != 0x12) {
        return false;
    }

    size_t offset = package_offset + 1;
    if (offset >= aml_length) return false;
    size_t package_length_offset = offset;
    uint8_t first_length = aml[offset++];
    uint8_t follow_bytes = first_length >> 6;
    uint64_t package_length;
    if (follow_bytes == 0) {
        package_length = first_length & 0x3F;
    } else {
        if (follow_bytes > 3 || follow_bytes > aml_length - offset) {
            return false;
        }
        package_length = first_length & 0x0F;
        for (uint8_t index = 0; index < follow_bytes; index++) {
            package_length |= (uint64_t)aml[offset++] << (4 + index * 8);
        }
    }
    if (package_length < offset - package_length_offset ||
        package_length > aml_length - package_length_offset) {
        return false;
    }
    size_t package_end = package_length_offset + (size_t)package_length;
    if (offset >= package_end) return false;
    uint8_t element_count = aml[offset++];
    uint64_t type_a;
    uint64_t type_b;
    if (element_count < 2 ||
        !parse_aml_integer(aml, package_end, &offset, &type_a) ||
        !parse_aml_integer(aml, package_end, &offset, &type_b) ||
        type_a > 7 || type_b > 7) {
        return false;
    }
    *sleep_type_a = (uint8_t)type_a;
    *sleep_type_b = (uint8_t)type_b;
    return true;
}

static bool find_s5_types(const uint8_t* dsdt, size_t dsdt_length,
    uint8_t* sleep_type_a, uint8_t* sleep_type_b) {
    static const uint8_t s5_name[] = {'_', 'S', '5', '_'};
    if (dsdt_length <= sizeof(acpi_sdt_header_t)) return false;
    const uint8_t* aml = dsdt + sizeof(acpi_sdt_header_t);
    size_t aml_length = dsdt_length - sizeof(acpi_sdt_header_t);

    for (size_t offset = 0; offset + sizeof(s5_name) + 1 < aml_length; offset++) {
        if (aml[offset] != 0x08) continue;
        size_t name_offset = offset + 1;
        if (name_offset < aml_length && aml[name_offset] == 0x5C) {
            name_offset++;
        }
        if (name_offset + sizeof(s5_name) >= aml_length ||
            !signature_is((const char*)(aml + name_offset),
                (const char*)s5_name, sizeof(s5_name))) {
            continue;
        }
        size_t object_offset = name_offset + sizeof(s5_name);
        if (aml[object_offset] == 0x12 &&
            parse_s5_package(aml, aml_length, object_offset,
                sleep_type_a, sleep_type_b)) {
            return true;
        }
    }
    return false;
}

static bool get_power_info(acpi_power_info_t* info) {
    const acpi_sdt_header_t* fadt = find_fadt();
    if (!fadt || fadt->length < 90) return false;
    const uint8_t* bytes = (const uint8_t*)fadt;
    uint64_t dsdt_address = read_u32(bytes + 40);
    if (fadt->length >= 148) {
        uint64_t extended_dsdt = read_u64(bytes + 140);
        if (extended_dsdt) dsdt_address = extended_dsdt;
    }

    const acpi_sdt_header_t* dsdt =
        get_valid_sdt(dsdt_address, "DSDT");
    if (!dsdt || !find_s5_types((const uint8_t*)dsdt, dsdt->length,
            &info->sleep_type_a, &info->sleep_type_b)) {
        return false;
    }

    uint32_t pm1a_control = read_u32(bytes + 64);
    uint32_t pm1b_control = read_u32(bytes + 68);
    uint32_t smi_command = read_u32(bytes + 48);
    if (pm1a_control >= UINT16_MAX || pm1b_control > UINT16_MAX ||
        smi_command > UINT16_MAX) {
        return false;
    }
    info->pm1a_control = (uint16_t)pm1a_control;
    info->pm1b_control = (uint16_t)pm1b_control;
    info->control_length = bytes[89];
    info->smi_command = (uint16_t)smi_command;
    info->acpi_enable = bytes[52];
    return info->pm1a_control != 0 && info->control_length >= 2;
}

bool acpi_poweroff(void) {
    acpi_power_info_t info;
    if (!get_power_info(&info)) return false;

    if ((inw(info.pm1a_control) & 1U) == 0) {
        if (info.smi_command == 0 || info.acpi_enable == 0) return false;
        outb(info.smi_command, info.acpi_enable);
        bool enabled = false;
        for (uint32_t attempt = 0; attempt < ACPI_SMI_ENABLE_TIMEOUT; attempt++) {
            if (inw(info.pm1a_control) & 1U) {
                enabled = true;
                break;
            }
            asm volatile ("pause");
        }
        if (!enabled) return false;
    }

    uint64_t irq_flags = save_irq_disable();
    if (info.pm1b_control != 0) {
        uint16_t pm1b = inw(info.pm1b_control);
        pm1b = (uint16_t)((pm1b & ~0x3C00U) |
            ((uint16_t)info.sleep_type_b << 10) | 0x2000U);
        outw(info.pm1b_control, pm1b);
    }

    uint16_t pm1a = inw(info.pm1a_control);
    pm1a = (uint16_t)((pm1a & ~0x3C00U) |
        ((uint16_t)info.sleep_type_a << 10) | 0x2000U);
    outw(info.pm1a_control, pm1a);

    restore_irq(irq_flags);
    return false;
}
