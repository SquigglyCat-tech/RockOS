/*
 * rok.c - RockOS native executable loader (.rok), see rok.h.
 *
 * Freestanding: no libc, no malloc, safe on malformed input. Used by
 * the shell's `run` command (Wine layer 1 native executable support).
 */

#include "rok.h"

#include <stddef.h>
#include <stdint.h>

static rok_status_t rok_fail(rok_status_t status, rok_image_t* image) {
    if (image) {
        image->error = rok_strerror(status);
    }
    return status;
}

uint64_t rok_code_size_limit(void) {
    return (uint64_t)ROK_CODE_SIZE_MAX;
}

const char* rok_strerror(rok_status_t status) {
    switch (status) {
        case ROK_STATUS_OK:          return "ok";
        case ROK_STATUS_TRUNCATED:   return "file truncated";
        case ROK_STATUS_BAD_MAGIC:   return "not a .rok file";
        case ROK_STATUS_BAD_HEADER:  return "invalid .rok header";
        case ROK_STATUS_BAD_ARCH:    return "payload arch mismatch";
        case ROK_STATUS_NO_ENTRY:    return "entry point outside code";
        case ROK_STATUS_CODE_TOO_BIG: return "code too large (4096 max)";
        default:                     return "unknown error";
    }
}

rok_status_t rok_parse(const uint8_t* blob, size_t blob_size,
    rok_image_t* image) {
    if (image) {
        image->is_64bit = false;
        image->entry_offset = 0;
        image->code_size = 0;
        image->code = 0;
        image->error = 0;
    }
    if (!blob || !image) {
        return rok_fail(ROK_STATUS_UNKNOWN_ERROR, image);
    }
    if (blob_size < ROK_HEADER_SIZE) {
        return rok_fail(ROK_STATUS_TRUNCATED, image);
    }
    if (blob[0] != ROK_MAGIC_0 || blob[1] != ROK_MAGIC_1 ||
        blob[2] != ROK_MAGIC_2 || blob[3] != ROK_MAGIC_3) {
        return rok_fail(ROK_STATUS_BAD_MAGIC, image);
    }
    const uint8_t* header = blob;
    uint32_t header_size = (uint32_t)header[4] |
        (uint32_t)header[5] << 8 | (uint32_t)header[6] << 16 |
        (uint32_t)header[7] << 24;
    if (header_size != ROK_HEADER_SIZE) {
        return rok_fail(ROK_STATUS_BAD_HEADER, image);
    }
    uint32_t flags = (uint32_t)header[8] | (uint32_t)header[9] << 8 |
        (uint32_t)header[10] << 16 | (uint32_t)header[11] << 24;
    if (flags & ~(uint32_t)1U) {
        return rok_fail(ROK_STATUS_BAD_HEADER, image);
    }
    uint64_t entry_offset = (uint64_t)((uint32_t)blob[12] |
        (uint32_t)blob[13] << 8 | (uint32_t)blob[14] << 16 |
        (uint32_t)blob[15] << 24);
    uint64_t code_size = (uint64_t)((uint32_t)blob[16] |
        (uint32_t)blob[17] << 8 | (uint32_t)blob[18] << 16 |
        (uint32_t)blob[19] << 24);
    const uint8_t* reserved = blob + 20;
    if (blob[20] || blob[21] || blob[22] || blob[23]) {
        (void)reserved;
        return rok_fail(ROK_STATUS_BAD_HEADER, image);
    }
    if (code_size > ROK_CODE_SIZE_MAX) {
        return rok_fail(ROK_STATUS_CODE_TOO_BIG, image);
    }
    if (entry_offset >= code_size) {
        return rok_fail(ROK_STATUS_NO_ENTRY, image);
    }
    if (blob_size < ROK_HEADER_SIZE + (size_t)code_size) {
        return rok_fail(ROK_STATUS_TRUNCATED, image);
    }
    image->is_64bit = (flags & ROK_FLAG_ARCH_64) != 0;
    image->entry_offset = entry_offset;
    image->code_size = code_size;
    image->code = blob + ROK_HEADER_SIZE;
    image->error = 0;
    return ROK_STATUS_OK;
}
