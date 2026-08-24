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
static int g_bar_prev;

static void scene(uint16_t *row, int y)
{
    if ((y >= g_bar_y) && (y < g_bar_y + BAR_H)) {
        vec_fill16(row, sh8601_rgb565(255, 40, 0), ROW_VECTORS);      /* the mover */
    } else {
        /* Static background: five bands. */
        static uint16_t bg[5];
        static int ready = 0;
        if (!ready) {
            bg[0] = sh8601_rgb565(10, 10, 30);   bg[1] = sh8601_rgb565(20, 20, 60);
            bg[2] = sh8601_rgb565(30, 30, 90);   bg[3] = sh8601_rgb565(20, 20, 60);
            bg[4] = sh8601_rgb565(10, 10, 30);
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
    elide_init();

    /* HOW MUCH OF THE SCREEN CAN CHANGE PER 60Hz FRAME?
     *
     * "60 fps" is the wrong question; the panel keeps its own framebuffer, so
     * the real limit is rows-updated-per-16.67ms. Sweep the dirty row count
     * over REAL updates and find where the budget runs out. */
    con_puts("rows | update  | % of 60Hz budget | verdict\r\n");
    {
        static const int counts[9] = { 32, 64, 96, 128, 192, 256, 320, 384, 448 };
        int k;
        for (k = 0; k < 9; k++) {
            const elide_stats *e;
            uint32_t pct;

            elide_reset();
            (void)elide_flush(scene);          /* settle: full repaint first */

            elide_mark(0, counts[k] - 1);
            rc = elide_flush(scene);
            if (rc != SPI2_OK) { con_puts("  FAILED\r\n"); continue; }
            e = elide_last();

            /* 60Hz period in CPU cycles = CPU_HZ/60 */
            pct = (uint32_t)(((uint64_t)e->cycles * 100u) / (CPU_HZ / 60u));
            con_dec((int32_t)e->rows_sent); con_puts(" | ");
            put_ms(e->cycles);
            con_puts("| "); con_dec((int32_t)pct); con_puts("% | ");
            con_puts(pct <= 100u ? "FITS 60Hz\r\n" : "misses\r\n");
            delay_ms(80u);
        }
        con_puts("---\r\n");
    }

    con_puts("mode | rows spans | update | eff fps | headroom vs 16.67ms\r\n");

    g_bar_y = 0; g_bar_prev = 0;
    for (i = 0; ; i++) {
        const elide_stats *e;
        uint32_t fps10, budget10;

        /* Declare what moved: the bar's old position and its new one. */
        elide_mark(g_bar_prev, g_bar_prev + BAR_H - 1);
        g_bar_prev = g_bar_y;
        g_bar_y = (g_bar_y + 4) % (SH8601_HEIGHT - BAR_H);
        elide_mark(g_bar_y, g_bar_y + BAR_H - 1);

        rc = elide_flush(scene);
        if (rc != SPI2_OK) {
            con_puts("  flush FAILED rc="); con_dec((int32_t)rc); con_puts("\r\n");
            elide_reset(); delay_ms(500u); continue;
        }
        e = elide_last();

        if ((i % 30) == 0 || e->was_resync) {
            fps10    = (e->cycles == 0u) ? 0u
                     : (uint32_t)(((uint64_t)CPU_HZ * 10u) / e->cycles);
            /* How many such updates fit in one 60Hz frame period. */
            budget10 = (e->cycles == 0u) ? 0u
                     : (uint32_t)(((uint64_t)CPU_HZ / 60u) * 10u / e->cycles);
            con_puts(e->was_resync ? "RESYNC| " : "elide | ");
            con_dec((int32_t)e->rows_sent); con_puts(" rows ");
            con_dec((int32_t)e->spans);     con_puts(" spans | ");
            put_ms(e->cycles);
            con_puts("| "); con_dec((int32_t)(fps10 / 10u)); con_putc('.');
            con_dec((int32_t)(fps10 % 10u)); con_puts(" fps | ");
            con_dec((int32_t)(budget10 / 10u)); con_putc('.');
            con_dec((int32_t)(budget10 % 10u)); con_puts("x\r\n");
        }
    }
}
