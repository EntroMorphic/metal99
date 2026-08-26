/*
 * Host implementation of the vec_* API so metal99's rendering code can be
 * exercised on a desktop, where iteration is ~200ms instead of a flash cycle.
 *
 * These ARE scalar. That is deliberate and does not breach the project's
 * no-scalar rule, which governs code that runs ON THE DEVICE. x86 has no
 * Xtensa PIE unit; the point here is to validate rendering LOGIC, not
 * instruction selection.
 *
 * vec_fold() is deliberately NOT here. It is pure C99 and lives in
 * metal99/src/fold.c, which this harness compiles directly - so the digest
 * under test is the same code the device runs, not a reimplementation of it.
 * An instrument checked against a copy of itself is not checked at all.
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

/*
 * Mirrors the device exactly: ee.movi.32.q seeds eight lanes with
 * start + k*step, then each further vector is ee.vadds.s16 of bump = step*8 -
 * a SIGNED SATURATING add, not a wrapping one.
 *
 * An earlier version of this file wrapped instead, with a comment claiming the
 * self-test's ramp "never reaches saturation". It does: 0x1234 step 0x0111
 * saturates at lane 103 and pins the remaining 265 of 368 lanes to 0x7FFF.
 * Modelling it as wrapping would have hidden that on the host forever, which
 * is precisely the divergence a host harness is supposed to remove.
 */
static int16_t sat_add_s16(int16_t a, int16_t b)
{
    int32_t r = (int32_t)a + (int32_t)b;
    if (r >  32767) r =  32767;
    if (r < -32768) r = -32768;
    return (int16_t)r;
}

void vec_ramp16(uint16_t *dst, uint16_t start, uint16_t step, uint32_t vectors)
{
    int16_t lane[8];
    int16_t bump = (int16_t)(uint16_t)(step * 8u);
    uint32_t v, k;

    for (k = 0u; k < 8u; k++)
        lane[k] = (int16_t)(uint16_t)(start + (uint16_t)(k * step));

    for (v = 0u; v < vectors; v++) {
        for (k = 0u; k < 8u; k++) dst[v * 8u + k] = (uint16_t)lane[k];
        for (k = 0u; k < 8u; k++) lane[k] = sat_add_s16(lane[k], bump);
    }
}

void vec_xor16(uint16_t *dst, const uint16_t *src, uint32_t vectors)
{
    uint32_t n = vectors * (VEC_BYTES / 2u), i;
    for (i = 0u; i < n; i++) dst[i] = (uint16_t)(dst[i] ^ src[i]);
}

/* Scalar mirror of the device blit: set bit -> fg, clear bit -> leave dst. */
void vec_glyph_row(uint16_t *dst, const uint8_t *bits, uint32_t bytes,
                   uint16_t fg)
{
    uint32_t i, b;
    for (i = 0u; i < bytes; i++)
        for (b = 0u; b < 8u; b++)
            if (bits[i] & (0x80u >> b)) dst[i * 8u + b] = fg;
}

/*
 * Scalar mirror of vec_hash16, lane for lane.
 *
 * The device version runs eight 16-bit lanes of acc = (acc * K) ^ data through
 * the PIE unit. This reproduces that exactly - same lane count, same constant,
 * same fold - so a hash computed here means what it means on hardware. A host
 * stub that merely "hashes somehow" would let the host tests pass while the
 * device disagreed about which tiles changed.
 */
#ifndef VEC_HASH_K
#define VEC_HASH_K 0x9E37u
#endif
#ifndef VEC_HASH_K2
#define VEC_HASH_K2 0xB2C5u
#endif

void vec_hash16(const void *p, uint32_t vectors, uint32_t stride_bytes,
                uint32_t out[8])
{
    static const uint16_t K[2] = { VEC_HASH_K, VEC_HASH_K2 };
    uint32_t pass, i, l;

    if (vectors == 0u) { for (l = 0u; l < 8u; l++) out[l] = 0u; return; }

    for (pass = 0u; pass < 2u; pass++) {
        const unsigned char *ptr = (const unsigned char *)p;
        uint16_t acc[8];
        for (l = 0u; l < 8u; l++) acc[l] = 0u;
        for (i = 0u; i < vectors; i++) {
            for (l = 0u; l < 8u; l++) {
                uint16_t d = (uint16_t)(ptr[l * 2u] | ((uint16_t)ptr[l * 2u + 1u] << 8));
                acc[l] = (uint16_t)((uint16_t)(acc[l] ^ d) * K[pass]);
            }
            ptr += stride_bytes;
        }
        for (l = 0u; l < 4u; l++)
            out[pass * 4u + l] = (uint32_t)acc[l * 2u]
                               | ((uint32_t)acc[l * 2u + 1u] << 16);
    }
}
