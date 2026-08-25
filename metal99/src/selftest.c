#include <stddef.h>
#include "selftest.h"
#include "io.h"
#include "vec.h"
#include "spi2.h"
#include "sh8601.h"

#define ROW_BYTES (SH8601_WIDTH * 2)
#define ROW_VECTORS (SH8601_WIDTH * 2 / VEC_BYTES)

/*
 * PROBE PATTERN - positionally unique in BOTH axes.
 *
 * pixel(x,y) = f(y) XOR g(x), where g is a per-column ramp built once. Every
 * pixel differs from every other, so any shift, duplication, omission or wrong
 * value changes the digest.
 *
 * The previous test scene was solid-colour rows - the worst possible input for
 * a digest, and the reason a broken transport passed. A verifier is only as
 * good as the pattern it is fed.
 */
/*
 * RAMP PARAMETERS, AND WHY THEY ARE NOT ARBITRARY.
 *
 * vec_ramp16 accumulates with ee.vadds.s16 - a SIGNED SATURATING add. The
 * original 0x0111 step overflowed int16 at lane 103 and pinned the remaining
 * 265 of 368 columns to 0x7FFF, so the pattern this file describes as
 * "positionally unique in BOTH axes" was in fact unique across 104 columns and
 * flat over the other 72% of every row.
 *
 * That is the same trap fold.c records as digest version 3, "collapsed under
 * saturation" - hit twice, in two files, with the same instruction.
 *
 * The static assertion below makes it structural rather than remembered: the
 * build fails if the ramp can reach int16 saturation. (C99: _Static_assert is
 * C11, so the negative-array-size idiom, per CONTRIBUTING.md.)
 */
typedef char probe_ramp_must_not_saturate[
    (PROBE_START + (SH8601_WIDTH - 1) * PROBE_STEP) <= 32767 ? 1 : -1];

static uint16_t VEC_ALIGN g_xpat[SH8601_WIDTH];
static int g_xpat_ready;

static void probe(uint16_t *row, int y)
{
    if (!g_xpat_ready) {
        vec_ramp16(g_xpat, PROBE_START, PROBE_STEP, ROW_VECTORS);
        g_xpat_ready = 1;
    }
    /* Golden-ratio multiplier: scrambles bits so consecutive rows do not pair
     * up. The previous y*0x0101 pattern XORed to zero over even row counts. */
    vec_fill16(row, (uint16_t)(0xC0DEu ^ (uint16_t)(y * 0x9E37u)), ROW_VECTORS);
    vec_xor16(row, g_xpat, ROW_VECTORS);
}

static void white(uint16_t *row, int y)
{
    (void)y;
    vec_fill16(row, sh8601_rgb565(255, 255, 255), ROW_VECTORS);
}


void selftest_liveness(void)
{
    /*
     * IS THE PANEL ALIVE? A boot-time check that does NOT depend on pixels.
     *
     * A dark screen is ambiguous: wedged panel, or correct code drawing black?
     * Filling white and pulsing brightness answers it in two seconds without
     * involving the elision, banding or DMA paths - only the command path that
     * first contact proved. If this does not flash, nothing downstream matters.
     */
    int k;
    con_puts("panel liveness: expect 3 white flashes\r\n");
    for (k = 0; k < 3; k++) {
        (void)sh8601_brightness(0xFFu);
        (void)sh8601_write_frame(white);
        delay_ms(250u);
        (void)sh8601_brightness(0x00u);
        delay_ms(250u);
    }
    (void)sh8601_brightness(0xFFu);

    /* DISCRIMINATOR: the FIFO row path predates banding and rendered colour
     * bars and gradients correctly. If the scene looks right here and wrong
     * with DMA, the fault is in the BANDED path, not in marking or elision. */
    sh8601_set_dma(0);
}

/*
 * Fold what `rows` rows of the probe pattern SHOULD contain, using the same
 * digest over the same buffers as the transmit ledger. Returns the reference.
 *
 * `corrupt_row` / `corrupt_idx`: inject a single-bit fault, or -1 for none.
 * `dup_row`: render this row's content twice, or -1 for none.
 * `shift`: added to every row index, so 1 shifts the whole pattern by a row.
 */
static uint32_t reference(int rows, int corrupt_row, int corrupt_idx,
                          int dup_row, int shift)
{
    static uint16_t VEC_ALIGN ref[SH8601_WIDTH];
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

/*
 * Fold the sub-range [x0,x1] of `rows` rows of the probe pattern.
 *
 * The sub-width transport had NO ledger coverage: every self-test case called
 * the full-width wrapper, so the path that sets a narrow address window and
 * streams &row[x0] was exercised only by looking at the panel. That is exactly
 * the "described by a human" loop this harness exists to replace.
 */
static uint32_t reference_x(int rows, int x0, int x1)
{
    static uint16_t VEC_ALIGN ref[SH8601_WIDTH];
    int y;
    vec_fold_reset();
    for (y = 0; y < rows; y++) {
        probe(ref, y);
        vec_fold(&ref[x0], (uint32_t)(x1 - x0 + 1) * 2u);
    }
    return vec_fold_get();
}

/*
 * The glyph blit's ASSEMBLY, against a scalar reference, on the device.
 *
 * tests/host exercises vec_glyph_row's scalar MIRROR in vec_host.c, not the
 * EE.* version the firmware actually runs - so until now the real blit was
 * verified by looking at the panel, which is the loop this harness exists to
 * replace. Text that is subtly wrong still looks like text.
 *
 * Patterns chosen to separate the failure modes: 0xA5 alternates (bit order),
 * 0x00 must leave the destination untouched (transparency), 0xFF must replace
 * all eight (mask polarity), 0x3C is neither end (no edge-only luck).
 */
static int selftest_glyph(void)
{
    static uint16_t VEC_ALIGN got[32];
    static uint16_t VEC_ALIGN want[32];
    static const uint8_t VEC_ALIGN pat[16] = { 0xA5u, 0x00u, 0xFFu, 0x3Cu };
    const uint16_t bg = 0x1234u, fg = 0xBEEFu;
    int i, b, bad = 0;

    for (i = 0; i < 32; i++) { got[i] = bg; want[i] = bg; }
    for (b = 0; b < 4; b++)
        for (i = 0; i < 8; i++)
            if ((pat[b] & (0x80u >> i)) != 0u) want[b * 8 + i] = fg;

    vec_glyph_row(got, pat, 4u, fg);

    for (i = 0; i < 32; i++) if (got[i] != want[i]) bad++;
    con_puts("\r\nself-test: GLYPH BLIT\r\n  32 px vs scalar reference: ");
    if (bad == 0) { con_puts("PASS\r\n"); return 0; }
    con_puts("FAIL, "); con_dec((int32_t)bad); con_puts(" px differ\r\n");
    return 1;
}

int selftest_transport(void)
{
    /*
     * ONE failure counter for the whole function.
     *
     * There used to be two: an outer `fails` and an inner one declared inside
     * the test block, shadowing it. Every fails++ hit the inner variable, the
     * inner variable went out of scope, and `return fails` handed back the
     * outer one - which nothing ever touched. The compiler folded the whole
     * function down to `movi.n a2, 0 ; retw.n`. It could not report a failure
     * even when it printed one, so main.c announced SELF-TEST PASSED
     * unconditionally for every build.
     *
     * That is the fourth time this harness has been a verifier that always
     * passes, which is why build.sh now compiles with -Wshadow. Do not
     * reintroduce a nested scope that redeclares this.
     */
    int fails = 0;
    int rc, k, tr;
    static const int cases[6] = { 1, 4, 32, 33, 100, 448 };

    /* ---- verify the transport WITHOUT looking at the screen ----
     *
     * The panel cannot be read back, so this checks what the hardware was
     * actually told to send. A span of N rows must transmit exactly
     * N * WIDTH * 2 pixel bytes. Truncation, duplication and reordering all
     * show up here - which is precisely the failure class banded DMA had, and
     * exactly what nobody could diagnose by describing a red screen. */

    /* The digest is O(n) per byte and off in steady state - arm it here, and
     * be sure to disarm on EVERY exit or the render loop pays ~12% forever. */
    spi2_ledger_digest_enable(1);

    for (tr = 0; tr < 2; tr++) {
        sh8601_set_dma(tr);
        con_puts(tr ? "\r\nself-test: BANDED DMA\r\n" : "\r\nself-test: FIFO\r\n");

        for (k = 0; k < 6; k++) {
            uint32_t want = (uint32_t)cases[k] * (uint32_t)ROW_BYTES;
            uint32_t got_px, got_all, dig, want_dig;

            /* Independently fold what the rows SHOULD contain, then compare
             * against what was actually transmitted. Byte counts alone proved
             * insufficient. */
            want_dig = reference(cases[k], -1, -1, -1, 0);

            spi2_ledger_reset();
            rc      = sh8601_write_span(0u, (uint16_t)(cases[k] - 1), probe);
            got_px  = spi2_ledger_pixel_bytes();
            got_all = spi2_ledger_bytes();
            dig     = spi2_ledger_digest();

            /* EXACT, not >=. This used to compare `got >= want` against the
             * total byte count, with a comment promising to subtract the
             * command preamble - which the code never did. A >= test cannot
             * detect over-transmission, and over-transmission (a band sent
             * twice into a CS-held stream) is the documented banded-DMA
             * failure. The ledger now separates pixel payload from preamble,
             * so this is an equality with no magic constant. */
            con_puts("  ");
            con_dec((int32_t)cases[k]);
            con_puts(" rows: rc=");   con_dec((int32_t)rc);
            con_puts(" px=");         con_dec((int32_t)got_px);
            con_puts("/");            con_dec((int32_t)want);
            con_puts(" pre=");        con_dec((int32_t)(got_all - got_px));
            con_puts(" dig=");        con_hex32(dig);
            con_puts("/");            con_hex32(want_dig);
            if (rc == SPI2_OK && got_px == want && dig == want_dig) {
                con_puts("  PASS\r\n");
            } else {
                con_puts("  FAIL\r\n"); fails++;
            }
        }
    }

    /*
     * SUB-WIDTH SPANS. Same ledger, narrower window.
     *
     * A rectangle must transmit exactly rows * cols * 2 pixel bytes and digest
     * to a reference folded over the same sub-range. Sub-width always takes the
     * FIFO path (a partial band is not contiguous), so this also proves the
     * fallback out of the banded path is clean.
     */
    {
        static const int xcase[3][4] = {   /* x0, x1, rows, unused */
            { 136, 223,  32, 0 },          /* a centred 88px element   */
            {   0,   7, 100, 0 },          /* the first column cell    */
            { 360, 367,  64, 0 }           /* the last column cell     */
        };
        int c;
        con_puts("\r\nself-test: SUB-WIDTH spans\r\n");
        for (c = 0; c < 3; c++) {
            int xa = xcase[c][0], xb = xcase[c][1], rows = xcase[c][2];
            uint32_t want = (uint32_t)rows * (uint32_t)(xb - xa + 1) * 2u;
            uint32_t got_px, dig, want_dig;

            want_dig = reference_x(rows, xa, xb);
            spi2_ledger_reset();
            rc = sh8601_write_span_x((uint16_t)xa, 0u, (uint16_t)xb,
                                     (uint16_t)(rows - 1), probe);
            got_px = spi2_ledger_pixel_bytes();
            dig    = spi2_ledger_digest();

            con_puts("  x="); con_dec((int32_t)xa);
            con_puts(".."); con_dec((int32_t)xb);
            con_puts(" rows="); con_dec((int32_t)rows);
            con_puts(": rc="); con_dec((int32_t)rc);
            con_puts(" px="); con_dec((int32_t)got_px);
            con_puts("/"); con_dec((int32_t)want);
            con_puts(" dig="); con_hex32(dig);
            con_puts("/"); con_hex32(want_dig);
            if (rc == SPI2_OK && got_px == want && dig == want_dig) {
                con_puts("  PASS\r\n");
            } else {
                con_puts("  FAIL\r\n"); fails++;
            }
        }
    }

    fails += selftest_glyph();

    /*
     * VALIDATE THE INSTRUMENT.
     *
     * A verifier that always passes is worthless, and four previous versions
     * of this harness did exactly that. Inject known faults and require the
     * digest to REJECT them. If these do not fail, nothing above means
     * anything.
     */
    {
        uint32_t clean = reference(32, -1, -1, -1, 0);
        int caught = 0;

        if (reference(32,  7, 123, -1, 0) != clean) { caught++; con_puts("  fault 1bit  -> DETECTED\r\n"); }
        else                                          con_puts("  fault 1bit  -> MISSED\r\n");

        if (reference(32, -1,  -1,  8, 0) != clean) { caught++; con_puts("  fault dup   -> DETECTED\r\n"); }
        else                                          con_puts("  fault dup   -> MISSED\r\n");

        if (reference(32, -1,  -1, -1, 1) != clean) { caught++; con_puts("  fault shift -> DETECTED\r\n"); }
        else                                          con_puts("  fault shift -> MISSED\r\n");

        if (caught != 3) { fails++; con_puts("  INSTRUMENT IS BLIND\r\n"); }
    }

    /*
     * BACK TO FIFO, and the digest back off.
     *
     * Stated plainly because it is the most important caveat in this file: the
     * self-test PASSES on banded DMA and the display is still visibly worse.
     * So the ledger proves the right BYTES were handed to the peripheral, and
     * proves nothing about when they arrive on the wire. Delivery timing is
     * outside what this instrument can see. FIFO is the known-good transport
     * and ships; see docs/DESIGN.md 6.6l.
     */
    sh8601_set_dma(0);
    spi2_ledger_digest_enable(0);

    con_puts(fails ? "SELF-TEST FAILED\r\n" : "SELF-TEST PASSED\r\n");
    return fails;
}
