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

static bool pe_reader_peek(const uint8_t* blob, size_t blob_size,
    size_t offset, size_t count, uint8_t* out) {
    if (!blob || !out) return false;
    if (offset > blob_size || count > blob_size - offset) return false;
    for (size_t index = 0; index < count; index++) {
        out[index] = blob[offset + index];
    }
    return true;
}

static void pe_write_u32(uint8_t* at, uint32_t value) {
    at[0] = (uint8_t)(value & 0xFF);
    at[1] = (uint8_t)((value >> 8) & 0xFF);
    at[2] = (uint8_t)((value >> 16) & 0xFF);
    at[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void pe_write_u64(uint8_t* at, uint64_t value) {
    pe_write_u32(at, (uint32_t)(value & 0xFFFFFFFFu));
    pe_write_u32(at + 4, (uint32_t)(value >> 32));
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
    /* data directories (bounded by NumberOfRvaAndSizes) */
    image->rva_and_sizes_count = pe_u32(optional + (is_plus ?
        PE_OFF_NUM_RVA_SIZES_64 : PE_OFF_NUM_RVA_SIZES_32));
    if (image->rva_and_sizes_count > PE_DATA_DIRECTORY_MAX) {
        image->rva_and_sizes_count = PE_DATA_DIRECTORY_MAX;
    }
    {
        size_t directory_base = optional_offset + (is_plus ?
            PE_OFF_DATA_DIRS_64 : PE_OFF_DATA_DIRS_32);
        for (uint32_t index = 0; index < image->rva_and_sizes_count;
            index++) {
            const uint8_t* entry = 0;
            image->data_directories[index].rva = 0;
            image->data_directories[index].size = 0;
            if (!pe_reader_read(&reader, directory_base + index * 8, 8,
                &entry)) {
                image->error = "truncated data directories";
                return false;
            }
            image->data_directories[index].rva = pe_u32(entry);
            image->data_directories[index].size = pe_u32(entry + 4);
        }
    }
    image->valid = true;
    image->error = 0;
    return true;
}

bool pe_rva_sourced(const pe_image_t* image, uint32_t rva,
    uint32_t count, uint32_t* file_offset) {
    if (!image || !image->valid) return false;
    if (!file_offset) return false;
    /* headers region serves RVAs strictly below SizeOfHeaders */
    if (rva < image->size_of_headers) {
        if (count > image->size_of_headers - rva) return false;
        *file_offset = rva;
        return true;
    }
    for (uint8_t index = 0; index < image->section_count; index++) {
        uint32_t section_base = image->sections[index].virtual_address;
        uint32_t section_size = image->sections[index].raw_size;
        if (section_size == 0) continue;
        if (rva < section_base) continue;
        uint32_t inside = rva - section_base;
        if (inside >= section_size) continue;
        if (count > section_size - inside) return false;
        *file_offset = image->sections[index].file_offset + inside;
        return true;
    }
    return false;
}

bool pe_image_snapshot(const pe_image_t* image, const uint8_t* blob,
    size_t blob_size, uint8_t* out, size_t out_size) {
    if (!image || !image->valid || !blob || !out) return false;
    if (out_size < image->size_of_image) return false;
    for (size_t index = 0; index < out_size; index++) out[index] = 0;
    if (image->size_of_headers > out_size) return false;
    if (!pe_reader_peek(blob, blob_size, 0, image->size_of_headers, out)) {
        return false;
    }
    for (uint8_t index = 0; index < image->section_count; index++) {
        uint32_t raw_size = image->sections[index].raw_size;
        if (raw_size == 0) continue;
        if (image->sections[index].virtual_address + raw_size >
            image->size_of_image) {
            /* leave the truncated tail zero-filled */
            continue;
        }
        if (!pe_reader_peek(blob, blob_size,
            image->sections[index].file_offset, raw_size,
            out + image->sections[index].virtual_address)) {
            return false;
        }
    }
    return true;
}

bool pe_relocate(const pe_image_t* image, uint8_t* snapshot,
    size_t snapshot_size, const uint8_t* blob, size_t blob_size,
    uint64_t delta) {
    if (!image || !image->valid || !snapshot || !blob) return false;
    if (image->rva_and_sizes_count <= PE_DIR_RELOCATION_TABLE) {
        /* no relocation table: only valid when the load base matches */
        return delta == 0;
    }
    if (image->data_directories[PE_DIR_RELOCATION_TABLE].size == 0) {
        return delta == 0;
    }
    uint32_t reloc_rva =
        image->data_directories[PE_DIR_RELOCATION_TABLE].rva;
    uint32_t reloc_size =
        image->data_directories[PE_DIR_RELOCATION_TABLE].size;
    uint32_t reloc_file_offset = 0;
    if (!pe_rva_sourced(image, reloc_rva, reloc_size, &reloc_file_offset)) {
        return false;
    }
    if (reloc_file_offset + reloc_size > blob_size) {
        return false;
    }
    /* relocation data is read straight from the blob */
    size_t cursor = 0;
    if (reloc_size < 8) return false;
    while (cursor + 8 <= reloc_size) {
        const uint8_t* block = blob + reloc_file_offset + cursor;
        uint32_t page_rva = pe_u32(block);
        uint32_t block_size = pe_u32(block + 4);
        if (block_size < 8) return false;
        size_t entry_bytes = block_size - 8;
        for (size_t entry = 0; entry + 2 <= entry_bytes; entry += 2) {
            uint16_t entry_value = pe_u16(block + 8 + entry);
            uint16_t type = (uint16_t)(entry_value >> 12);
            uint16_t offset = (uint16_t)(entry_value & 0x0FFF);
            uint32_t target_rva = page_rva + offset;
            if (type == PE_REL_ABSOLUTE) continue;
            if (target_rva >= snapshot_size) return false;
            if (image->is_plus) {
                if (type == PE_REL_DIR64) {
                    uint64_t value = pe_u64(snapshot + target_rva);
                    pe_write_u64(snapshot + target_rva, value + delta);
                    continue;
                }
            } else {
                if (type == PE_REL_HIGHLOW) {
                    uint32_t value = pe_u32(snapshot + target_rva);
                    if (target_rva > snapshot_size - 4) return false;
                    pe_write_u32(snapshot + target_rva, value +
                        (uint32_t)delta);
                    continue;
                }
            }
            return false; /* unsupported relocation type */
        }
        cursor += block_size;
        if (cursor % 4 != 0) {
            /* blocks are 32-bit aligned inside the directory */
            cursor += 4 - (cursor % 4);
        }
    }
    return cursor == reloc_size;
}
