/*
 * memcpy and memset for vendored code. See shim/README.md.
 *
 * BYTE LOOPS, and that is not a violation of the no-scalar-per-element rule.
 * That rule governs BULK DATA WORK - framebuffers, spans, digests - where the
 * vector unit is the difference between 3 fps and 60. These serve a decoder's
 * internal moves of a few hundred bytes at arbitrary alignment, where vec_copy
 * cannot be used (it requires 16-byte alignment and whole vectors) and where
 * the cost is lost in the arithmetic around it.
 *
 * GCC may also emit calls to these for struct assignment, which is exactly how
 * this project has twice been bitten by memcpy appearing with no libc to
 * provide it. Now there is one, on purpose, and small enough to reason about.
 */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

/*
 * memmove: the overlap-safe one. minimp3 uses it to slide its bit reservoir
 * along, which overlaps by construction - copying forward there would smear
 * the buffer, and the failure would look like decoder corruption rather than a
 * missing library function.
 */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0u) return dst;
    if (d < s) { while (n--) *d++ = *s++; }
    else       { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
