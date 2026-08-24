/*
 * 128-bit vector primitives for the ESP32-S3 LX7 PIE unit.
 *
 * PROJECT RULE: no scalar per-element math. All bulk data work goes through
 * these. GCC-for-Xtensa does not auto-vectorise to EE.*, so they are written
 * as inline __asm__ - which is ISO C99 legal (the reserved __asm__ spelling,
 * not the bare `asm` keyword that -pedantic-errors rejects).
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
 * VECTOR REGISTER ALLOCATION - a contract, not an accident.
 *
 * There is no compiler awareness of the q registers, so nothing stops two
 * modules clobbering each other. Current owners:
 *
 *   q0  vec_fill16
 *   q1  vec_copy
 *   q2  vec_zero
 *   q3  spi2 FIFO load
 *   q4-q7  UNUSED - take these for new code
 *
 * Any new EE.* code must claim a register here first. A collision would show
 * up as intermittent visual corruption, which is exactly the kind of bug this
 * project keeps paying for.
 */
#define VEC_ALIGN __attribute__((aligned(16)))
#define VEC_BYTES 16
#define VEC_PIX16 8            /* 16-bit pixels per 128-bit vector */

/* dst[0..n16-1] = value, n16 in units of 8 pixels (one vector). */
void vec_fill16(uint16_t *dst, uint16_t value, uint32_t vectors);

/* Copy `vectors` * 16 bytes. */
void vec_copy(void *dst, const void *src, uint32_t vectors);

/* Zero `vectors` * 16 bytes. */
void vec_zero(void *dst, uint32_t vectors);

/* dst[i] = start + i*step, i in units of 16-bit lanes. Builds a positionally
 * unique ramp with no scalar per-element work. */
void vec_ramp16(uint16_t *dst, uint16_t start, uint16_t step, uint32_t vectors);

/* dst ^= src, 16-bit lanes. */
void vec_xor16(uint16_t *dst, const uint16_t *src, uint32_t vectors);

/*
 * CONTENT DIGEST - folds EVERY byte, not a sample.
 *
 * The previous ledger folded only the first and last byte of each transfer,
 * which for solid-colour rows is blind to the entire payload. It reported
 * PASS on a visibly broken display. This covers the whole buffer.
 *
 * WHAT IT CATCHES, given a positionally unique pattern: any wrong value,
 * omission, truncation, or duplication (a duplicate XORs a chunk twice and
 * cancels it, changing the result).
 * WHAT IT MISSES: an exact reordering of whole 128-bit chunks within a single
 * transfer. DMA reads descriptors sequentially, so that is not a failure mode
 * here - but it is a real limit and is stated rather than glossed over.
 */
void     vec_fold_reset(void);
void     vec_fold(const void *p, uint32_t vectors);
uint32_t vec_fold_get(void);

#endif /* VEC_H */
