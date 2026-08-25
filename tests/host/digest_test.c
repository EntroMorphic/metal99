/*
 * Assertions for the transmit ledger's content digest.
 *
 * WHY THIS EXISTS. tests/host/ used to contain no assertions at all - it
 * rendered a PPM for a human to look at. Meanwhile the digest is the only
 * thing standing between "the panel cannot be read back" and "we have no idea
 * whether the right bytes went out", and every previous version of it was
 * silently wrong in a way that reported PASS. The on-device fault injection
 * catches some of that, but it only runs when a board is attached, and the
 * self-test that reported its result was itself broken for the whole of this
 * project's history.
 *
 * This links metal99/src/fold.c - the exact implementation the firmware runs.
 *
 * Run: make -C tests/host test
 */
#include <stdio.h>
#include <string.h>
#include "vec.h"
#include "selftest.h"      /* PROBE_START / PROBE_STEP - one definition only */

#define W           368
#define ROW_BYTES   (W * 2)
#define ROW_VECS    (ROW_BYTES / VEC_BYTES)

static int g_fails;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_fails++;
}

/* The self-test's probe pattern: positionally unique in both axes. */
static void probe(uint16_t *row, int y)
{
    static uint16_t VEC_ALIGN xpat[W];
    static int ready;
    uint32_t i;
    if (!ready) { vec_ramp16(xpat, PROBE_START, PROBE_STEP, ROW_VECS); ready = 1; }
    vec_fill16(row, (uint16_t)(0xC0DEu ^ (uint16_t)(y * 0x9E37u)), ROW_VECS);
    for (i = 0u; i < (uint32_t)W; i++) row[i] = (uint16_t)(row[i] ^ xpat[i]);
}

static uint32_t fold_rows(int rows, int corrupt_row, int corrupt_idx,
                          int dup_row, int shift)
{
    static uint16_t VEC_ALIGN ref[W];
    int y;
    vec_fold_reset();
    for (y = 0; y < rows; y++) {
        int src = (dup_row >= 0 && y == dup_row + 1) ? dup_row : y;
        probe(ref, src + shift);
        if (y == corrupt_row && corrupt_idx >= 0) ref[corrupt_idx] ^= 0x0001u;
        vec_fold(ref, ROW_BYTES);
    }
    return vec_fold_get();
}

/* Fold `bytes` from `p` in slices of `chunk`, the way a transport would. */
static uint32_t fold_chunked(const void *p, uint32_t bytes, uint32_t chunk)
{
    const unsigned char *b = p;
    uint32_t off = 0u;
    vec_fold_reset();
    while (off < bytes) {
        uint32_t n = (bytes - off < chunk) ? (bytes - off) : chunk;
        vec_fold(b + off, n);
        off += n;
    }
    return vec_fold_get();
}

int main(void)
{
    static uint16_t VEC_ALIGN buf[W * 4];
    uint32_t clean, a, b, c;
    int i;

    printf("digest_test: metal99 transmit-ledger content digest\n");

    /* ---- 0. the probe pattern is actually what it claims to be ----
     *
     * vec_ramp16 accumulates with a SIGNED SATURATING add. With the original
     * step of 0x0111 the ramp pinned at 0x7FFF from lane 103 onward, leaving
     * 265 of 368 columns identical - so "positionally unique in BOTH axes" was
     * false across 72% of every row, in the one pattern the whole harness is
     * fed. selftest.c now carries a static assertion; this is the runtime half,
     * and it fails loudly rather than quietly degrading the pattern. */
    {
        static uint16_t VEC_ALIGN ramp[W];
        int uniq = 0, j;
        vec_ramp16(ramp, PROBE_START, PROBE_STEP, ROW_VECS);
        for (i = 0; i < W; i++) {
            for (j = 0; j < i; j++) if (ramp[j] == ramp[i]) break;
            if (j == i) uniq++;
        }
        printf("  %-58s %s\n", "probe ramp is unique across all 368 columns",
               uniq == W ? "PASS" : "FAIL");
        if (uniq != W) {
            g_fails++;
            printf("      only %d of %d distinct - ramp saturated\n", uniq, W);
        }
        check(ramp[W - 1] != 0x7FFFu, "ramp did not pin at int16 saturation");
    }

    /* ---- 1. fault injection: the digest must REJECT known corruption ---- */
    clean = fold_rows(32, -1, -1, -1, 0);
    check(fold_rows(32,  7, 123, -1, 0) != clean, "single-bit flip changes the digest");
    check(fold_rows(32, -1,  -1,  8, 0) != clean, "duplicated row changes the digest");
    check(fold_rows(32, -1,  -1, -1, 1) != clean, "one-row shift changes the digest");
    check(fold_rows(31, -1,  -1, -1, 0) != clean, "truncated span changes the digest");

    /* Reordering. The previous digest was commutative and documented this as a
     * known blind spot ("DMA reads descriptors sequentially, so that is not a
     * failure mode here"). The rolling hash is not commutative, so the blind
     * spot is gone and the claim can be asserted rather than argued. */
    {
        static uint16_t VEC_ALIGN r[W];
        uint16_t t;
        probe(r, 3);
        vec_fold_reset(); vec_fold(r, ROW_BYTES); a = vec_fold_get();
        t = r[0]; r[0] = r[W - 1]; r[W - 1] = t;      /* swap two words */
        vec_fold_reset(); vec_fold(r, ROW_BYTES); b = vec_fold_get();
        check(a != b, "reordering within a transfer changes the digest");
    }

    /* ---- 2. GROUPING INDEPENDENCE ----
     *
     * The load-bearing property, and the one never previously tested. The
     * self-test compares a reference folded ROW AT A TIME against bytes the
     * FIFO transport folded in 64-byte chunks and the DMA transport folded in
     * whole 2944-byte bands. If the digest depended on grouping, those three
     * would disagree on identical data and every comparison would be noise.
     * Two earlier versions did exactly that (per-call mixing, per-call
     * position index) and the bug was found on hardware both times. */
    for (i = 0; i < 4; i++) probe(&buf[i * W], i);
    a = fold_chunked(buf, 4u * ROW_BYTES, 4u * ROW_BYTES);  /* one band      */
    b = fold_chunked(buf, 4u * ROW_BYTES, ROW_BYTES);       /* row at a time */
    c = fold_chunked(buf, 4u * ROW_BYTES, 64u);             /* FIFO chunks   */
    check(a == b, "band-at-once == row-at-a-time");
    check(b == c, "row-at-a-time == 64-byte FIFO chunks");

    /* 736 bytes is 11*64 + 32, so the final chunk is a different size than the
     * rest - exactly the shape the FIFO path produces for a real row. */
    check(fold_chunked(buf, ROW_BYTES, 64u) == fold_chunked(buf, ROW_BYTES, ROW_BYTES),
          "uneven final FIFO chunk folds identically");

    /* ---- 3. the byte tail is NOT silently discarded ----
     *
     * Regression test. vec_fold() took a VECTOR count and folded
     * `len / VEC_BYTES`, so up to 15 trailing bytes of any transfer whose
     * length was not a multiple of 16 were never looked at. It now takes
     * BYTES. Corrupting a byte inside that old blind spot must be visible. */
    memset(buf, 0xA5, sizeof buf);
    vec_fold_reset(); vec_fold(buf, 20u); a = vec_fold_get();
    ((unsigned char *)buf)[17] ^= 0x01u;          /* byte 17 of 20: was blind */
    vec_fold_reset(); vec_fold(buf, 20u); b = vec_fold_get();
    check(a != b, "corruption in a sub-vector tail is detected");

    ((unsigned char *)buf)[17] ^= 0x01u;          /* restore */
    vec_fold_reset(); vec_fold(buf, 16u); a = vec_fold_get();
    vec_fold_reset(); vec_fold(buf, 20u); b = vec_fold_get();
    check(a != b, "16 bytes and 20 bytes do not digest alike");

    /* ---- 4. position weighting ---- */
    memset(buf, 0, sizeof buf);
    ((unsigned char *)buf)[0] = 0x01u;
    vec_fold_reset(); vec_fold(buf, 64u); a = vec_fold_get();
    ((unsigned char *)buf)[0] = 0x00u;
    ((unsigned char *)buf)[32] = 0x01u;
    vec_fold_reset(); vec_fold(buf, 64u); b = vec_fold_get();
    check(a != b, "same byte at a different offset digests differently");

    /* A digest that ignores an all-zero payload would be blind to a dropped
     * transfer of zeros; the position weight is what prevents that. */
    memset(buf, 0, sizeof buf);
    vec_fold_reset(); vec_fold(buf, 64u);  a = vec_fold_get();
    vec_fold_reset(); vec_fold(buf, 128u); b = vec_fold_get();
    check(a != 0u,  "all-zero payload still produces a non-zero digest");
    check(a != b,   "length change alone is visible on zero payload");

    /* ---- 5. reset clears the position, not just the accumulator ---- */
    vec_fold_reset(); vec_fold(buf, 64u); a = vec_fold_get();
    vec_fold_reset(); vec_fold(buf, 64u); b = vec_fold_get();
    check(a == b, "vec_fold_reset() makes folding reproducible");

    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails != 0;
}
