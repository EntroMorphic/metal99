/*
 * Host implementation of the vec_* API so metal99's rendering code can be
 * exercised on a desktop, where iteration is ~200ms instead of a flash cycle.
 *
 * These ARE scalar. That is deliberate and does not breach the project's
 * no-scalar rule, which governs code that runs ON THE DEVICE. x86 has no
 * Xtensa PIE unit; the point here is to validate rendering LOGIC, not
 * instruction selection.
 */
#include "vec.h"

void vec_fill16(uint16_t *dst, uint16_t value, uint32_t vectors)
{
    uint32_t n = vectors * (VEC_BYTES / 2u), i;
    for (i = 0u; i < n; i++) dst[i] = value;
}

void vec_copy(void *dst, const void *src, uint32_t vectors)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    uint32_t n = vectors * VEC_BYTES, i;
    for (i = 0u; i < n; i++) d[i] = s[i];
}

void vec_zero(void *dst, uint32_t vectors)
{
    unsigned char *d = dst;
    uint32_t n = vectors * VEC_BYTES, i;
    for (i = 0u; i < n; i++) d[i] = 0u;
}
