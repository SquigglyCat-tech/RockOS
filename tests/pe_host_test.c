/*
 * Host test for pe.c (Wine layer 1): builds a synthetic PE32 and PE32+
 * in memory and asserts the parser's results.
 *
 * Build: cc -Iinclude -I. -fsanitize=address -o pe_host_test tests/pe_host_test.c src/kernel/pe.c
 */

#include "../include/pe.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t payload[1u << 16];

static void poke32(size_t offset, uint32_t data) {
    payload[offset] = (uint8_t)(data & 0xFF);
    payload[offset + 1] = (uint8_t)((data >> 8) & 0xFF);
    payload[offset + 2] = (uint8_t)((data >> 16) & 0xFF);
    payload[offset + 3] = (uint8_t)((data >> 24) & 0xFF);
}

static void poke64(size_t offset, uint64_t data) {
    poke32(offset, (uint32_t)(data & 0xFFFFFFFFu));
    poke32(offset + 4, (uint32_t)(data >> 32));
}

static void poke_bytes(size_t offset, const char* text, size_t length) {
    memcpy(payload + offset, text, length);
}

static void poke16(size_t offset, uint16_t data) {
    payload[offset] = (uint8_t)(data & 0xFF);
    payload[offset + 1] = (uint8_t)((data >> 8) & 0xFF);
}

static int check(pe_image_t* image, bool expect_ok) {
    if (expect_ok != image->valid) {
        printf("FAIL: parsed ok=%d expected ok=%d: %s\n", image->valid,
            expect_ok, image->error ? image->error : "none");
        return 0;
    }
    return 1;
}

static int test_valid_32(void) {
    memset(payload, 0, sizeof(payload));
    poke_bytes(0, "MZ", 2);                     /* DOS signature      */
    poke32(0x3C, 0x40);                         /* COFF offset        */
    poke_bytes(0x40, "PE\x00\x00", 4);          /* PE signature       */
    poke16(0x44, PE_MACHINE_I386);              /* machine            */
    poke16(0x46, 1);                            /* section count      */
    poke16(0x4a, 0);                            /* timestamp          */
    poke32(0x4c, 0);                            /* symbol table ptr   */
    poke32(0x50, 0);                            /* symbol count       */
    poke16(0x54, 224);                          /* optional hdr size  */
    poke16(0x56, 0x0102);                       /* characteristics    */
    size_t optional = 0x58;                     /* COFF header end    */
    poke16(optional, PE_OPTIONAL_MAGIC_32);     /* 0x10B              */
    poke32(optional + 16, 0x1000);              /* AddressOfEntryPoint*/
    poke32(optional + 28, 0x00400000);          /* ImageBase          */
    poke32(optional + 32, 0x1000);              /* SectionAlignment   */
    poke32(optional + 36, 0x0200);              /* FileAlignment      */
    poke32(optional + 56, 0x2000);              /* SizeOfImage        */
    poke32(optional + 60, 0x0200);              /* SizeOfHeaders      */
    size_t sections = optional + 224;
    poke_bytes(sections, ".text", 6);           /* section name       */
    poke32(sections + 8, 0x1000);               /* VirtualSize        */
    poke32(sections + 12, 0x1000);              /* VirtualAddress     */
    poke32(sections + 16, 0x0200);              /* SizeOfRawData      */
    poke32(sections + 20, 0x0200);              /* PointerToRawData   */
    poke32(sections + 36, 0x20000000);          /* characteristics    */

    pe_image_t image;
    memset(&image, 0, sizeof(image));
    bool ok = pe_parse(payload, 0x5000, &image);
    if (!check(&image, true) || !ok) return 0;
    if (image.machine != PE_MACHINE_I386) return 0;
    if (image.is_plus) return 0;
    if (image.entry_point_rva != 0x1000) return 0;
    if (image.section_count != 1) return 0;
    if (strcmp(image.sections[0].name, ".text") != 0) return 0;
    if (image.sections[0].virtual_address != 0x1000) return 0;
    if (image.sections[0].file_offset != 0x0200) return 0;
    if (image.sections[0].raw_size != 0x0200) return 0;
    if (image.sections[0].executable == false) return 0;
    if (memcmp(pe_machine_name(image.machine), "i386", 5) != 0) return 0;
    printf("PASS: PE32 layout\n");
    return 1;
}

static int test_valid_64(void) {
    memset(payload, 0, sizeof(payload));
    poke_bytes(0, "MZ", 2);
    poke32(0x3C, 0x40);
    poke_bytes(0x40, "PE\x00\x00", 4);
    poke16(0x44, PE_MACHINE_AMD64);
    poke16(0x46, 2);
    poke32(0x50, 0);
    poke16(0x54, 240);
    poke16(0x56, 0x0022);
    size_t optional = 0x58;
    poke16(optional, PE_OPTIONAL_MAGIC_64);
    poke32(optional + 16, 0x2000);              /* AddressOfEntryPoint */
    poke64(optional + 24, 0x140000000ull);      /* ImageBase           */
    poke32(optional + 32, 0x1000);
    poke32(optional + 36, 0x0200);
    poke32(optional + 56, 0x3000);
    poke32(optional + 60, 0x0200);
    size_t sections = optional + 240;
    poke_bytes(sections, ".text", 6);
    poke32(sections + 8, 0x1000);
    poke32(sections + 12, 0x1000);
    poke32(sections + 16, 0x0200);
    poke32(sections + 20, 0x0200);
    poke32(sections + 36, 0x20000000);
    poke_bytes(sections + 40, ".data", 6);
    poke32(sections + 48, 0x400);
    poke32(sections + 52, 0x2000);
    poke32(sections + 56, 0x400);
    poke32(sections + 60, 0x0400);
    poke32(sections + 76, 0xC0000040u);         /* data: rwx characteristics */

    pe_image_t image;
    memset(&image, 0, sizeof(image));
    bool ok = pe_parse(payload, 0x5000, &image);
    if (!check(&image, true) || !ok) return 0;
    if (image.machine != PE_MACHINE_AMD64) return 0;
    if (!image.is_plus) return 0;
    if (image.entry_point_rva != 0x2000) return 0;
    if (image.image_base != 0x140000000ull) return 0;
    if (image.section_count != 2) return 0;
    if (strcmp(image.sections[1].name, ".data") != 0) return 0;
    if (!image.sections[1].writable || image.sections[1].executable) {
        return 0;
    }
    printf("PASS: PE32+ layout\n");
    return 1;
}

static int test_invalid(void) {
    pe_image_t image;
    memset(&image, 0, sizeof(image));
    if (pe_parse(payload, 0, &image)) return 0;  /* blob too small */
    memset(payload, 0, sizeof(payload));
    poke_bytes(0, "MZ", 2);
    poke32(0x3C, 0x40);
    poke_bytes(0x40, "NOPE", 4);                 /* not "PE" */
    if (pe_parse(payload, sizeof(payload), &image)) return 0;
    if (!image.error) return 0;
    poke_bytes(0x40, "PE\x00\x00", 4);
    poke16(0x44, 0xFFFF);                        /* machine FreeBSD-99? */
    if (pe_parse(payload, sizeof(payload), &image)) return 0;
    poke16(0x44, PE_MACHINE_I386);
    poke16(0x46, PE_SECTION_MAX + 1);            /* too many sections */
    if (pe_parse(payload, sizeof(payload), &image)) return 0;
    poke16(0x46, 1);
    poke16(0x54, 0);                             /* missing optional hdr */
    if (pe_parse(payload, sizeof(payload), &image)) return 0;
    /* a valid image with a section pointing out of blobs bounds */
    poke16(0x54, 224);
    size_t optional = 0x58;
    poke16(optional, PE_OPTIONAL_MAGIC_32);
    poke32(optional + 28, 0x00400000);
    poke32(optional + 32, 0x1000);
    poke32(optional + 36, 0x0200);
    poke32(optional + 56, 0x2000);
    poke32(optional + 60, 0x0200);
    size_t sections = optional + 224;
    poke_bytes(sections, ".text", 6);
    poke32(sections + 8, 0x1000);
    poke32(sections + 12, 0x1000);
    poke32(sections + 16, 0x0200);
    poke32(sections + 20, 65536);                /* past end of payload */
    if (pe_parse(payload, sizeof(payload), &image)) return 0;
    printf("PASS: invalid-input rejection\n");
    return 1;
}

int main(void) {
    if (!test_valid_32()) return 1;
    if (!test_valid_64()) return 1;
    if (!test_invalid()) return 1;
    printf("pe_host_test: all 3 cases PASS\n");
    return 0;
}
