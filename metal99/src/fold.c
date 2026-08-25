/*
 * Content digest for the transmit ledger. Pure ISO C99, NO asm.
 *
 * Split out of vec.c so the HOST test harness compiles the exact same
 * implementation the device runs. It previously lived beside the EE.* inline
 * assembly, which meant tests/host could not build it and had to reimplement
 * it - and an instrument verified against a copy of itself is not verified at
 * all. tests/host/digest_test.c links this file directly.
 */
#include "vec.h"

/*
 * A POLYNOMIAL ROLLING HASH:  acc = acc * MULT + word.
 *
 * Deliberately the most ordinary construction available. This is the
 * instrument, and being obviously correct matters more than being clever -
 * five previous versions were clever and silently wrong:
 *
 *   1. sampled only the first and last byte  -> blind to the payload, and it
 *      reported PASS on a visibly broken display
 *   2. mixed per call                        -> granularity-dependent, so FIFO
 *      chunks and DMA bands disagreed on identical data
 *   3. XOR, then saturating ee.vadds.s32     -> cancelled to zero, then
 *      collapsed under saturation
 *   4. took a VECTOR count and folded len/16 -> silently discarded up to 15
 *      trailing bytes of any non-multiple-of-16 transfer
 *   5. sum of (word ^ (position * K))        -> see below
 *
 * WHY VERSION 5 WAS REPLACED. It carried a running global word index and
 * folded `acc += w[i] ^ (pos * K)`. That is additively separable: position and
 * value never actually interact, so for a sparse payload a set bit can move to
 * a different position without changing the sum. tests/host/digest_test.c
 * demonstrates it - a single 0x01 byte at offset 0 and at offset 32 produce
 * the identical digest 0x2A010AF9, because 8*K has its low bit clear and
 * XOR-with-1 is then the same as ADD-1, while sum(i*K) is unchanged. A
 * transport that displaced a word would have gone undetected.
 *
 * The rolling hash fixes that structurally. Position is not a separate counter
 * at all - it is the number of multiplications a word has been through - which
 * also deletes the global index whose misuse caused version 2 (and the comment
 * in the old code noting that mistake "was made twice").
 *
 * WHAT IT CATCHES, given a positionally unique pattern: any wrong value,
 * omission, truncation, duplication, displacement, AND reordering. The
 * previous version was commutative and documented reordering as a known blind
 * spot; this one is not commutative, so that blind spot is gone.
 *
 * GROUPING INDEPENDENCE is structural, not a property to be maintained by
 * hand: it is a left fold over one byte sequence, so it cannot depend on how
 * that sequence was sliced into transfers. A per-row reference, 64-byte FIFO
 * chunks and whole DMA bands necessarily agree. digest_test.c asserts it.
 *
 * WHAT IT STILL MISSES: it is not a cryptographic hash and makes no claim to
 * be. Adversarial collisions are constructible; transport faults are not
 * adversarial.
 *
 * SEED must be non-zero. With acc starting at 0 a payload of leading zeros
 * leaves acc at 0, so an all-zero transfer would digest to 0 and a dropped one
 * would be invisible. Seeded, an N-word run of zeros digests to SEED*MULT^N,
 * which is distinct for every N that fits in this device's memory.
 */
#define FOLD_SEED 0x811C9DC5u      /* FNV-1a offset basis; any non-zero works */
#define FOLD_MULT 2654435761u      /* golden ratio, odd => invertible mod 2^32 */

static uint32_t g_fold = FOLD_SEED;

void vec_fold_reset(void) { g_fold = FOLD_SEED; }
uint32_t vec_fold_get(void) { return g_fold; }

void vec_fold(const void *p, uint32_t bytes)
{
    /*
     * VERIFICATION CODE, NOT THE RENDER PATH.
     *
     * The no-scalar rule governs code that runs on the render path. This is
     * reached only when the digest is armed - see spi2_ledger_digest_enable()
     * - which is the self-test, not steady state. Same reasoning as
     * tests/host/vec_host.c.
     *
     * All callers pass 16-byte-aligned buffers, so the word loads are aligned.
     */
    const uint32_t *w = (const uint32_t *)p;
    uint32_t n = bytes / 4u;
    uint32_t i, acc = g_fold;

    for (i = 0u; i < n; i++) acc = acc * FOLD_MULT + w[i];

    /* Byte tail. Every length this project transmits is a whole number of
     * words (a 736-byte row, 64- and 32-byte FIFO chunks), so this does not
     * run today. It exists so a future odd length is folded rather than
     * silently dropped, which is exactly how version 4 above went blind.
     * STATED LIMIT: a transfer ENDING mid-word zero-pads its tail into a word
     * of its own, so the digest would then depend on where the transfers were
     * cut. Word-aligned transfer lengths keep it grouping-independent. */
    if ((bytes & 3u) != 0u) {
        const uint8_t *b = (const uint8_t *)p + (n * 4u);
        uint32_t rem = bytes & 3u, t = 0u, k;
        for (k = 0u; k < rem; k++) t |= (uint32_t)b[k] << (k * 8u);
        acc = acc * FOLD_MULT + t;
    }

    g_fold = acc;
}
