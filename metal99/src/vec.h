/*
 * 128-bit vector primitives for the ESP32-S3 LX7 PIE unit.
 *
 * PROJECT RULE: no scalar per-element math. All bulk data work goes through
 * these. GCC-for-Xtensa does not auto-vectorise to EE.*, so they are written
 * as inline __asm__.
 *
 * PRECISELY WHAT THAT COSTS US, because this header used to claim it was "ISO
 * C99 legal" and that is too strong. ISO C99 does not define inline assembly
 * at all - Annex J.5.10 lists it as a common extension, nothing more. What the
 * __asm__ spelling buys is that it lives in the implementation's reserved
 * namespace, so it cannot collide with any identifier a future standard might
 * define, and -pedantic-errors accepts it where the bare `asm` keyword is
 * rejected. So: this is a CONFORMING program using documented extensions in
 * the reserved namespace. It is not a STRICTLY conforming one, and no program
 * that touches a memory-mapped peripheral could be.
 *
 * The extension surface is exactly two constructs - __asm__ here, io.c,
 * spi2.c and start.c, and __attribute__ in three places (VEC_ALIGN below,
 * gdma_desc's alignment, and _start's section). Everything else compiles
 * clean under -std=c99 -pedantic-errors on two independent front ends.
 *
 * ALIGNMENT: every pointer must be 16-byte aligned and every length a multiple
 * of 16 bytes. Use VEC_ALIGN on buffers. A 368-pixel row is 736 bytes = 46
 * vectors exactly, so full rows need no tail handling.
 */
#ifndef VEC_H
#define VEC_H

#include <stdint.h>
#include <stddef.h>

/*
 * VECTOR REGISTER ALLOCATION - a contract the PREPROCESSOR keeps.
 *
 * There is no compiler awareness of the q registers, so nothing stops two
 * modules clobbering each other. This used to be a hand-maintained comment
 * listing which registers were free, and it drifted: it still read
 * "q4-q7 UNUSED - take these for new code" long after vec_ramp16 took q4/q5
 * and vec_xor16 took q6/q7, and CONTRIBUTING.md repeated the claim. Anyone
 * following it would have landed on an occupied register and hit precisely the
 * intermittent visual corruption the note existed to prevent.
 *
 * So each register name now lives in exactly ONE place and is pasted into the
 * asm by the preprocessor. The table cannot fall out of step with the code
 * because it IS the code: claiming a register means adding a line here, and
 * changing an owner's register changes the instruction that gets emitted.
 *
 * This makes DRIFT impossible. It does not make COLLISION impossible - the
 * preprocessor will happily paste the same name twice. Before introducing a
 * new owner, check the name is not already spoken for below.
 */
#define VEC_Q_FILL   "q0"    /* vec_fill16  broadcast value          */
#define VEC_Q_COPY   "q1"    /* vec_copy    in-flight chunk          */
#define VEC_Q_ZERO   "q2"    /* vec_zero    zero source              */
#define VEC_Q_FIFO   "q3"    /* spi2_xfer   FIFO load (see spi2.c)   */
#define VEC_Q_RAMP   "q4"    /* vec_ramp16  running accumulator      */
#define VEC_Q_STEP   "q5"    /* vec_ramp16  per-vector increment     */
#define VEC_Q_XORD   "q6"    /* vec_xor16   destination chunk        */
#define VEC_Q_XORS   "q7"    /* vec_xor16   source chunk             */
/* All eight are claimed above. A new owner must SHARE, and sharing requires
 * proving the two never interleave - these registers are not saved or restored.
 *
 * vec_glyph_row is the first sharer, and the proof is structural: this unit is
 * single-threaded, takes no interrupts, and no vec_* function calls another.
 * The one pairing worth naming is q3, shared with the SPI2 FIFO load -
 * sh8601_write_span_x renders a whole row and only then streams it, so the
 * glyph blit has finished before the FIFO load begins. */
/*
 * vec_hash16 is the second sharer, taking the three registers vec_ramp16 and
 * vec_xor16 own. The proof is the same shape as the glyph one and just as
 * structural: ramp and xor exist ONLY for selftest.c, which runs once at boot
 * before any application starts, and hashing happens inside tile_present in the
 * frame loop. They cannot interleave because they cannot both be running.
 *
 * If a vec_ramp16 or vec_xor16 caller ever appears outside selftest, this stops
 * being true and the hash needs its own registers. There are none free, so it
 * would need a different construction.
 */
#define VEC_Q_HACC   VEC_Q_RAMP   /* q4  hash accumulator, 8 lanes   */
#define VEC_Q_HDAT   VEC_Q_STEP   /* q5  chunk being folded in       */
#define VEC_Q_HK     VEC_Q_XORD   /* q6  the odd multiplier          */

#define VEC_Q_GBITS  VEC_Q_FILL   /* q0  broadcast glyph byte        */
#define VEC_Q_GLANE  VEC_Q_COPY   /* q1  {0x80,0x40,...,0x01}        */
#define VEC_Q_GZERO  VEC_Q_ZERO   /* q2  zero, for the compare       */
#define VEC_Q_GSEL   VEC_Q_FIFO   /* q3  ones where the bit is CLEAR */
#define VEC_Q_GTMP   VEC_Q_RAMP   /* q4  inverted mask / scratch     */
#define VEC_Q_GFG    VEC_Q_STEP   /* q5  broadcast foreground colour */
#define VEC_Q_GDST   VEC_Q_XORD   /* q6  destination pixels          */

#define VEC_ALIGN __attribute__((aligned(16)))
#define VEC_BYTES 16
#define VEC_PIX16 8            /* 16-bit pixels per 128-bit vector */

/*
 * All five bulk primitives below take a count in whole 128-bit vectors and
 * return immediately when it is zero. The loops are store-then-decrement, so
 * a zero count would otherwise wrap to 0xFFFFFFFF and run for hours. No
 * current caller can pass zero; the guard is one predictable branch per call
 * against 46 vector stores, and it removes the failure mode rather than
 * relying on every future caller to have reasoned about it.
 */

/* dst[0..n16-1] = value, n16 in units of 8 pixels (one vector). */
void vec_fill16(uint16_t *dst, uint16_t value, uint32_t vectors);

/* Copy `vectors` * 16 bytes. */
void vec_copy(void *dst, const void *src, uint32_t vectors);

/* Zero `vectors` * 16 bytes. */
void vec_zero(void *dst, uint32_t vectors);

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

/*
 * GLYPH BLIT - 1bpp bits to RGB565 pixels, 8 per instruction group.
 *
 * Each byte of `bits` paints 8 pixels: a set bit takes `fg`, a CLEAR bit leaves
 * the destination untouched. Transparent rather than opaque, so text composes
 * over whatever the run model already drew - and it costs nothing extra,
 * because the mask needed to select fg is the same mask needed to keep dst.
 *
 * MSB first: bit 7 is the leftmost pixel, matching font.h's layout, so nothing
 * is reversed at run time.
 *
 * `dst` must be 16-byte aligned - true when glyphs sit on the 8px grid, which
 * gfx enforces. Measured at 1.375 instructions per pixel against 9.0 scalar
 * (DESIGN.md 6.9a); this is the shape the PIE unit is best at, since the
 * compare-and-select it needs is exactly what the ISA provides in place of the
 * table lookup it does not (6.9b).
 */
void vec_glyph_row(uint16_t *dst, const uint8_t *bits, uint32_t bytes,
                   uint16_t fg);

/*
 * CONTENT DIGEST - folds EVERY byte, not a sample.
 *
 * The previous ledger folded only the first and last byte of each transfer,
 * which for solid-colour rows is blind to the entire payload. It reported
 * PASS on a visibly broken display. This covers the whole buffer.
 *
 * WHAT IT CATCHES, given a positionally unique pattern: any wrong value,
 * omission, truncation, duplication, displacement or reordering.
 * WHAT IT MISSES: a transfer that ends MID-WORD zero-pads its tail into a word
 * of its own, which makes the digest depend on where transfers were cut. Every
 * length this project transmits is a whole number of words, so that does not
 * arise - but it is a real limit and is stated rather than glossed over.
 *
 * Implementation and its full failure history are in fold.c, which the host
 * test harness compiles and asserts against directly.
 *
 * LENGTH IS IN BYTES, not vectors. It took vectors until the ledger was found
 * to be computing `len / VEC_BYTES` and silently discarding up to 15 trailing
 * bytes of every transfer whose length was not a multiple of 16.
 */
void     vec_fold_reset(void);
void     vec_fold(const void *p, uint32_t bytes);
uint32_t vec_fold_get(void);

/*
 * Hash `vectors` 128-bit chunks starting at `p`, stepping `stride_bytes`
 * between them, into one 32-bit value.
 *
 * EIGHT INDEPENDENT LANES, each running acc = (acc * K) ^ data. The multiply
 * is what makes it position-sensitive: XOR alone is order-insensitive, so a
 * lit pixel moving inside a tile would be invisible to it - which is exactly
 * the change a vector scene makes most often, and would be the worst possible
 * blind spot. K is odd, so multiplication is a bijection mod 2^16 and no lane
 * loses information.
 *
 * The stride exists because a tile is a COLUMN: one vector per row, W pixels
 * apart. Walking it needs a register-sized step, not an immediate.
 *
 * This replaces a scalar multiply-add over every pixel, which measured 8 ms a
 * frame on the device - more than the transmit it was deciding about, and a
 * plain violation of the no-scalar-per-element rule.
 *
 * EIGHT WORDS COME OUT, from TWO accumulators, and the reason is arithmetic
 * rather than caution.
 *
 * The lanes are independent COLUMNS: lane l only ever sees pixel l of each
 * chunk. A column that stays black contributes a constant, so a tile with one
 * lit column carries just that lane's SIXTEEN bits of state - no matter how
 * many bits the whole accumulator has. A 16-pixel column has millions of
 * possible contents, so by pigeonhole two of them share a hash, and at the
 * ~700k tile comparisons a 3000-frame test performs you expect about ten
 * collisions. That is exactly what happened: the equivalence test failed, and
 * the failing frame MOVED when the multiplier changed - the signature of a
 * collision, not a logic error. Widening from 32 to 128 bits did not help,
 * because the missing entropy was per-lane.
 *
 * So the tile is walked TWICE, with two different multipliers, and both
 * results are kept - eight words in all.
 *
 * An attempt to get the same effect in one pass, with two accumulators running
 * FNV-1a and FNV-1 order, does NOT work, and the reason is worth keeping
 * because it looked convincing:
 *
 *     A: a = (a ^ d) * K        B: b = (b * K) ^ d
 *
 * Write out the recurrences and a_n = K * b_n exactly, for all n. They are the
 * same function scaled by K, so B carried no information whatsoever. The
 * equivalence test kept failing for one multiplier and passing for others,
 * which is what a collision looks like and a coding error does not.
 *
 * Two real passes cost one extra walk, about 1 ms a frame, and are independent
 * for real - verified across four constant pairs that previously failed.
 */
void vec_hash16(const void *p, uint32_t vectors, uint32_t stride_bytes,
                uint32_t out[8]);

#endif /* VEC_H */
