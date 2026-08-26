/*
 * SPANCOST - what does a span actually cost?
 *
 * Every design decision about elision granularity turns on this number and it
 * has never been measured. DESIGN.md quotes "a window command plus a 20 byte
 * preamble" - that is the BYTES. The time also includes two DCS commands with
 * their parameters, a CS cycle, and the register writes and UPDATE sync around
 * them, and those are CPU-bound, not wire-bound.
 *
 * It matters right now because tile-level diffing would cut gridvoid's
 * transmitted pixels from 88.7% to 27.6% while taking spans from 1.6 to 15.3
 * per frame. Whether that trade is a win, a wash, or a regression is entirely
 * decided by the per-span cost, and guessing it would be how you talk yourself
 * into rewriting a memory map for nothing.
 *
 * METHOD: transmit the SAME 448 rows split into 1, 2, 4 ... 64 spans. Identical
 * pixels, identical rendering, identical wire bytes - only the number of window
 * setups changes. The slope is the answer.
 *
 * APP=spancost ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"
#include "sh8601.h"
#include "spi2.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT

static uint16_t g_c;

/* Trivial and scalar on purpose: rendering must not vary between runs. */
static void flat_row(uint16_t *row, int y)
{
    int x;
    (void)y;
    for (x = 0; x < W; x++) row[x] = g_c;
}

static uint32_t us_of(uint32_t cycles) { return cycles / (CPU_HZ / 1000000u); }

static void measure(int nspans)
{
    int rows = H / nspans, i, rc = SPI2_OK;
    uint32_t t0, t1;

    g_c = (uint16_t)(0x0841u + (uint16_t)nspans);   /* force a real repaint */
    t0 = cpu_cycles();
    for (i = 0; i < nspans; i++) {
        int y0 = i * rows;
        int y1 = (i == nspans - 1) ? (H - 1) : (y0 + rows - 1);
        rc = sh8601_write_span_x(0u, (uint16_t)y0, (uint16_t)(W - 1),
                                 (uint16_t)y1, flat_row);
        if (rc != SPI2_OK) break;
    }
    t1 = cpu_cycles();

    con_puts("  spans="); con_dec((int32_t)nspans);
    con_puts("\trows/span="); con_dec((int32_t)rows);
    con_puts("\ttotal="); con_dec((int32_t)us_of(t1 - t0)); con_puts(" us");
    if (rc != SPI2_OK) { con_puts("  rc="); con_dec((int32_t)rc); }
    con_puts("\r\n");
}

static void sc_init(void)
{
    static const int N[] = { 1, 2, 4, 8, 16, 32, 64 };
    int i, pass;

    con_puts("\r\nspancost: same 448 rows, split into N spans\r\n");
    /* Two passes: the first also pays for whatever the panel does on a cold
     * window, and reporting that as the steady-state cost would overstate it. */
    for (pass = 0; pass < 2; pass++) {
        con_puts(pass ? "-- steady state --\r\n" : "-- warm-up (ignore) --\r\n");
        for (i = 0; i < (int)(sizeof N / sizeof N[0]); i++) measure(N[i]);
    }
    con_puts("spancost: done\r\n");
}

static int sc_frame(uint32_t f) { (void)f; return 0; }

const app_t APP = { "spancost", 1u, sc_init, sc_frame, 0 };
