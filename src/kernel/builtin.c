/*
 * builtin.c - freestanding C runtime subset.
 *
 * The kernel is compiled -ffreestanding, so the compiler is allowed to
 * emit calls to memcpy/memmove/memset even when the source never refers
 * to them directly (e.g. an inlined struct copy such as
 * `*dst = *src;` in pci.c under clang). GCC inlined these; clang emits
 * real calls, so provide thin definitions that dispatch to kernel
 * helpers instead of linking against a libc.
 */

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t count)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (count--) { *d++ = *s++; }
    return dst;
}

void *memmove(void *dst, const void *src, size_t count)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || count == 0) return dst;
    if (d < s) {
        while (count--) { *d++ = *s++; }
    } else {
        d += count;
        s += count;
        while (count--) { *--d = *--s; }
    }
    return dst;
}

void *memset(void *dst, int value, size_t count)
{
    uint8_t *d = (uint8_t *)dst;
    while (count--) { *d++ = (uint8_t)value; }
    return dst;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
