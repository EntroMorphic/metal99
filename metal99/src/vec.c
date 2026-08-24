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

void vec_ramp16(uint16_t *dst, uint16_t start, uint16_t step, uint32_t vectors)
{
    /* Seed q4 with [start, start+step, ... start+7*step] by inserting four
     * 32-bit words, each holding two 16-bit lanes. O(1), not per element. */
    uint32_t w0 = (uint32_t)start | ((uint32_t)(uint16_t)(start + step) << 16);
    uint32_t w1 = (uint32_t)(uint16_t)(start + 2*step) | ((uint32_t)(uint16_t)(start + 3*step) << 16);
    uint32_t w2 = (uint32_t)(uint16_t)(start + 4*step) | ((uint32_t)(uint16_t)(start + 5*step) << 16);
    uint32_t w3 = (uint32_t)(uint16_t)(start + 6*step) | ((uint32_t)(uint16_t)(start + 7*step) << 16);
    uint16_t bump = (uint16_t)(step * 8);

    __asm__ __volatile__ (
        "ee.movi.32.q q4, %0, 0 \n"
        "ee.movi.32.q q4, %1, 1 \n"
        "ee.movi.32.q q4, %2, 2 \n"
        "ee.movi.32.q q4, %3, 3 \n"
        : : "a"(w0), "a"(w1), "a"(w2), "a"(w3));
    __asm__ __volatile__ ("ee.vldbc.16 q5, %0" : : "a"(&bump));

    __asm__ __volatile__ (
        "1:                            \n"
        "  ee.vst.128.ip q4, %0, 16    \n"
        "  ee.vadds.s16  q4, q4, q5    \n"
        "  addi.n        %1, %1, -1    \n"
        "  bnez          %1, 1b        \n"
        : "+a"(dst), "+a"(vectors) : : "memory");
}

void vec_xor16(uint16_t *dst, const uint16_t *src, uint32_t vectors)
{
    uint16_t *d = dst;
    __asm__ __volatile__ (
        "1:                            \n"
        "  ee.vld.128.ip q6, %0, 16    \n"   /* dst chunk */
        "  ee.vld.128.ip q7, %1, 16    \n"   /* src chunk */
        "  ee.xorq       q6, q6, q7    \n"
        "  addi          %0, %0, -16   \n"   /* rewind to store in place */
        "  ee.vst.128.ip q6, %0, 16    \n"
        "  addi.n        %2, %2, -1    \n"
        "  bnez          %2, 1b        \n"
        : "+a"(d), "+a"(src), "+a"(vectors) : : "memory");
}

/* ---- content digest ---- */
static uint32_t g_fold;
static uint32_t g_fold_pos;      /* GLOBAL word index, not per call */

void vec_fold_reset(void) { g_fold = 0u; g_fold_pos = 0u; }
uint32_t vec_fold_get(void) { return g_fold; }

void vec_fold(const void *p, uint32_t vectors)
{
    /*
     * VERIFICATION CODE, NOT THE RENDER PATH.
     *
     * Deliberately a plain wrapping 32-bit sum. It is obviously correct, which
     * matters more here than being fast: this is the instrument, and three
     * cleverer versions were all silently wrong -
     *
     *   1. sampled only the first and last byte  -> blind to the payload,
     *      reported PASS on a visibly broken display
     *   2. mixed per call                        -> granularity-dependent, so
     *      FIFO chunks and DMA bands disagreed on identical data
     *   3. XOR, then saturating ee.vadds.s32     -> cancelled to zero, then
     *      collapsed under saturation
     *
     * The no-scalar rule governs code that runs on the render path. This runs
     * during the self-test only - the same reasoning as tests/host/vec_host.c.
     * Wrapping addition is associative and commutative, so the result does not
     * depend on how bytes are grouped into transfers, and nothing cancels.
     *
     * Known limit: commutative, so an exact reorder of words is invisible. DMA
     * reads descriptors sequentially, so that is not a failure mode here.
     */
    const uint32_t *w = (const uint32_t *)p;
    uint32_t n = vectors * (uint32_t)(VEC_BYTES / 4);
    uint32_t i, acc = g_fold, pos = g_fold_pos;

    /* The position weight MUST use a running global index. Using a per-call
     * index makes the digest depend on how bytes were grouped into transfers,
     * so 64-byte FIFO chunks, whole DMA bands and a per-row reference all
     * disagree on identical data. That mistake was made twice. */
    for (i = 0u; i < n; i++) {
        acc += w[i] ^ (pos * 2654435761u);
        pos++;
    }
    g_fold = acc;
    g_fold_pos = pos;
}
