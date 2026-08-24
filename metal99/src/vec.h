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

#endif /* VEC_H */
