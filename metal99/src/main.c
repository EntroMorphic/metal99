#include <stddef.h>
#include "io.h"
#include "vec.h"
#include "clk.h"
#include "spi2.h"
#include "gdma.h"
#include "sh8601.h"
#include "elide.h"
#include "gfx.h"
#include "font.h"
#include "selftest.h"

#define ROW_VECTORS (SH8601_WIDTH * 2 / VEC_BYTES)

/* A representative interface: a static background with one moving element.
 * This is what real UIs look like - almost nothing changes between frames. */
/* 96 rows of 448 on a 1.8-inch panel is ~7mm. The first version used 24 rows
 * (~1.7mm), which at the top edge read as "nothing there" and cost a round of
 * misdiagnosis; this is big enough to be unambiguous. Every position and span
 * below is derived from BAR_H - the demo used to repeat 96 and 95 as literals,
 * so changing this constant silently broke the erase/draw pairing. */
#define BAR_H 96
#define BAR_TRAVEL (SH8601_HEIGHT - BAR_H)

/* 3 s stationary, then 20 s with the rolling resync disabled. See the note at
 * the pacing loop for why the second one is the load-bearing test. */
#define STATIONARY_FRAMES 180u
#define RESYNC_OFF_FRAMES 1200u

/* The A/B element: 88x88, away from every edge. Grid-aligned so nothing is
 * snapped outward and the comparison is exact. */
#define BOX_X 136u
#define BOX_W 88u
#define BOX_H 88u
#define BGCOL sh8601_rgb565(0, 20, 60)
#define FGCOL sh8601_rgb565(255, 70, 0)
/* The pacing loop's palette: an unlit background, so any pixel that should have
 * been erased and was not is unmistakable on an AMOLED. */
#define BLACKCOL sh8601_rgb565(0, 0, 0)
#define BARCOL   sh8601_rgb565(255, 60, 0)
static int g_bar_y;

static void scene(uint16_t *row, int y)
{
    /* Black everywhere except the bar. On an AMOLED black is unlit, so any
     * pixel that should have been erased and was not is unmistakable. */
    if ((y >= g_bar_y) && (y < g_bar_y + BAR_H)) {
        vec_fill16(row, sh8601_rgb565(255, 60, 0), ROW_VECTORS);
    } else {
        vec_fill16(row, sh8601_rgb565(0, 0, 0), ROW_VECTORS);
    }
}

/* Decimal into a caller's buffer. No libc, and con_dec writes to the console
 * rather than to memory, so this is the one place that needs it. */
static void u32str(char *b, uint32_t v, int width)
{
    int i;
    for (i = width - 1; i >= 0; i--) { b[i] = (char)('0' + (v % 10u)); v /= 10u; }
    b[width] = '\0';
}

static void put_ms(uint32_t cycles)
{
    uint32_t us = cycles / (CPU_HZ / 1000000u);
    con_dec((int32_t)(us / 1000u)); con_putc('.');
    con_dec((int32_t)((us % 1000u) / 100u)); con_puts("ms ");
}

void app_entry(void)
{
    int rc;

    con_puts("\r\n=== metal99 : elision ===\r\n");
    /* NOT ignorable. g_cpu_hz only updates on success, so a silent failure
     * leaves every delay computed for 20 MHz while the core runs at 160 -
     * including the SH8601's 120 ms sleep-out MINIMUM, which would become
     * 15 ms and make init marginal in a way that looks like a panel fault. */
    rc = clk_set_cpu_pll(160u);
    if (rc != CLK_OK) {
        con_puts("PLL switch FAILED rc="); con_dec((int32_t)rc);
        con_puts(" - staying at 20 MHz (delays remain correct)\r\n");
    }
    spi2_init();
    gdma_init();
    (void)spi2_set_clock(40u);
    rc = sh8601_init();
    con_puts("sh8601_init rc="); con_dec((int32_t)rc); con_puts("\r\n");

    selftest_liveness();
    /* Print the RETURN VALUE, not a second copy of selftest.c's own verdict.
     * The two used to be identical strings, which hid the fact that the return
     * value was a shadowed variable folded to a constant 0 - the console said
     * PASSED twice while the function was incapable of saying anything else.
     * Distinct now, so the two disagreeing is visible rather than invisible. */
    rc = selftest_transport();
    con_puts("selftest_transport rc="); con_dec((int32_t)rc);
    con_puts(rc == 0 ? " (0 failures)\r\n" : " <-- FAILURES\r\n");

    /*
     * ONE FULL REPAINT, TIMED, ON THE TRANSPORT THAT SHIPS.
     *
     * README's performance table mixes two transports: 0.037 ms/row is banded
     * DMA wire time, while the 104-row / 7.1 ms figure is FIFO. They disagree
     * by about 2x - 0.037 x 104 = 3.9 ms, not 7.1 - and "All 448 rows are
     * updatable at 60 Hz" is a claim about DMA, which is parked. The project's
     * own rule is to measure rather than extrapolate, so measure it.
     */
    {
        const sh8601_stats *s;
        uint32_t fps10;
        g_bar_y = SH8601_HEIGHT / 2 - BAR_H / 2;
        rc = sh8601_write_frame(scene);
        s  = sh8601_last_frame();
        fps10 = (s->total_cycles == 0u) ? 0u
              : (uint32_t)(((uint64_t)CPU_HZ * 10u) / s->total_cycles);
        con_puts("\r\nfull repaint, FIFO, 448 rows: rc=");
        con_dec((int32_t)rc);
        con_puts(" total="); put_ms(s->total_cycles);
        con_puts("render="); put_ms(s->render_cycles);
        con_puts("flush=");  put_ms(s->flush_cycles);
        con_puts("-> "); con_dec((int32_t)(fps10 / 10u)); con_putc('.');
        con_dec((int32_t)(fps10 % 10u)); con_puts(" fps full-frame\r\n");
    }

    /* ---------------- gfx: retained-mode messaging layer ---------------- */
    gfx_init();
    con_puts("\r\ngfx layer\r\n");

    {
        uint32_t ch;
        /* Background, once. */
        ch = gfx_solid(0u, SH8601_HEIGHT - 1u, BGCOL);
        con_puts("  paint background : "); con_dec((int32_t)ch); con_puts(" rows changed\r\n");
        (void)gfx_present();

        /* Same colour again - should be FULLY elided, zero rows. */
        ch = gfx_solid(0u, SH8601_HEIGHT - 1u, BGCOL);
        con_puts("  repaint same     : "); con_dec((int32_t)ch);
        con_puts(" rows changed (0 = elided)\r\n");
        (void)gfx_present();
        con_puts("  -> transmitted   : "); con_dec((int32_t)gfx_last()->rows_sent);
        con_puts(" rows\r\n");
    }

    /*
     * A/B: THE SAME MOTION, FULL-WIDTH vs SUB-WIDTH.
     *
     * Both move an element 4 px per frame for 60 frames and report the pixels
     * actually transmitted. The bar is full width by nature, so it is already
     * optimal and cannot improve - which is exactly why it hid this for so
     * long. The box is what a real interface is made of.
     */
    {
        uint32_t px_bar = 0u, px_box = 0u;
        int k, prev;

        con_puts("  A/B: identical motion, full-width vs sub-width\r\n");

        prev = -1;
        for (k = 0; k < 60; k++) {
            int by = (k * 4) % BAR_TRAVEL;
            if (prev >= 0) (void)gfx_solid((uint16_t)prev,
                                           (uint16_t)(prev + BAR_H - 1), BGCOL);
            (void)gfx_solid((uint16_t)by, (uint16_t)(by + BAR_H - 1), FGCOL);
            prev = by;
            if (gfx_present() != SPI2_OK) break;
            px_bar += gfx_last()->px_sent;
        }

        prev = -1;
        for (k = 0; k < 60; k++) {
            int by = (k * 4) % BAR_TRAVEL;
            if (prev >= 0) (void)gfx_rect(BOX_X, (uint16_t)prev,
                                          BOX_X + BOX_W - 1,
                                          (uint16_t)(prev + BOX_H - 1), BGCOL);
            (void)gfx_rect(BOX_X, (uint16_t)by, BOX_X + BOX_W - 1,
                           (uint16_t)(by + BOX_H - 1), FGCOL);
            prev = by;
            if (gfx_present() != SPI2_OK) break;
            px_box += gfx_last()->px_sent;
        }

        con_puts("   full-width bar  "); con_dec((int32_t)(px_bar / 60u));
        con_puts(" px/frame\r\n");
        con_puts("   sub-width box   "); con_dec((int32_t)(px_box / 60u));
        con_puts(" px/frame\r\n");
        /* Both are 60-frame sums, so the ratio is simply their quotient.
         * Computing it as px_bar/(px_box/60)/60 truncated twice and under-
         * reported. One decimal, since the ratio is not a whole number. */
        con_puts("   ratio           ");
        if (px_box > 0u) {
            uint32_t r10 = (px_bar * 10u) / px_box;
            con_dec((int32_t)(r10 / 10u)); con_putc('.');
            con_dec((int32_t)(r10 % 10u));
        } else { con_putc('0'); }
        con_puts("x fewer pixels for the same motion\r\n");

        /* Leave a centred box on screen: the shape the old two-kind row model
         * could not express at all. */
        (void)gfx_solid(0u, SH8601_HEIGHT - 1u, BGCOL);
        (void)gfx_rect(BOX_X, 180u, BOX_X + BOX_W - 1, 180u + BOX_H - 1, FGCOL);
        (void)gfx_present();
        con_puts("  gfx demo done - centred box is on the panel\r\n");
    }

    /* ---------------- text ---------------- */
    {
        char buf[12];
        uint32_t px_first = 0u, px_update = 0u;
        int k;

        con_puts("\r\ntext layer\r\n");
        (void)gfx_solid(0u, SH8601_HEIGHT - 1u, BGCOL);
        (void)gfx_present();

        /* Two sizes, both rasterised from the same TTF at build time. */
        (void)gfx_text(0, 16u,  40u, "metal99", FGCOL, &share_mono_16x32);
        (void)gfx_text(1, 16u,  88u, "Share Tech Mono, 8x16", FGCOL,
                       &share_mono_8x16);
        (void)gfx_text(2, 16u, 112u, "1bpp, blitted at 1.375 i/px", FGCOL,
                       &share_mono_8x16);
        (void)gfx_present();
        px_first = gfx_last()->px_sent;
        con_puts("  first paint      : "); con_dec((int32_t)px_first);
        con_puts(" px\r\n");

        /* Setting the SAME text again must cost nothing at all. */
        (void)gfx_text(0, 16u, 40u, "metal99", FGCOL, &share_mono_16x32);
        (void)gfx_present();
        con_puts("  identical text   : "); con_dec((int32_t)gfx_last()->px_sent);
        con_puts(" px (resync only)\r\n");

        /* A counter: only the label's own rectangle moves. */
        for (k = 0; k < 60; k++) {
            u32str(buf, (uint32_t)k, 5);
            (void)gfx_text(3, 16u, 160u, buf, sh8601_rgb565(120, 220, 255),
                           &share_mono_16x32);
            if (gfx_present() != SPI2_OK) break;
            px_update += gfx_last()->px_sent;
        }
        con_puts("  counter update   : "); con_dec((int32_t)(px_update / 60u));
        con_puts(" px/frame for a 5-digit field\r\n");
        con_puts("  full screen would be 164864 px\r\n");

        delay_ms(1500u);
    }

    con_puts("mode | rows spans | update | eff fps | headroom vs 16.67ms\r\n");

    /*
     * FRAME PACING TO 60 Hz.
     *
     * Unpaced, an elided update takes 1.1 ms - about 900 fps - and the bar
     * crossed the screen roughly eight times a second, which reads as a blur
     * or as nothing at all. Pacing is not a limitation here, it is the goal:
     * hold a steady 60 Hz and spend the remaining ~94% of each period on
     * whatever the interface actually wants to do.
     */
    g_bar_y = SH8601_HEIGHT / 2 - BAR_H / 2;   /* start centred, not at the edge */

    /*
     * Establish the scene through gfx before the pacing loop takes over.
     *
     * The gfx demo leaves a blue background and a centred box; this loop draws
     * a bar on black. Painting it here means g_model, g_sent and the panel all
     * agree before the safety net comes down - and with resync off, any
     * disagreement would sit on the glass for twenty seconds looking exactly
     * like a marking bug.
     */
    (void)gfx_solid(0u, SH8601_HEIGHT - 1u, BLACKCOL);
    (void)gfx_solid((uint16_t)g_bar_y, (uint16_t)(g_bar_y + BAR_H - 1), BARCOL);
    gfx_text_clear(1); gfx_text_clear(2); gfx_text_clear(3);
    /* A STATIC label stays on screen for the whole paced run. It marks nothing
     * after the first present - a label that has not changed cannot differ from
     * what the panel holds - so the wrap trace below stays exactly 192 rows and
     * the 20 s resync-off window is unaffected. It still RENDERS over the bar
     * every time the bar passes under it, because the blit is transparent and
     * gfx_rowfn draws runs then text. Static text being free is the whole
     * point of describing it rather than drawing it. */
    (void)gfx_text(0, 16u, 8u, "metal99 60Hz", sh8601_rgb565(120, 220, 255),
                   &share_mono_16x32);
    (void)gfx_present();
    {
    uint32_t period = CPU_HZ / 60u;      /* cycles in one 60 Hz frame */
    uint32_t next   = cpu_cycles();
    uint32_t late   = 0u;
    /*
     * TAKE THE SAFETY NET DOWN, ON PURPOSE.
     *
     * elide.h documents elide_set_resync(0) as exactly this test: "If the
     * display stays correct with resync off, the tracking is genuinely right
     * rather than merely being repaired often enough to look right." Until now
     * nothing called it - --gc-sections dropped it from the image entirely, so
     * the escape hatch had never once been pulled.
     *
     * It matters because the rolling resync rewrites the whole screen every
     * 112 frames (~1.9 s) while the bar completes a traversal every 88 frames
     * (~1.5 s). A marking leak is therefore scrubbed at about the rate it
     * accumulates, and a clean screen proves nothing over a short watch. That
     * is precisely how the g_bar_prev bug hid (DESIGN.md 6.6k): it "only became
     * visible with the safety net switched off".
     *
     * 20 s with the net down is ~13 wraps. A leak at the wrap - the documented
     * failure - would stack visibly.
     */
    /* UNSIGNED. This ran forever on a signed int, so at 60 Hz it reached
     * INT_MAX in about 414 days and the increment became undefined behaviour -
     * which -Os is entitled to assume never happens when folding `i % 60`.
     * Unsigned wraparound is defined, and the counter is only used for
     * modular reporting. */
    uint32_t f;
    int net_off = 0, wrapped = 0;

    for (f = 0u; ; f++) {
        if (f == STATIONARY_FRAMES) {
            elide_set_resync(0u);
            net_off = 1;
            con_puts("\r\n*** resync OFF: dirty tracking stands alone for 20s"
                     " - any leak now accumulates ***\r\n");
        }
        if (f == STATIONARY_FRAMES + RESYNC_OFF_FRAMES) {
            elide_set_resync(ELIDE_RESYNC_FRAMES);
            net_off = 0;
            con_puts("*** resync back ON ***\r\n");
        }
        const elide_stats *e;
        uint32_t fps10, budget10;

        /*
         * DRIVEN THROUGH gfx, NOT RAW elide.
         *
         * This loop is the strongest verification in the project - 20 s with
         * the resync safety net down, every wrap traced - and it used to call
         * elide_mark() and scene() directly, so it tested elide's marking and
         * nothing above it. gfx's second model, g_sent, is what the panel is
         * believed to hold; if that ever drifts from reality the symptom is
         * exactly what this loop exists to catch, and it was the one layer the
         * loop did not touch.
         *
         * Describing the scene rather than marking it also removes the last
         * hand-marking from the demo. There is nothing left here to get wrong.
         */
        {
            /* First STATIONARY_FRAMES (3s at 60Hz): bar STATIONARY. If the screen
             * shows one clean bar on black, the window/write path is correct
             * and any smearing afterwards is a MARKING problem. */
            int step  = (f < STATIONARY_FRAMES) ? 0 : 4;
            int old_y = g_bar_y;
            int new_y = (g_bar_y + step) % BAR_TRAVEL;
            /* THE WRAP IS THE FAILURE POINT. Everywhere else consecutive bar
             * positions overlap by 92 of 96 rows, so even a badly wrong mark
             * gets covered by its neighbour. At the wrap old and new stop being
             * adjacent, the cover fails, and a marking bug leaves red behind -
             * which is exactly what DESIGN.md 6.6k records. Trace it explicitly
             * rather than hoping the every-60-frames telemetry lands on one:
             * the bar wraps every 88 frames, so it mostly does not. */
            wrapped = (new_y < old_y);
            if (new_y != old_y)
                (void)gfx_solid((uint16_t)old_y,
                                (uint16_t)(old_y + BAR_H - 1), BLACKCOL);
            (void)gfx_solid((uint16_t)new_y,
                            (uint16_t)(new_y + BAR_H - 1), BARCOL);
            g_bar_y = new_y;
        }

        rc = gfx_present();
        if (rc != SPI2_OK) {
            con_puts("  flush FAILED rc="); con_dec((int32_t)rc);
            con_puts(" (-5 sync -6 usr -7 dma)  gdma_raw=");
            con_hex32(gdma_last_status()); con_puts("\r\n");
            gfx_invalidate(); delay_ms(500u); continue;
        }
        e = elide_last();

        /* Wait out the rest of the 60 Hz period. If we are already past it,
         * count it as a miss rather than silently drifting. */
        if ((cpu_cycles() - next) < period) {
            while ((cpu_cycles() - next) < period) { }
            next += period;
        } else {
            /* Overran. Re-base rather than advancing by one period: persistent
             * lateness would otherwise accumulate without bound, and once it
             * passed 2^31 cycles (13.4 s at 160 MHz) the unsigned comparison
             * inverts and pacing silently stops working. */
            late++;
            next = cpu_cycles();
        }

        if (wrapped) {
            con_puts("  WRAP f=");   con_dec((int32_t)f);
            con_puts(" rows=");      con_dec((int32_t)e->rows_sent);
            con_puts(" spans=");     con_dec((int32_t)e->spans);
            /* At the wrap old and new do not overlap, so BOTH bands are net
             * changes: 96 erased plus 96 drawn. Between wraps only the 4 rows
             * vacated and the 4 newly covered differ from what the panel holds,
             * which is what present-time diffing buys - the steady state fell
             * from 100 rows to 8. */
            con_puts(" expect rows=192 spans=2");
            con_puts(net_off ? "  [resync OFF]\r\n" : "  [resync on]\r\n");
        }

        /* Trace the first frames: what did we mark, what got sent? */
        if (f < 6u) {
            con_puts("  f"); con_dec((int32_t)f);
            con_puts(" bar_y="); con_dec((int32_t)g_bar_y);
            con_puts(" rows="); con_dec((int32_t)e->rows_sent);
            con_puts(" spans="); con_dec((int32_t)e->spans);
            con_puts("\r\n");
        }
        if ((f % 60u) == 0u) {
            fps10    = (e->cycles == 0u) ? 0u
                     : (uint32_t)(((uint64_t)CPU_HZ * 10u) / e->cycles);
            /* How many such updates fit in one 60Hz frame period. */
            budget10 = (e->cycles == 0u) ? 0u
                     : (uint32_t)(((uint64_t)CPU_HZ / 60u) * 10u / e->cycles);
            con_puts("elide | ");
            con_dec((int32_t)e->rows_sent); con_puts(" rows ");
            con_dec((int32_t)e->spans);     con_puts(" spans | ");
            put_ms(e->cycles);
            con_puts("| "); con_dec((int32_t)(fps10 / 10u)); con_putc('.');
            con_dec((int32_t)(fps10 % 10u)); con_puts(" fps | ");
            con_dec((int32_t)(budget10 / 10u)); con_putc('.');
            con_dec((int32_t)(budget10 % 10u)); con_puts("x | late=");
            con_dec((int32_t)late);
            /* Where the time actually goes. sh8601 measures this per span and
             * elide sums it across the frame; before that it was computed every
             * span and read by nobody. */
            con_puts(" | render="); put_ms(e->render_cycles);
            con_puts("flush=");     put_ms(e->flush_cycles);
            con_puts(" px="); con_dec((int32_t)e->px_sent);
            con_puts(net_off ? " | resync=OFF\r\n" : " | resync=on\r\n");
        }
    }
    }
}
