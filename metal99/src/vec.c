#include "vec.h"

void vec_fill16(uint16_t *dst, uint16_t value, uint32_t vectors)
{
    /* EE.VLDBC.16 broadcasts one halfword to all eight lanes in a single
     * instruction - no scalar seed loop. */
    uint16_t v = value;
    if (vectors == 0u) return;

    /* "memory" is load-bearing: the asm reads `v` through a pointer, and
     * without it nothing tells GCC the store to `v` is observed here. It keeps
     * `v` addressable today because the address escapes, but it is not
     * obliged to order the store before the asm. */
    __asm__ __volatile__ ("ee.vldbc.16 " VEC_Q_FILL ", %0" : : "a"(&v) : "memory");

    __asm__ __volatile__ (
        "1:                                       \n"
        "  ee.vst.128.ip " VEC_Q_FILL ", %0, 16   \n"   /* 8 px, post-inc 16 */
        "  addi.n        %1, %1, -1               \n"
        "  bnez          %1, 1b                   \n"
        : "+a"(dst), "+a"(vectors) : : "memory");
}

void vec_copy(void *dst, const void *src, uint32_t vectors)
{
    if (vectors == 0u) return;
    __asm__ __volatile__ (
        "1:                                       \n"
        "  ee.vld.128.ip " VEC_Q_COPY ", %1, 16   \n"
        "  ee.vst.128.ip " VEC_Q_COPY ", %0, 16   \n"
        "  addi.n        %2, %2, -1               \n"
        "  bnez          %2, 1b                   \n"
        : "+a"(dst), "+a"(src), "+a"(vectors) : : "memory");
}

void vec_zero(void *dst, uint32_t vectors)
{
    if (vectors == 0u) return;
    __asm__ __volatile__ (
        "  ee.zero.q     " VEC_Q_ZERO "           \n"
        "1:                                       \n"
        "  ee.vst.128.ip " VEC_Q_ZERO ", %0, 16   \n"
        "  addi.n        %1, %1, -1               \n"
        "  bnez          %1, 1b                   \n"
        : "+a"(dst), "+a"(vectors) : : "memory");
}

void vec_ramp16(uint16_t *dst, uint16_t start, uint16_t step, uint32_t vectors)
{
    /* Seed the accumulator with [start, start+step, ... start+7*step] by
     * inserting four 32-bit words, each holding two 16-bit lanes. O(1), not
     * per element. */
    uint32_t w0 = (uint32_t)start | ((uint32_t)(uint16_t)(start + step) << 16);
    uint32_t w1 = (uint32_t)(uint16_t)(start + 2*step) | ((uint32_t)(uint16_t)(start + 3*step) << 16);
    uint32_t w2 = (uint32_t)(uint16_t)(start + 4*step) | ((uint32_t)(uint16_t)(start + 5*step) << 16);
    uint32_t w3 = (uint32_t)(uint16_t)(start + 6*step) | ((uint32_t)(uint16_t)(start + 7*step) << 16);
    uint16_t bump = (uint16_t)(step * 8);

    if (vectors == 0u) return;

    __asm__ __volatile__ (
        "ee.movi.32.q " VEC_Q_RAMP ", %0, 0 \n"
        "ee.movi.32.q " VEC_Q_RAMP ", %1, 1 \n"
        "ee.movi.32.q " VEC_Q_RAMP ", %2, 2 \n"
        "ee.movi.32.q " VEC_Q_RAMP ", %3, 3 \n"
        : : "a"(w0), "a"(w1), "a"(w2), "a"(w3));
    /* "memory" for the same reason as vec_fill16: &bump escapes into the asm. */
    __asm__ __volatile__ ("ee.vldbc.16 " VEC_Q_STEP ", %0" : : "a"(&bump) : "memory");

    __asm__ __volatile__ (
        "1:                                                          \n"
        "  ee.vst.128.ip " VEC_Q_RAMP ", %0, 16                      \n"
        "  ee.vadds.s16  " VEC_Q_RAMP ", " VEC_Q_RAMP ", " VEC_Q_STEP "\n"
        "  addi.n        %1, %1, -1                                  \n"
        "  bnez          %1, 1b                                      \n"
        : "+a"(dst), "+a"(vectors) : : "memory");
}

void vec_xor16(uint16_t *dst, const uint16_t *src, uint32_t vectors)
{
    uint16_t *d = dst;
    if (vectors == 0u) return;
    __asm__ __volatile__ (
        "1:                                                          \n"
        "  ee.vld.128.ip " VEC_Q_XORD ", %0, 16                      \n"   /* dst */
        "  ee.vld.128.ip " VEC_Q_XORS ", %1, 16                      \n"   /* src */
        "  ee.xorq       " VEC_Q_XORD ", " VEC_Q_XORD ", " VEC_Q_XORS "\n"
        "  addi          %0, %0, -16                                 \n"   /* rewind */
        "  ee.vst.128.ip " VEC_Q_XORD ", %0, 16                      \n"
        "  addi.n        %2, %2, -1                                  \n"
        "  bnez          %2, 1b                                      \n"
        : "+a"(d), "+a"(src), "+a"(vectors) : : "memory");
}

void vec_glyph_row(uint16_t *dst, const uint8_t *bits, uint32_t bytes,
                   uint16_t fg)
{
    /* MSB first, so lane 0 - the leftmost pixel - tests bit 7. */
    static const uint16_t VEC_ALIGN lanebit[8] =
        { 0x80u, 0x40u, 0x20u, 0x10u, 0x08u, 0x04u, 0x02u, 0x01u };
    static const uint16_t VEC_ALIGN zero[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    uint16_t f = fg;

    if (bytes == 0u) return;

    __asm__ __volatile__ ("ee.vld.128.ip " VEC_Q_GLANE ", %0, 0" : : "a"(lanebit) : "memory");
    __asm__ __volatile__ ("ee.vld.128.ip " VEC_Q_GZERO ", %0, 0" : : "a"(zero)    : "memory");
    __asm__ __volatile__ ("ee.vldbc.16   " VEC_Q_GFG   ", %0"    : : "a"(&f)      : "memory");

    /* dst = (dst AND clear_mask) OR (fg AND NOT clear_mask).
     * One compare produces both halves of the select, which is why transparent
     * costs the same as opaque here. */
    __asm__ __volatile__ (
        "1:                                                                \n"
        "  ee.vldbc.8     " VEC_Q_GBITS ", %1                              \n"
        "  addi.n         %1, %1, 1                                        \n"
        "  ee.andq        " VEC_Q_GBITS ", " VEC_Q_GBITS ", " VEC_Q_GLANE "\n"
        "  ee.vcmp.eq.s16 " VEC_Q_GSEL  ", " VEC_Q_GBITS ", " VEC_Q_GZERO "\n"
        "  ee.notq        " VEC_Q_GTMP  ", " VEC_Q_GSEL  "                 \n"
        "  ee.vld.128.ip  " VEC_Q_GDST  ", %0, 0                           \n"
        "  ee.andq        " VEC_Q_GDST  ", " VEC_Q_GDST  ", " VEC_Q_GSEL  "\n"
        "  ee.andq        " VEC_Q_GTMP  ", " VEC_Q_GFG   ", " VEC_Q_GTMP  "\n"
        "  ee.orq         " VEC_Q_GDST  ", " VEC_Q_GDST  ", " VEC_Q_GTMP  "\n"
        "  ee.vst.128.ip  " VEC_Q_GDST  ", %0, 16                          \n"
        "  addi.n         %2, %2, -1                                       \n"
        "  bnez           %2, 1b                                           \n"
        : "+a"(dst), "+a"(bits), "+a"(bytes) : : "memory");
}
