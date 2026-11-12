/*
 * Host test for Wine layer 1c: the relocatable PE32 fixture parses,
 * satisfies the new envelope (custom ImageBase allowed), snapshots
 * correctly, and base-relocates through pe_relocate().
 *
 * Build:
 *   cc -Iinclude -o pe_run_host_test tests/pe_run_host_test.c \
 *      src/kernel/pe.c src/kernel/pe_run.c tests/pe_run_host_stubs.c \
 *      src/kernel/rok.c -Wall -Wextra
 */

#include "../include/pe.h"
#include "../include/pe_run.h"
#include "../src/kernel/pe_sample32.h"
#include "../src/kernel/pe_reloc32.h"
#include "../src/kernel/pe_import32.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
    else { printf("PASS: %s\n", (msg)); } \
} while (0)

int main(void) {
    const uint8_t* blob = (const uint8_t*)pe_sample32_bytes;
    size_t size = PE_SAMPLE32_NUM_WORDS * 2;

    pe_image_t image;
    if (!pe_parse(blob, size, &image)) {
        printf("FAIL: sample PE32 parse: %s\n",
            image.error ? image.error : "unknown");
        return 1;
    }
    CHECK(image.machine == PE_MACHINE_I386 && !image.is_plus,
        "sample machine flags");
    CHECK(image.entry_point_rva == PE_SAMPLE32_ENTRY_RVA,
        "sample entry RVA");
    CHECK(image.rva_and_sizes_count <= PE_DATA_DIRECTORY_MAX,
        "sample directory count bounded");
    if (!pe_run_can_load(&image)) {
        printf("FAIL: sample outside the layer-1c envelope\n");
        return 1;
    }
    printf("PASS: sample satisfies the layer-1c envelope\n");

    /* --- envelope rejections --- */
    pe_image_t probe = image;
    probe.machine = PE_MACHINE_AMD64;
    probe.is_plus = true;
    CHECK(!pe_run_can_load(&probe), "wrong machine refused");

    probe = image;
    probe.size_of_image = 256 * 1024 + 1;
    CHECK(!pe_run_can_load(&probe), "oversized image refused");

    probe = image;
    probe.section_count = RING3_PE_MAX_SECTIONS + 1;
    CHECK(!pe_run_can_load(&probe), "too many sections refused");

    probe = image;
    probe.section_count = 0;
    CHECK(!pe_run_can_load(&probe), "section-less image refused");

    probe = image;
    probe.rva_and_sizes_count = 2;
    probe.data_directories[PE_DIR_IMPORT_TABLE].size = 64;
    CHECK(!pe_run_can_load(&probe), "import-bearing image refused");

    /* --- reloc fixture --- */
    const uint8_t* rblob = (const uint8_t*)pe_reloc32_bytes;
    size_t rsize = PE_RELOC32_NUM_WORDS * 2;

    pe_image_t rimage;
    if (!pe_parse(rblob, rsize, &rimage)) {
        printf("FAIL: reloc PE32 parse: %s\n",
            rimage.error ? rimage.error : "unknown");
        failures++;
        return 1;
    }
    CHECK(rimage.image_base == PE_RELOC32_IMAGE_BASE,
        "reloc fixture ImageBase");
    CHECK(rimage.section_count == 1, "reloc fixture section count");
    CHECK(rimage.rva_and_sizes_count == 16,
        "reloc fixture directory count");
    CHECK(rimage.data_directories[PE_DIR_RELOCATION_TABLE].size == 0x18 &&
        rimage.data_directories[PE_DIR_RELOCATION_TABLE].rva == 0x1080,
        "reloc directory location");
    CHECK(rimage.rva_and_sizes_count <= PE_DIR_IMPORT_TABLE ||
        rimage.data_directories[PE_DIR_IMPORT_TABLE].size == 0,
        "reloc fixture has no imports");
    if (!pe_run_can_load(&rimage)) {
        printf("FAIL: relocated image outside the envelope\n");
        failures++;
        return 1;
    }
    printf("PASS: reloc fixture satisfies the layer-1c envelope\n");

    /* --- snapshot --- */
    uint8_t snapshot[PE_RELOC32_SIZE_OF_IMAGE];
    memset(snapshot, 0xCC, sizeof(snapshot));
    CHECK(pe_image_snapshot(&rimage, rblob, rsize, snapshot,
        sizeof(snapshot)), "snapshot builds");
    CHECK(snapshot[0] == 'M' && snapshot[1] == 'Z',
        "snapshot starts with headers");
    CHECK(snapshot[0x10F0] == 0x10 && snapshot[0x10F1] == 0x01 &&
        snapshot[0x10F2] == 0x40 && snapshot[0x10F3] == 0x00,
        "section bytes copied at their RVA");
    CHECK(snapshot[0x1400] == 0x00, "zero-filled unmapped RVA range");

    /* --- pe_rva_sourced --- */
    uint32_t file_offset = 0;
    CHECK(pe_rva_sourced(&rimage, 0x10F0, 4, &file_offset) &&
        file_offset == 0x10F0,
        "rva lookup maps into a file-backed section");
    CHECK(pe_rva_sourced(&rimage, 0x0104, 4, &file_offset) &&
        file_offset == 0x0104,
        "rva lookup maps into headers");

    /* --- relocation --- */
    uint64_t delta = RING3_PE_BASE - (uint64_t)PE_RELOC32_IMAGE_BASE;
    CHECK(pe_relocate(&rimage, snapshot, sizeof(snapshot),
        rblob, rsize, delta), "relocation applied");
    {
        uint32_t value = (uint32_t)snapshot[0x10F0] |
            ((uint32_t)snapshot[0x10F1] << 8) |
            ((uint32_t)snapshot[0x10F2] << 16) |
            ((uint32_t)snapshot[0x10F3] << 24);
        CHECK(value == 0x60000110u,
            "HIGHLOW relocation fixed the absolute pointer");
    }
    /* re-running with delta 0 is idempotent on the fixed snapshot */
    CHECK(pe_relocate(&rimage, snapshot, sizeof(snapshot),
        rblob, rsize, 0), "delta-0 relocation succeeds");
    {
        uint32_t value = (uint32_t)snapshot[0x10F0] |
            ((uint32_t)snapshot[0x10F1] << 8) |
            ((uint32_t)snapshot[0x10F2] << 16) |
            ((uint32_t)snapshot[0x10F3] << 24);
        CHECK(value == 0x60000110u, "delta 0 left the pointer intact");
    }
    /* a snapshot smaller than the fixup target must be refused */
    CHECK(!pe_relocate(&rimage, snapshot, 0x10F0,
        rblob, rsize, delta), "bounds-checked relocation");

    /* --- layer 1d: import-bearing fixture --- */
    const uint8_t* iblob = (const uint8_t*)pe_import32_bytes;
    size_t isize = PE_IMPORT32_NUM_WORDS * 2;
    pe_image_t iimage;
    CHECK(pe_parse(iblob, isize, &iimage),
        iimage.error ? iimage.error : "import fixture parses");
    CHECK(iimage.import_count == 2, "two import functions parsed");
    CHECK(iimage.import_rva == PE_IMPORT32_IAT_RVA, "IAT RVA parsed");
    CHECK(strcmp(iimage.import_dll, "kernel32.dll") == 0,
        "kernel32.dll recognized (case-folded)");
    CHECK(strcmp(iimage.import_names[0], "WriteFile") == 0 &&
        strcmp(iimage.import_names[1], "ExitProcess") == 0,
        "import names in arrival order");
    CHECK(pe_run_can_load(&iimage),
        "kernel32 import fixture satisfies the 1d envelope");

    uint8_t isnapshot[PE_IMPORT32_SIZE_OF_IMAGE + 0x1000];
    memset(isnapshot, 0xCC, sizeof(isnapshot));
    CHECK(pe_image_snapshot(&iimage, iblob, isize, isnapshot,
        PE_IMPORT32_SIZE_OF_IMAGE), "import fixture snapshot builds");
    CHECK(pe_run_prepare_imports(&iimage, isnapshot,
        sizeof(isnapshot)), "import table prepared");

    /* stub area starts at SizeOfImage, 16-byte aligned */
    static const uint8_t writefile_stub[] = {
        0x8B, 0x74, 0x24, 0x04, 0x8B, 0x54, 0x24, 0x08,
        0x8B, 0x4C, 0x24, 0x0C, 0x8B, 0x7C, 0x24, 0x10,
        0xBB, 0x02, 0x00, 0x00, 0x00,
        0xB8, 0x07, 0x00, 0x00, 0x00,
        0xCD, 0x80, 0xC2, 0x10, 0x00,
    };
    static const uint8_t exitprocess_stub[] = {
        0x8B, 0x74, 0x24, 0x04,
        0xBB, 0x00, 0x00, 0x00, 0x00,
        0xB8, 0x07, 0x00, 0x00, 0x00,
        0xCD, 0x80, 0xC2, 0x04, 0x00,
    };
    CHECK(memcmp(isnapshot + PE_IMPORT32_SIZE_OF_IMAGE, writefile_stub,
        sizeof(writefile_stub)) == 0, "WriteFile stub bytes");
    CHECK(memcmp(isnapshot + PE_IMPORT32_SIZE_OF_IMAGE + 0x20,
        exitprocess_stub, sizeof(exitprocess_stub)) == 0,
        "ExitProcess stub bytes (16-byte aligned slot)");
    {
        uint32_t iat0 = (uint32_t)isnapshot[0x1050] |
            ((uint32_t)isnapshot[0x1051] << 8) |
            ((uint32_t)isnapshot[0x1052] << 16) |
            ((uint32_t)isnapshot[0x1053] << 24);
        uint32_t iat1 = (uint32_t)isnapshot[0x1054] |
            ((uint32_t)isnapshot[0x1055] << 8) |
            ((uint32_t)isnapshot[0x1056] << 16) |
            ((uint32_t)isnapshot[0x1057] << 24);
        CHECK(iat0 == PE_IMPORT32_SIZE_OF_IMAGE,
            "IAT slot 0 patched to the WriteFile stub");
        CHECK(iat1 == PE_IMPORT32_SIZE_OF_IMAGE + 0x20,
            "IAT slot 1 patched to the ExitProcess stub");
    }

    /* envelope rejections for imports */
    pe_image_t iprobe = iimage;
    strcpy(iprobe.import_dll, "user32.dll");
    CHECK(!pe_run_can_load(&iprobe), "unknown DLL refused");
    iprobe = iimage;
    strcpy(iprobe.import_names[0], "CreateFileA");
    CHECK(!pe_run_can_load(&iprobe), "unknown function refused");

    /* unknown name through the prepare path too */
    iprobe = iimage;
    strcpy(iprobe.import_names[1], "BogusFn");
    CHECK(!pe_run_prepare_imports(&iprobe, isnapshot,
        sizeof(isnapshot)), "unknown name refused at prepare");
    strcpy(iprobe.import_names[1], "ExitProcess");
    iprobe.import_count = PE_IMPORT_MAX + 1;
    CHECK(!pe_run_prepare_imports(&iprobe, isnapshot,
        sizeof(isnapshot)), "oversized import list refused");

    if (failures) {
        printf("pe_run_host_test: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("pe_run_host_test: all cases PASS\n");
    return 0;
}
