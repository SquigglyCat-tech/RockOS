#ifndef ROCKOS_PE_H
#define ROCKOS_PE_H

/*
 * pe.h - Windows PE32/PE32+ image loader (Wine layer 1).
 *
 * Parses DOS header -> COFF -> Optional header, validates machine and
 * section layout, maps sections into a freshly prepared address space
 * (via paging_map_user_page, WINE layer 0's user mapping), and reports
 * the entry point. Import resolution is stubbed at this layer:
 * `pe_import_stub(name)` receives the RVA of the hinted import so a
 * later Win32-API layer (kernel32: VirtualAlloc, ReadFile, WriteFile,
 * etc.) can take over. Single-import, single-section lazy import
 * resolution is deferred to that layer.
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
    /* Entry point for ring-3 dispatch; zero if the file is an EXE-only
     * stub (e.g. resource-only images). */
    uint64_t entry_point_rva;
    /* Diagnostics (NULL when ok, short message otherwise) */
    const char* error;
} pe_image_t;

/* Parse the in-memory PE image into `image`. Copies header/section
 * metadata; does NOT touch memory lock or paging state. False on any
 * malformed input, with `error` set. */
bool pe_parse(const void* blob, size_t blob_size, pe_image_t* image);

/* Section-relative name for logs. */
const char* pe_machine_name(uint16_t machine);

#endif /* ROCKOS_PE_H */
