/*
 * Host test for rok.c: builds a synthetic .rok blob and asserts parse
 * results and error paths.
 *
 * Build: cc -Iinclude -o rok_host_test tests/rok_host_test.c src/kernel/rok.c -Wall -Wextra
 */

#include "../include/rok.h"

#include <stdio.h>
#include <string.h>

static uint8_t blob[8192];

static int check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    return ok ? 1 : 0;
}

static void make_rok(bool arch64, uint64_t entry, uint64_t code_size) {
    memset(blob, 0, sizeof(blob));
    blob[0] = 'R';
    blob[1] = 'O';
    blob[2] = 'K';
    blob[3] = '1';
    blob[4] = ROK_HEADER_SIZE;
    blob[8] = arch64 ? ROK_FLAG_ARCH_64 : 0;
    blob[12] = (uint8_t)(entry & 0xFF);
    blob[13] = (uint8_t)((entry >> 8) & 0xFF);
    blob[14] = (uint8_t)((entry >> 16) & 0xFF);
    blob[15] = (uint8_t)((entry >> 24) & 0xFF);
    blob[16] = (uint8_t)(code_size & 0xFF);
    blob[17] = (uint8_t)((code_size >> 8) & 0xFF);
    blob[18] = (uint8_t)((code_size >> 16) & 0xFF);
    blob[19] = (uint8_t)((code_size >> 24) & 0xFF);
    for (uint32_t index = 0; index < code_size; index++) {
        blob[ROK_HEADER_SIZE + index] = (uint8_t)(0x90 + (index % 16));
    }
}

int main(void) {
    rok_image_t image;
    rok_status_t status;

    /* valid 32-bit image */
    make_rok(false, 4, 16);
    status = rok_parse(blob, ROK_HEADER_SIZE + 16, &image);
    if (!check(status == ROK_STATUS_OK, "valid i386 image") ||
        !check(!image.is_64bit, "arch flag 32") ||
        !check(image.entry_offset == 4, "entry offset 4") ||
        !check(image.code_size == 16, "code size 16") ||
        !check(image.code != NULL, "code pointer")) {
        return 1;
    }

    /* valid 64-bit image */
    make_rok(true, 2, 8);
    status = rok_parse(blob, ROK_HEADER_SIZE + 8, &image);
    if (!(status == ROK_STATUS_OK && image.is_64bit &&
        image.entry_offset == 2 && image.code_size == 8)) {
        printf("FAIL: valid amd64 image\n");
        return 1;
    }
    printf("PASS: valid amd64 image\n");

    /* bad magic */
    make_rok(false, 0, 4);
    blob[0] = 'X';
    status = rok_parse(blob, ROK_HEADER_SIZE + 4, &image);
    if (!check(status == ROK_STATUS_BAD_MAGIC, "bad magic") ||
        !check(image.error != NULL, "error text set")) {
        return 1;
    }

    /* wrong header size */
    make_rok(false, 0, 4);
    blob[4] = 32;
    status = rok_parse(blob, ROK_HEADER_SIZE + 4, &image);
    if (!check(status == ROK_STATUS_BAD_HEADER, "bad header size")) {
        return 1;
    }

    /* unknown flag bit */
    make_rok(false, 0, 4);
    blob[9] = 0x80;
    status = rok_parse(blob, ROK_HEADER_SIZE + 4, &image);
    if (!check(status == ROK_STATUS_BAD_HEADER, "unknown flag")) {
        return 1;
    }

    /* entry beyond code */
    make_rok(false, 100, 4);
    status = rok_parse(blob, ROK_HEADER_SIZE + 4, &image);
    if (!check(status == ROK_STATUS_NO_ENTRY, "entry beyond code")) {
        return 1;
    }

    /* code too big */
    make_rok(false, 0, 5000);
    status = rok_parse(blob, ROK_HEADER_SIZE + 5000, &image);
    if (!check(status == ROK_STATUS_CODE_TOO_BIG, "code too big")) {
        return 1;
    }

    /* truncated blob */
    make_rok(false, 4, 16);
    status = rok_parse(blob, ROK_HEADER_SIZE + 15, &image);
    if (!check(status == ROK_STATUS_TRUNCATED, "truncated blob")) {
        return 1;
    }

    /* reserved bytes must be zero */
    make_rok(false, 0, 4);
    blob[21] = 7;
    status = rok_parse(blob, ROK_HEADER_SIZE + 4, &image);
    if (!check(status == ROK_STATUS_BAD_HEADER, "reserved nonzero")) {
        return 1;
    }

    /* blob too small */
    make_rok(false, 0, 0);
    status = rok_parse(blob, 8, &image);
    if (!check(status == ROK_STATUS_TRUNCATED, "blob too small")) {
        return 1;
    }

    printf("rok_host_test: all cases PASS\n");
    return 0;
}
