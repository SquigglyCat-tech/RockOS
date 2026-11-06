#ifndef ROCKOS_ROK_H
#define ROCKOS_ROK_H

/*
 * rok.h - RockOS native executable loader (.rok), Wine layer 1 sibling.
 *
 * A .rok file is the smallest possible native executable for RockOS:
 * raw machine code for the running kernel architecture, loaded into a
 * user code page and entered in ring 3 through the existing layer 0
 * syscall surface (int 0x80; putchar, exit, mmap, munmap).
 *
 * File layout (little-endian, byte-exact):
 *
 *   offset  size  field
 *   0       4     magic 'R','O','K','1'
 *   4       4     header_size (must be 24)
 *   8       4     flags: bit0 = arch (0 = i386, 1 = amd64), rest zero
 *   12      4     entry_offset (rva into the payload)
 *   16      4     code_size (payload bytes after the header)
 *   20      4     reserved (must be 0)
 *   24      *     payload
 *
 * One code page holds up to 4096 bytes of payload; the whole file must
 * fit within 24 + 4096 bytes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROK_MAGIC_0 'R'
#define ROK_MAGIC_1 'O'
#define ROK_MAGIC_2 'K'
#define ROK_MAGIC_3 '1'
#define ROK_HEADER_SIZE 24
#define ROK_FLAG_ARCH_64 0x1
#define ROK_CODE_SIZE_MAX 4096U

typedef enum {
    ROK_STATUS_OK = 0,
    ROK_STATUS_TRUNCATED,
    ROK_STATUS_BAD_MAGIC,
    ROK_STATUS_BAD_HEADER,
    ROK_STATUS_BAD_ARCH,
    ROK_STATUS_NO_ENTRY,
    ROK_STATUS_CODE_TOO_BIG,
    ROK_STATUS_UNKNOWN_ERROR
} rok_status_t;

typedef struct {
    bool is_64bit;      /* payload machine arch */
    uint64_t entry_offset;   /* offset of entry (rva into payload) */
    uint64_t code_size;      /* payload bytes */
    const uint8_t* code;     /* payload start inside the caller blob */
    const char* error;       /* NULL unless status != ROK_STATUS_OK */
} rok_image_t;

/* Parse a .rok blob. Returns ROK_STATUS_OK (and fills image) when it
 * is a valid RockOS native executable, otherwise the error status with
 * error filled. The returned code pointer points into the caller blob. */
rok_status_t rok_parse(const uint8_t* blob, size_t blob_size,
    rok_image_t* image);

const char* rok_strerror(rok_status_t status);

/* Cap a user-entered code size (small helper used by callers and the
 * host test to compare against the kernel limit). */
uint64_t rok_code_size_limit(void);

#endif /* ROCKOS_ROK_H */
