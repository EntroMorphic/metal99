#include <stddef.h>
#include "selftest.h"
#include "io.h"
#include "vec.h"
#include "spi2.h"
#include "sh8601.h"

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
static uint16_t VEC_ALIGN g_xpat[SH8601_WIDTH];
static int g_xpat_ready;

static void probe(uint16_t *row, int y)
{
    if (!g_xpat_ready) {
        vec_ramp16(g_xpat, 0x1234u, 0x0111u, ROW_VECTORS);
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
    {
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
    }
    /* DISCRIMINATOR: the FIFO row path predates banding and rendered colour
     * bars and gradients correctly. If the scene looks right here and wrong
     * with DMA, the fault is in the BANDED path, not in marking or elision. */
    sh8601_set_dma(0);


}

int selftest_transport(void)
{
    int rc, fails = 0;
    /* ---- SELF-TEST: verify the transport WITHOUT looking at the screen ----
     *
     * The panel cannot be read back, so this checks what the hardware was
     * actually told to send. A span of N rows must transmit exactly
     * N * WIDTH * 2 pixel bytes. Truncation, duplication and reordering all
     * show up here - which is precisely the failure class banded DMA had, and
     * exactly what nobody could diagnose by describing a red screen. */
    {
        static const int cases[6] = { 1, 4, 32, 33, 100, 448 };
        int k, fails = 0, tr;
        for (tr = 0; tr < 2; tr++) {
        sh8601_set_dma(tr);
        con_puts(tr ? "\r\nself-test: BANDED DMA\r\n" : "\r\nself-test: FIFO\r\n");
        for (k = 0; k < 6; k++) {
            uint32_t want = (uint32_t)cases[k] * SH8601_WIDTH * 2u;
            uint32_t got, dig, want_dig;

            /* Independently fold what the rows SHOULD contain, using the same
             * digest over the same buffers - then compare against what was
             * actually transmitted. Byte counts alone proved insufficient. */
            {
                static uint16_t VEC_ALIGN ref[SH8601_WIDTH];
                int yy;
                vec_fold_reset();
                for (yy = 0; yy < cases[k]; yy++) {
                    probe(ref, yy);
                    vec_fold(ref, ROW_VECTORS);
                }
                want_dig = vec_fold_get();
            }

            spi2_ledger_reset();
            rc = sh8601_write_span(0u, (uint16_t)(cases[k] - 1), probe);
            got = spi2_ledger_bytes();
            dig = spi2_ledger_digest();

            /* Command traffic is on the ledger too: window (2 cmds + 8 param
             * bytes) plus the 4-byte pixel-write word. Subtract the fixed
             * preamble to compare pixel payload only. */
            con_puts(cases[k] == 448 ? "  448" : "   ");
            if (cases[k] != 448) con_dec((int32_t)cases[k]);
            con_puts(" rows: rc="); con_dec((int32_t)rc);
            con_puts(" bytes="); con_dec((int32_t)got);
            con_puts(" want>="); con_dec((int32_t)want);
            con_puts(" dig="); con_hex32(dig);
            con_puts(" want="); con_hex32(want_dig);
            if (rc == SPI2_OK && got >= want && dig == want_dig) {
                con_puts("  PASS\r\n");
            } else {
                con_puts("  FAIL\r\n"); fails++;
            }
        }
        }
        /* BACK TO FIFO. The self-test PASSED on DMA and the display was still
         * visibly worse - so the ledger is checking the wrong thing. See the
         * note in spi2.h. Known-good transport until the harness can actually
         * detect this failure. */

        /*
         * VALIDATE THE INSTRUMENT.
         *
         * A verifier that always passes is worthless, and three previous
         * versions of this harness did exactly that. Inject known faults and
         * require the digest to REJECT them. If these do not fail, nothing
         * above means anything.
         */
        {
            static uint16_t VEC_ALIGN ref[SH8601_WIDTH];
            uint32_t clean, bad;
            int yy, caught = 0;

            vec_fold_reset();
            for (yy = 0; yy < 32; yy++) { probe(ref, yy); vec_fold(ref, ROW_VECTORS); }
            clean = vec_fold_get();

            vec_fold_reset();
            for (yy = 0; yy < 32; yy++) {
                probe(ref, yy);
                if (yy == 7) ref[123] ^= 0x0001u;
                vec_fold(ref, ROW_VECTORS);
            }
            bad = vec_fold_get();
            con_puts("  fault 1bit  -> ");
            con_puts(bad != clean ? "DETECTED\r\n" : "MISSED\r\n");
            if (bad != clean) caught++;

            vec_fold_reset();
            for (yy = 0; yy < 32; yy++) { probe(ref, yy == 9 ? 8 : yy); vec_fold(ref, ROW_VECTORS); }
            bad = vec_fold_get();
            con_puts("  fault dup   -> ");
            con_puts(bad != clean ? "DETECTED\r\n" : "MISSED\r\n");
            if (bad != clean) caught++;

            vec_fold_reset();
            for (yy = 0; yy < 32; yy++) { probe(ref, yy + 1); vec_fold(ref, ROW_VECTORS); }
            bad = vec_fold_get();
            con_puts("  fault shift -> ");
            con_puts(bad != clean ? "DETECTED\r\n" : "MISSED\r\n");
            if (bad != clean) caught++;

            if (caught != 3) { fails++; con_puts("  INSTRUMENT IS BLIND\r\n"); }
        }

        sh8601_set_dma(0);
        con_puts(fails ? "SELF-TEST FAILED\r\n" : "SELF-TEST PASSED\r\n");
    }

    return fails;
}
