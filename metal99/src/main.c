#include <stddef.h>
#include "io.h"
#include "vec.h"
#include "clk.h"
#include "spi2.h"
#include "gdma.h"
#include "sh8601.h"
#include "elide.h"
#include "gfx.h"
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

static void put_ms(uint32_t cycles)
{
    uint32_t us = cycles / (CPU_HZ / 1000000u);
    con_dec((int32_t)(us / 1000u)); con_putc('.');
    con_dec((int32_t)((us % 1000u) / 100u)); con_puts("ms ");
}

void app_entry(void)
{
    int rc, i;

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
        ch = gfx_solid(0u, SH8601_HEIGHT - 1u, sh8601_rgb565(0, 20, 60));
        con_puts("  paint background : "); con_dec((int32_t)ch); con_puts(" rows changed\r\n");
        (void)gfx_present();

        /* Same colour again - should be FULLY elided, zero rows. */
        ch = gfx_solid(0u, SH8601_HEIGHT - 1u, sh8601_rgb565(0, 20, 60));
        con_puts("  repaint same     : "); con_dec((int32_t)ch);
        con_puts(" rows changed (0 = elided)\r\n");
        (void)gfx_present();
        con_puts("  -> transmitted   : "); con_dec((int32_t)gfx_last()->rows_sent);
        con_puts(" rows\r\n");
    }

    /* Animate a bar by describing WHERE IT IS, not what changed. The layer
     * works out the difference, so there is no marking to get wrong. */
    {
        int prev = -1;
        con_puts("  animating - layer derives dirty rows itself\r\n");
        for (i = 0; i < 240; i++) {
            int by = (i * 4) % BAR_TRAVEL;
            uint32_t changed;

            if (prev >= 0) (void)gfx_solid((uint16_t)prev,
                                           (uint16_t)(prev + BAR_H - 1),
                                           sh8601_rgb565(0, 20, 60));
            changed = gfx_solid((uint16_t)by, (uint16_t)(by + BAR_H - 1),
                                sh8601_rgb565(255, 70, 0));
            prev = by;

            rc = gfx_present();
            if (rc != SPI2_OK) { con_puts("  gfx_present FAILED\r\n"); break; }
            if ((i % 60) == 0) {
                const gfx_stats *g = gfx_last();
                con_puts("   changed="); con_dec((int32_t)changed);
                con_puts(" model="); con_dec((int32_t)g->rows_changed);
                con_puts(" sent="); con_dec((int32_t)g->rows_sent);
                con_puts(" spans="); con_dec((int32_t)g->spans);
                con_puts(" "); put_ms(g->cycles); con_puts("\r\n");
            }
            delay_ms(16u);
        }
        con_puts("  gfx demo done\r\n");
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
     * Full repaint before the pacing loop takes over.
     *
     * The gfx demo leaves a blue background on the panel; scene() draws black.
     * Without this the first paced frame marks only the bar's rows and the old
     * background survives underneath - repaired a few frames later by the
     * rolling resync, which is why it was never noticed. With the resync
     * switched off below it would sit there for twenty seconds and look
     * exactly like a marking bug.
     */
    elide_reset();
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

        /* Erase where it IS, draw where it will be.
         *
         * The previous version marked g_bar_prev, which held the position from
         * TWO frames ago. It only worked by accident: with a 4px step and a
         * 96px bar, consecutive positions overlap so heavily that the union
         * covered the gap anyway. At the wrap the positions stop being
         * adjacent, the cover fails, and red is left behind permanently.
         *
         * Resync hid this completely - every 120 frames scrubbed the evidence.
         * It only became visible with the safety net switched off. */
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
            elide_mark(old_y, old_y + BAR_H - 1);   /* erase */
            elide_mark(new_y, new_y + BAR_H - 1);   /* draw  */
            g_bar_y = new_y;
        }

        rc = elide_flush(scene);
        if (rc != SPI2_OK) {
            con_puts("  flush FAILED rc="); con_dec((int32_t)rc);
            con_puts(" (-5 sync -6 usr -7 dma)  gdma_raw=");
            con_hex32(gdma_last_status()); con_puts("\r\n");
            elide_reset(); delay_ms(500u); continue;
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
            con_puts(net_off ? "| resync=OFF\r\n" : "| resync=on\r\n");
        }
    }
    }
}
