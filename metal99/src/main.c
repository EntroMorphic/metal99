/*
 * Firmware entry. Brings the hardware up, proves it works, then runs the app.
 *
 * This file owns nothing an interface cares about. It used to own everything -
 * 597 lines with the demo tangled through boot, the self-test and the pacing
 * loop - so changing a button meant editing the code that makes the board come
 * up. The application is now app.h/APP, and this is the platform beneath it.
 */
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
#include "i2c.h"
#include "touch.h"
#include "ui.h"
#include "app.h"

/* One knob, one place. 80 needs the PLL (APB is 20 MHz without it). */
#define SPI_MHZ 80u

#define ROW_VECTORS (SH8601_WIDTH * 2 / VEC_BYTES)
#define BLACKCOL    sh8601_rgb565(0, 0, 0)
#define BARCOL      sh8601_rgb565(255, 60, 0)

/* --- verification scene, deliberately NOT the app's ---------------------- */
#define VBAR_H      96
#define VBAR_TRAVEL (SH8601_HEIGHT - VBAR_H)
#define STATIONARY_FRAMES 180u      /* 3 s: a clean static bar proves the write path */
#define RESYNC_OFF_FRAMES 300u      /* 5 s with the safety net down, ~3 wraps         */

static uint16_t g_bandcol;
static void solid_row(uint16_t *row, int y) { (void)y; vec_fill16(row, g_bandcol, ROW_VECTORS); }

static void put_ms(uint32_t cycles)
{
    uint32_t us = cycles / (CPU_HZ / 1000000u);
    con_dec((int32_t)(us / 1000u)); con_putc('.');
    con_dec((int32_t)((us % 1000u) / 100u)); con_puts("ms ");
}

/*
 * WINDOW GEOMETRY, ON GLASS.
 *
 * The ledger proves which bytes reached the peripheral and is structurally
 * blind to where the panel puts them - the gap that let banded DMA pass every
 * check while looking wrong (DESIGN.md 6.6l). Three bands of known geometry,
 * two deliberately narrow and on opposite edges, is the cheapest thing that
 * can see it. It is what proved partial column windows ARE honoured, and then
 * showed colour bleeding from one band into the start of the next (11.4).
 */
static void window_check(void)
{
    g_bandcol = BLACKCOL;
    (void)sh8601_write_frame(solid_row);
    g_bandcol = sh8601_rgb565(255, 60, 0);
    (void)sh8601_write_span_x(0u, 120u, (uint16_t)(SH8601_WIDTH - 1), 159u, solid_row);
    g_bandcol = sh8601_rgb565(0, 220, 120);
    (void)sh8601_write_span_x(0u, 220u, 63u, 259u, solid_row);
    g_bandcol = sh8601_rgb565(80, 140, 255);
    (void)sh8601_write_span_x(304u, 300u, 367u, 339u, solid_row);
    con_puts("window check: orange full width, green left sixth, blue right"
             " sixth - edges should be clean\r\n");
    delay_ms(2000u);
}

/*
 * MARKING, WITH THE SAFETY NET DOWN.
 *
 * The rolling resync rewrites the screen every ~1.9 s while this bar wraps
 * every ~1.5 s, so a marking leak is scrubbed at about the rate it accumulates
 * and a clean screen proves nothing. With resync off it accumulates instead.
 *
 * Its own scene, not the app's: this verifies the FIRMWARE, and must keep
 * working whatever application is linked. At the wrap old and new positions do
 * not overlap, so both bands are genuine net changes - 192 rows, 2 spans.
 * Between wraps only the 4 rows vacated and the 4 newly covered differ.
 */
static void verify_marking(void)
{
    uint32_t f, late = 0u, period = CPU_HZ / 60u, next;
    int bar = SH8601_HEIGHT / 2 - VBAR_H / 2, rc;

    con_puts("\r\nmarking check: 3s stationary, then 5s with resync OFF\r\n");
    gfx_init();
    (void)gfx_solid(0u, SH8601_HEIGHT - 1u, BLACKCOL);
    (void)gfx_solid((uint16_t)bar, (uint16_t)(bar + VBAR_H - 1), BARCOL);
    (void)gfx_present();
    next = cpu_cycles();

    for (f = 0u; f < STATIONARY_FRAMES + RESYNC_OFF_FRAMES; f++) {
        const elide_stats *e;
        int old_bar = bar, wrapped;

        if (f == STATIONARY_FRAMES) {
            elide_set_resync(0u);
            con_puts("  resync OFF - dirty tracking stands alone\r\n");
        }
        bar = (bar + ((f < STATIONARY_FRAMES) ? 0 : 4)) % VBAR_TRAVEL;
        wrapped = (bar < old_bar);

        (void)gfx_solid(0u, SH8601_HEIGHT - 1u, BLACKCOL);
        (void)gfx_solid((uint16_t)bar, (uint16_t)(bar + VBAR_H - 1), BARCOL);
        rc = gfx_present();
        if (rc != SPI2_OK) {
            con_puts("  flush FAILED rc="); con_dec((int32_t)rc); con_puts("\r\n");
            gfx_invalidate(); continue;
        }
        e = elide_last();
        if (wrapped) {
            con_puts("  WRAP rows="); con_dec((int32_t)e->rows_sent);
            con_puts(" spans=");      con_dec((int32_t)e->spans);
            con_puts(" expect 192/2\r\n");
        }
        if ((cpu_cycles() - next) < period) {
            while ((cpu_cycles() - next) < period) { }
            next += period;
        } else { late++; next = cpu_cycles(); }
    }
    elide_set_resync(ELIDE_RESYNC_FRAMES);
    con_puts("  late="); con_dec((int32_t)late); con_puts("\r\n");
}

void app_entry(void)
{
    int rc, trc;
    uint32_t f, late = 0u, period, next, worst = 0u;

    con_puts("\r\n=== metal99 ===\r\n");
    /* NOT ignorable: g_cpu_hz only updates on success, so a silent failure
     * leaves every delay computed for 20 MHz while the core runs at 160 -
     * including the SH8601's 120 ms sleep-out MINIMUM. */
    rc = clk_set_cpu_pll(160u);
    if (rc != CLK_OK) { con_puts("PLL FAILED rc="); con_dec((int32_t)rc);
                        con_puts(" - staying at 20 MHz\r\n"); }
    spi2_init();
    gdma_init();
    /*
     * BUS CLOCK. 40 was the vendor BSP's choice and we inherited it without
     * testing the alternative.
     *
     * It is not just throughput. At 40 MHz a full vector repaint is 31.2 ms
     * against the panel's ~16.7 ms refresh, so the scanout laps our write and
     * crosses it repeatedly - which is what makes moving wireframes shimmer
     * while static ones sit perfectly still. At 80 the write is ~15.6 ms,
     * under one refresh period, so we cross the beam once per frame instead of
     * being overtaken by it. Without a TE line (this board does not route one -
     * see apps/tescan.c) outrunning the scanout is the only phase control we
     * have.
     *
     * The failure mode is visible and harmless: a panel that cannot keep up
     * shows corruption immediately. Drop to 40 if it does.
     */
    (void)spi2_set_clock(SPI_MHZ);
    rc = sh8601_init();
    con_puts("sh8601_init rc="); con_dec((int32_t)rc); con_puts("\r\n");

    /* Identity BEFORE coordinates: the V2 board's CST816 answers at the same
     * address, and reading its registers as FocalTech's yields plausible
     * coordinates from nowhere. */
    i2c_init();
    delay_ms(10u);
    trc = touch_init();
    con_puts("touch: rc="); con_dec((int32_t)trc);
    con_puts(" vendor="); con_hex32(touch_vendor_id());
    con_puts(" chip=");   con_hex32(touch_chip_id());
    con_puts(trc == TOUCH_OK ? "  FT3168\r\n" : "  ABSENT\r\n");

    window_check();
    selftest_liveness();
    rc = selftest_transport();
    con_puts("selftest_transport rc="); con_dec((int32_t)rc);
    con_puts(rc == 0 ? " (0 failures)\r\n" : " <-- FAILURES\r\n");

    /* One timed full repaint on the transport that ships. README quoted banded
     * DMA's 0.037 ms/row as if it described this; it does not (DESIGN.md 5). */
    {
        const sh8601_stats *s;
        g_bandcol = BLACKCOL;
        (void)sh8601_write_frame(solid_row);
        s = sh8601_last_frame();
        con_puts("full repaint, FIFO, 448 rows: total="); put_ms(s->total_cycles);
        con_puts("flush="); put_ms(s->flush_cycles); con_puts("\r\n");
    }

    verify_marking();

    /* ---- hand off ---- */
    con_puts("\r\napp: "); con_puts(APP.name);
    con_puts(" @ "); con_dec((int32_t)(APP.hz ? APP.hz : 60u)); con_puts(" Hz\r\n");
    gfx_init();
    ui_init();
    if (APP.init) APP.init();

    period = CPU_HZ / (APP.hz ? APP.hz : 60u);
    next   = cpu_cycles();
    for (f = 0u; ; f++) {
        uint32_t t0 = cpu_cycles(), spent;

        if (trc == TOUCH_OK) ui_poll(APP.event);
        rc = APP.frame ? APP.frame(f) : 0;
        if (rc != SPI2_OK) {
            con_puts("  frame FAILED rc="); con_dec((int32_t)rc); con_puts("\r\n");
            gfx_invalidate(); delay_ms(500u); continue;
        }
        /* Measured here rather than read from elide: an app that streams rows
         * straight to the panel never touches elide, and reporting its stale
         * numbers would be worse than reporting none. */
        spent = cpu_cycles() - t0;
        if (spent > worst) worst = spent;

        if ((cpu_cycles() - next) < period) {
            while ((cpu_cycles() - next) < period) { }
            next += period;
        } else {
            /* Re-base rather than advancing a period: persistent lateness would
             * accumulate without bound, and past 2^31 cycles the unsigned
             * comparison inverts and pacing silently stops working. */
            late++; next = cpu_cycles();
        }

        if ((f % 300u) == 0u) {
            con_puts("frame "); put_ms(spent);
            con_puts("| worst "); put_ms(worst);
            con_puts("| late="); con_dec((int32_t)late);
            con_puts("\r\n");
            worst = 0u;
        }
    }
}
