#ifndef ROCKOS_PE_H
#define ROCKOS_PE_H

/*
 * pe.h - Windows PE32/PE32+ image loader (Wine layer 1).
 *
 * Parses DOS header -> COFF -> Optional header, validates machine and
 * section layout, maps sections into a freshly prepared address space
 * (via paging_map_user_page, Wine layer 0's user mapping), and reports
 * the entry point. Import resolution is stubbed at this layer:
 * layer 1c handles base relocations and the Win32 API surface (kernel32).
 *
 * Freestanding, no libc. Works for images already resident in kernel
 * memory (read from RockFS, or from the install image).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PE_MAGIC_DOS "MZ"           /* 0x4D 0x5A */
#define PE_OFFSET_COFF_SIGNATURE 0x3C
#define PE_MACHINE_I386 0x014C        /* PE32 */
#define PE_MACHINE_AMD64 0x8664       /* PE32+ */
#define PE_SIGNATURE "PE"
#define PE_OPTIONAL_MAGIC_32 0x10B    /* PE32  optional header */
#define PE_OPTIONAL_MAGIC_64 0x20B    /* PE32+ optional header */
#define PE_SECTION_MAX 32
#define PE_STRING_MAX 9

#define PE_DATA_DIRECTORY_MAX 16
#define PE_DIR_IMPORT_TABLE 1
#define PE_DIR_RELOCATION_TABLE 5

/* Data directory layout offsets (bytes from the optional header start):
 * PE32: NumberOfRvaAndSizes at 92, directories from 96.
 * PE32+: NumberOfRvaAndSizes at 108, directories from 112. */
#define PE_OFF_NUM_RVA_SIZES_32 92U
#define PE_OFF_NUM_RVA_SIZES_64 108U
#define PE_OFF_DATA_DIRS_32 96U
#define PE_OFF_DATA_DIRS_64 112U

/* Relocation entry types handled by pe_relocate(). */
#define PE_REL_ABSOLUTE 0
#define PE_REL_HIGHLOW 3
#define PE_REL_DIR64 10

typedef struct {
    /* Identifies what was found */
    bool valid;
    bool is_plus;                 /* PE32+ (AMD64) or PE32 (i386) */
    uint16_t machine;
    /* Layout parameters used for mapping */
    uint64_t image_base;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t section_alignment;
    uint32_t file_alignment;
    /* Section list prepared for mapping */
    uint8_t section_count;
    struct {
        char name[PE_STRING_MAX];
        uint32_t virtual_address;
        uint32_t virtual_size;
        uint32_t file_offset;
        uint32_t raw_size;
        bool executable;
        bool writable;
    } sections[PE_SECTION_MAX];
    /* Entry point for ring-3 dispatch; zero for EXE-only stubs. */
    uint64_t entry_point_rva;
    /* Optional-header data directories (fewer than 16 stored). */
    uint32_t rva_and_sizes_count;
    struct {
        uint32_t rva;
        uint32_t size;
    } data_directories[PE_DATA_DIRECTORY_MAX];
    /* Diagnostics (NULL when ok, short message otherwise) */
    const char* error;
} pe_image_t;

/* Parse the in-memory PE image into `image`. Copies header/section
 * metadata; does NOT touch memory or paging state. False on any
 * malformed input, with `error` set. */
bool pe_parse(const void* blob, size_t blob_size, pe_image_t* image);

/* Short printable machine identity (i386 / amd64). */
const char* pe_machine_name(uint16_t machine);

/* True when rva..rva+count maps inside headers or a file-backed
 * section of the blob; on success *file_offset receives the file
 * offset to read from. */
bool pe_rva_sourced(const pe_image_t* image, uint32_t rva, uint32_t count,
    uint32_t* file_offset);

/* Copy the whole image (headers + raw sections at their RVAs) into
 * out; zero-fills anything unmapped. Returns false on bounds errors. */
bool pe_image_snapshot(const pe_image_t* image, const uint8_t* blob,
    size_t blob_size, uint8_t* out, size_t out_size);

/* Apply base relocations from the relocation data directory into the
 * snapshot buffer. delta = load_base - image_base. Handles types
 * 0 (padding), 3 (PE32 HIGHLOW), 10 (PE32+ DIR64). */
bool pe_relocate(const pe_image_t* image, uint8_t* snapshot,
    size_t snapshot_size, const uint8_t* blob, size_t blob_size,
    uint64_t delta);

#endif /* ROCKOS_PE_H */
