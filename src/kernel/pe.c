/*
 * pe.c - Windows PE32/PE32+ image loader for the Wine layers (see pe.h).
 */

#include "pe.h"

#include <stddef.h>
#include <stdint.h>

typedef struct { const uint8_t* bytes; size_t size; } pe_reader_t;

static bool pe_reader_read(const pe_reader_t* reader, size_t offset,
    size_t count, const uint8_t** out) {
    if (offset > reader->size) return false;
    if (count > reader->size - offset) return false;
    *out = reader->bytes + offset;
    return true;
}

static uint16_t pe_u16(const uint8_t* at) {
    return (uint16_t)(at[0] | (uint16_t)at[1] << 8);
}

static uint32_t pe_u32(const uint8_t* at) {
    return (uint32_t)(at[0] | (uint32_t)at[1] << 8 |
        (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24);
}

static uint64_t pe_u64(const uint8_t* at) {
    uint64_t low = (uint64_t)pe_u32(at);
    return low | (uint64_t)pe_u32(at + 4) << 32;
}

const char* pe_machine_name(uint16_t machine) {
    switch (machine) {
        case PE_MACHINE_I386:  return "i386";
        case PE_MACHINE_AMD64: return "amd64";
        default: return "?";
    }
}

bool pe_parse(const void* blob, size_t blob_size, pe_image_t* image) {
    if (!blob || !image || blob_size < PE_OFFSET_COFF_SIGNATURE + 4) {
        if (image) image->error = "blob too small";
        return false;
    }
    image->valid = false;
    image->is_plus = false;
    image->entry_point_rva = 0;
    image->error = 0;
    image->section_count = 0;

    pe_reader_t reader = { blob, blob_size };
    const uint8_t* dos = 0;
    if (!pe_reader_read(&reader, 0, 2, &dos) ||
        dos[0] != (uint8_t)'M' || dos[1] != (uint8_t)'Z') {
        image->error = "missing MZ";
        return false;
    }
    const uint8_t* offset_field = 0;
    if (!pe_reader_read(&reader, PE_OFFSET_COFF_SIGNATURE, 4,
        &offset_field)) {
        image->error = "missing COFF offset";
        return false;
    }
    size_t coff_offset = (size_t)pe_u32(offset_field);
    const uint8_t* coff_signature = 0;
    if (!pe_reader_read(&reader, coff_offset, 4, &coff_signature) ||
        coff_signature[0] != (uint8_t)'P' || coff_signature[1] !=
        (uint8_t)'E' || coff_signature[2] || coff_signature[3]) {
        image->error = "missing PE";
        return false;
    }
    size_t coff_header = coff_offset + 4;
    /* COFF header is 20 bytes: machine, numberOfSections, stamp, symbol
     * table pointer, symbol count, optional header size, characteristics */
    const uint8_t* coff = 0;
    if (!pe_reader_read(&reader, coff_header, 20, &coff)) {
        image->error = "truncated COFF";
        return false;
    }
    uint16_t machine = pe_u16(coff);
    if (machine != PE_MACHINE_I386 && machine != PE_MACHINE_AMD64) {
        image->error = "unsupported machine";
        return false;
    }
    uint16_t number_of_sections = pe_u16(coff + 2);
    if (number_of_sections > PE_SECTION_MAX) {
        image->error = "too many sections";
        return false;
    }
    uint16_t optional_header_size = pe_u16(coff + 16);
    if (optional_header_size == 0) {
        image->error = "no optional header";
        return false;
    }
    size_t optional_offset = coff_header + 20;
    const uint8_t* optional = 0;
    if (!pe_reader_read(&reader, optional_offset, optional_header_size,
        &optional)) {
        image->error = "truncated optional header";
        return false;
    }
    bool is_plus = optional[0] == (PE_OPTIONAL_MAGIC_64 & 0xFF) &&
        optional[1] == (PE_OPTIONAL_MAGIC_64 >> 8);
    if (!is_plus && (optional[0] != (PE_OPTIONAL_MAGIC_32 & 0xFF) ||
        optional[1] != (PE_OPTIONAL_MAGIC_32 >> 8))) {
        image->error = "unsupported optional magic";
        return false;
    }
    /* Magic 0x10B = PE32 layout: 96-byte optional header; 0x20B = PE32+.
     * Image base size and threshold fields change with the magic. */
    uint64_t image_base = is_plus
        ? pe_u64(optional + 24) : (uint64_t)pe_u32(optional + 28);
    uint32_t section_alignment = pe_u32(optional + (is_plus ? 32 : 32));
    uint32_t file_alignment = pe_u32(optional + (is_plus ? 36 : 36));
    uint32_t size_of_image = pe_u32(optional + (is_plus ? 56 : 56));
    uint32_t size_of_headers = pe_u32(optional + (is_plus ? 60 : 60));
    uint32_t entry_point = pe_u32(optional + (is_plus ? 16 : 16));
    if (section_alignment == 0) {
        image->error = "zero section alignment";
        return false;
    }
    uint32_t min_alignment = is_plus ? 1u : 0u;
    (void)min_alignment;
    /* Section table sits right after the optional header. */
    size_t section_offset = optional_offset + optional_header_size;
    const uint8_t* section_table = 0;
    if (!pe_reader_read(&reader, section_offset,
        (size_t)number_of_sections * 40, &section_table)) {
        image->error = "truncated section table";
        return false;
    }
    image->machine = machine;
    image->is_plus = is_plus;
    image->image_base = image_base;
    image->size_of_image = size_of_image;
    image->size_of_headers = size_of_headers;
    image->section_alignment = section_alignment;
    image->file_alignment = file_alignment;
    image->entry_point_rva = entry_point;
    image->section_count = (uint8_t)number_of_sections;
    for (uint16_t index = 0; index < number_of_sections; index++) {
        const uint8_t* section = section_table + (size_t)index * 40;
        for (size_t pos = 0; pos < 8; pos++) {
            image->sections[index].name[pos] = (char)section[pos];
        }
        image->sections[index].name[8] = '\0';
        image->sections[index].virtual_size = pe_u32(section + 8);
        image->sections[index].virtual_address = pe_u32(section + 12);
        image->sections[index].raw_size = pe_u32(section + 16);
        image->sections[index].file_offset = pe_u32(section + 20);
        uint32_t characteristics = pe_u32(section + 36);
        image->sections[index].executable = (characteristics & 0x20000000)
            != 0;
        image->sections[index].writable = (characteristics & 0x80000000)
            != 0;
        if (image->sections[index].raw_size > blob_size ||
            image->sections[index].file_offset >
            blob_size - image->sections[index].raw_size) {
            image->error = "section bytes out of range";
            image->sections[index].raw_size = 0;
            return false;
        }
    }
    image->valid = true;
    image->error = 0;
    return true;
}
