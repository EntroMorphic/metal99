#include "vec.h"

void vec_fill16(uint16_t *dst, uint16_t value, uint32_t vectors)
{
    /* EE.VLDBC.16 broadcasts one halfword to all eight lanes in a single
     * instruction - no scalar seed loop. */
    uint16_t v = value;
    __asm__ __volatile__ ("ee.vldbc.16 q0, %0" : : "a"(&v));

    __asm__ __volatile__ (
        "1:                             \n"
        "  ee.vst.128.ip q0, %0, 16     \n"   /* store 8 px, post-inc by 16  */
        "  addi.n        %1, %1, -1     \n"
        "  bnez          %1, 1b         \n"
        : "+a"(dst), "+a"(vectors) : : "memory");
}

void vec_copy(void *dst, const void *src, uint32_t vectors)
{
    __asm__ __volatile__ (
        "1:                             \n"
        "  ee.vld.128.ip q1, %1, 16     \n"
        "  ee.vst.128.ip q1, %0, 16     \n"
        "  addi.n        %2, %2, -1     \n"
        "  bnez          %2, 1b         \n"
        : "+a"(dst), "+a"(src), "+a"(vectors) : : "memory");
}

void vec_zero(void *dst, uint32_t vectors)
{
    __asm__ __volatile__ (
        "  ee.zero.q     q2             \n"
        "1:                             \n"
        "  ee.vst.128.ip q2, %0, 16     \n"
        "  addi.n        %1, %1, -1     \n"
        "  bnez          %1, 1b         \n"
        : "+a"(dst), "+a"(vectors) : : "memory");
}
