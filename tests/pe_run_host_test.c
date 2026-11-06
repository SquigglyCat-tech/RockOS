/*
 * Host test for Wine layer 1b: the built-in PE32 sample parses and
 * satisfies the pe_run_can_load envelope; malformed envelopes are
 * refused. (pe_run.c's stage/launch path needs the kernel, so the host
 * test covers parsing + envelope logic only.)
 *
 * Build:
 *   cc -Iinclude -o pe_run_host_test tests/pe_run_host_test.c \
 *      src/kernel/pe.c src/kernel/pe_run.c tests/pe_run_host_stubs.c \
 *      src/kernel/rok.c -Wall -Wextra
 */

#include "../include/pe.h"
#include "../include/pe_run.h"
#include "../src/kernel/pe_sample32.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    const uint8_t* blob = (const uint8_t*)pe_sample32_bytes;
    size_t size = PE_SAMPLE32_NUM_WORDS * 2;

    pe_image_t image;
    if (!pe_parse(blob, size, &image)) {
        printf("FAIL: sample PE32 parse: %s\n",
            image.error ? image.error : "unknown");
        return 1;
    }
    if (image.machine != PE_MACHINE_I386 || image.is_plus) {
        printf("FAIL: sample machine flags\n");
        return 1;
    }
    if (image.entry_point_rva != PE_SAMPLE32_ENTRY_RVA) {
        printf("FAIL: sample entry RVA\n");
        return 1;
    }
    printf("PASS: sample PE32 parses (machine i386, entry 0x%x)\n",
        (unsigned)PE_SAMPLE32_ENTRY_RVA);

    if (!pe_run_can_load(&image)) {
        printf("FAIL: sample outside 1b envelope\n");
        return 1;
    }
    printf("PASS: sample satisfies the layer-1b envelope\n");

    /* envelope rejections */
    pe_image_t probe = image;
    probe.machine = PE_MACHINE_AMD64;
    probe.is_plus = true;
    if (pe_run_can_load(&probe)) {
        printf("FAIL: amd64 image accepted by an i386-kernel envelope\n");
        return 1;
    }
    printf("PASS: wrong machine refused\n");

    probe = image;
    probe.image_base = 0x400000ULL;
    if (pe_run_can_load(&probe)) {
        printf("FAIL: nonstandard ImageBase accepted\n");
        return 1;
    }
    printf("PASS: ImageBase mismatch refused\n");

    probe = image;
    probe.section_count = 2;
    if (pe_run_can_load(&probe)) {
        printf("FAIL: multi-section image accepted\n");
        return 1;
    }
    printf("PASS: multi-section refused\n");

    probe = image;
    probe.entry_point_rva = image.sections[0].virtual_address - 1;
    if (pe_run_can_load(&probe)) {
        printf("FAIL: entry before section base accepted\n");
        return 1;
    }
    printf("PASS: entry outside section refused\n");

    probe = image;
    probe.sections[0].raw_size = 4097;
    if (pe_run_can_load(&probe)) {
        printf("FAIL: oversized section accepted\n");
        return 1;
    }
    printf("PASS: oversized section refused\n");

    printf("pe_run_host_test: all cases PASS\n");
    return 0;
}
