/*
 * src/arch/i386/divdi3.c
 *
 * 64-bit division/modulo helpers for the 32-bit (i386) RockOS build.
 *
 * On i386 GCC lowers every uint64_t '/' and '%' to calls to __udivdi3,
 * __umoddi3, __divdi3 and __moddi3, normally supplied by libgcc. The
 * x86_64-elf toolchain has no 32-bit libgcc, so we provide them here.
 *
 * IMPORTANT: this file must never use '/' or '%' on 64-bit values itself
 * (that would recurse). Only shifts, compares and adds on 64-bit values,
 * plus native 32-bit divides.
 */

#include <stdbool.h>
#include <stdint.h>

/* edx:eax / divisor -> eax quotient, edx remainder. Requires hi < divisor. */
static inline uint32_t div_64_by_32(uint32_t hi, uint32_t lo, uint32_t divisor,
    uint32_t* remainder)
{
    uint32_t quotient, rem;
    __asm__ ("divl %4"
        : "=a"(quotient), "=d"(rem)
        : "a"(lo), "d"(hi), "rm"(divisor)
        : "cc");
    *remainder = rem;
    return quotient;
}

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t* rem)
{
    if (d == 0) {
        /* Real hardware would raise #DE; keep going with a sentinel. */
        if (rem) *rem = n;
        return ~(uint64_t)0;
    }

    /* Fast path: divisor fits in 32 bits (ticks / 100, value % 10, ...). */
    if ((d >> 32) == 0) {
        uint32_t d32 = (uint32_t)d;
        uint32_t hi = (uint32_t)(n >> 32);
        uint32_t lo = (uint32_t)n;
        uint32_t q_hi = hi / d32;          /* native 32-bit divide */
        uint32_t r_hi = hi % d32;
        uint32_t r_lo;
        uint32_t q_lo = div_64_by_32(r_hi, lo, d32, &r_lo);
        if (rem) *rem = r_lo;
        return ((uint64_t)q_hi << 32) | q_lo;
    }

    /* Slow path: divisor >= 2^32, so the quotient is < 2^32. Shift-subtract. */
    {
        uint64_t q = 0;
        uint64_t r = 0;
        for (int i = 0; i < 64; i++) {
            uint64_t carry = r >> 63;
            r = (r << 1) | (n >> 63);
            n <<= 1;
            q <<= 1;
            if (carry || r >= d) {
                r -= d;
                q |= 1;
            }
        }
        if (rem) *rem = r;
        return q;
    }
}

__attribute__((used)) uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    return __udivmoddi4(n, d, 0);
}

__attribute__((used)) uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    __udivmoddi4(n, d, &r);
    return r;
}

__attribute__((used)) int64_t __divdi3(int64_t a, int64_t b)
{
    bool negative = (a < 0) != (b < 0);
    uint64_t ua = a < 0 ? (uint64_t)0 - (uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? (uint64_t)0 - (uint64_t)b : (uint64_t)b;
    uint64_t q = __udivmoddi4(ua, ub, 0);
    return negative ? (int64_t)((uint64_t)0 - q) : (int64_t)q;
}

__attribute__((used)) int64_t __moddi3(int64_t a, int64_t b)
{
    uint64_t ua = a < 0 ? (uint64_t)0 - (uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? (uint64_t)0 - (uint64_t)b : (uint64_t)b;
    uint64_t r;
    __udivmoddi4(ua, ub, &r);
    return a < 0 ? (int64_t)((uint64_t)0 - r) : (int64_t)r;
}
