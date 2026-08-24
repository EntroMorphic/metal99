#include <stddef.h>
#include "io.h"
#include "vec.h"
#include "clk.h"
#include "spi2.h"
#include "gdma.h"
#include "sh8601.h"
#include "elide.h"

#define ROW_VECTORS (SH8601_WIDTH * 2 / VEC_BYTES)

/* A representative interface: a static background with one moving element.
 * This is what real UIs look like - almost nothing changes between frames. */
#define BAR_H 24
static int g_bar_y;

static void scene(uint16_t *row, int y)
{
    if ((y >= g_bar_y) && (y < g_bar_y + BAR_H)) {
        vec_fill16(row, sh8601_rgb565(255, 40, 0), ROW_VECTORS);      /* the mover */
    } else {
        /* Static background: five bands. */
        static uint16_t bg[5];
        static int ready = 0;
        if (!ready) {
            /* Visible on an AMOLED. The first attempt used values around
             * rgb565(10,10,30), which is essentially black - the screen looked
             * unpowered. */
            bg[0] = sh8601_rgb565(0,  40, 120);  bg[1] = sh8601_rgb565(0,  70, 170);
            bg[2] = sh8601_rgb565(0, 110, 220);  bg[3] = sh8601_rgb565(0,  70, 170);
            bg[4] = sh8601_rgb565(0,  40, 120);
            ready = 1;
        }
        vec_fill16(row, bg[(y * 5) / SH8601_HEIGHT], ROW_VECTORS);
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
    (void)clk_set_cpu_pll(160u);
    spi2_init();
    gdma_init();
    (void)spi2_set_clock(40u);
    rc = sh8601_init();
    con_puts("sh8601_init rc="); con_dec((int32_t)rc); con_puts("\r\n");
    sh8601_set_dma(1);

    /* HYPOTHESIS: the first DMA after sh8601_init needs settling time. The
     * failure recovers only after the error path's 500ms delay, and a full
     * channel reset plus re-arm does NOT fix it - which points at time rather
     * than state. */
    /* settle delay: tested, made no difference */

    elide_init();
    elide_set_resync(ELIDE_RESYNC_FRAMES);

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
    g_bar_y = 0;
    {
    uint32_t period = CPU_HZ / 60u;      /* cycles in one 60 Hz frame */
    uint32_t next   = cpu_cycles();
    uint32_t late   = 0u;

    for (i = 0; ; i++) {
        const elide_stats *e;
        uint32_t fps10, budget10;

        /* Erase where it IS, draw where it will be.
         *
         * The previous version marked g_bar_prev, which held the position from
         * TWO frames ago. It only worked by accident: with a 4px step and a
         * 24px bar, consecutive positions overlap so heavily that the union
         * covered the gap anyway. At the wrap the positions stop being
         * adjacent, the cover fails, and red is left behind permanently.
         *
         * Resync hid this completely - every 120 frames scrubbed the evidence.
         * It only became visible with the safety net switched off. */
        {
            int old_y = g_bar_y;
            int new_y = (g_bar_y + 4) % (SH8601_HEIGHT - BAR_H);
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
        } else {
            late++;
        }
        next += period;

        /* Trace the first frames: what did we mark, what got sent? */
        if (i < 6) {
            con_puts("  f"); con_dec((int32_t)i);
            con_puts(" bar_y="); con_dec((int32_t)g_bar_y);
            con_puts(" rows="); con_dec((int32_t)e->rows_sent);
            con_puts(" spans="); con_dec((int32_t)e->spans);
            con_puts("\r\n");
        }
        if ((i % 60) == 0) {
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
            con_dec((int32_t)late); con_puts("\r\n");
        }
    }
    }
}
