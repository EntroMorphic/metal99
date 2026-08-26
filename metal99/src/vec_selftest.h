/*
 * VECTOR PRIMITIVES RESERVED FOR selftest.c.
 *
 * These live here, and NOT in vec.h, because they are unsafe to call from an
 * application and there was previously nothing but a comment saying so.
 *
 * vec_ramp16 owns q4/q5 and vec_xor16 owns q6/q7. vec_hash16 - which runs
 * inside tile_present, on the hot path, every frame - shares q4, q5 and q6.
 * That sharing is sound only because ramp and xor run exactly once, at boot,
 * from the self-test, and hashing does not start until an application does.
 *
 * WHAT HAPPENS IF THAT STOPS BEING TRUE is the reason this header exists.
 * Calling vec_xor16 from a rowfn clobbers q6, which is the hash multiplier,
 * in the middle of a frame. The hashes come out wrong, tiles that changed are
 * declared clean, and stale pixels stay on the glass. Nothing returns an
 * error. No test fails. It is indistinguishable from the span-boundary debris
 * this project spent two days chasing into the transport - and it would send
 * the next person to the same wrong place.
 *
 * Because these registers are not saved or restored, that failure cannot be
 * detected at runtime, only prevented at compile time. So: including this
 * header is the act of asserting you run before any application does. Nothing
 * but selftest.c should include it.
 */
#ifndef VEC_SELFTEST_H
#define VEC_SELFTEST_H

#include <stdint.h>

/*
 * dst[i] = start + i*step, i in units of 16-bit lanes, with no scalar
 * per-element work.
 *
 * SATURATES. The accumulate is ee.vadds.s16, a signed saturating add, so once
 * start + i*step passes 32767 every remaining lane pins to 0x7FFF and the ramp
 * stops being a ramp. Callers wanting a positionally unique pattern must keep
 * start + (n-1)*step within int16 - selftest.c enforces that with a static
 * assertion after its probe pattern silently went flat across 72% of each row.
 */
void vec_ramp16(uint16_t *dst, uint16_t start, uint16_t step, uint32_t vectors);

/* dst ^= src, 16-bit lanes. */
void vec_xor16(uint16_t *dst, const uint16_t *src, uint32_t vectors);

#endif /* VEC_SELFTEST_H */
