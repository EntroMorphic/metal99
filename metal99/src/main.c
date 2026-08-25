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
#define TOUCHCOL sh8601_rgb565(120, 220, 255)
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

/* "<id> X:nnn Y:nnn" - 13 chars. The leading digit is the controller's own
 * tracking id, which is what makes two contacts distinguishable as they move
 * rather than just two coordinates that happen to exist. */
static void fmt_point(char *b, const touch_point *p)
{
    b[0] = (char)('0' + (p->id & 0x0Fu));
    b[1] = ' '; b[2] = 'X'; b[3] = ':';
    u32str(b + 4, (uint32_t)p->x, 3);
    b[7] = ' '; b[8] = 'Y'; b[9] = ':';
    u32str(b + 10, (uint32_t)p->y, 3);
}

/*
 * DOES THE PANEL HONOUR A PARTIAL COLUMN WINDOW?
 *
 * The transmit ledger proves which BYTES reached the peripheral - exactly, for
 * sub-width spans, on both transports. It says nothing about where the panel
 * PUTS them, and that gap is the whole remaining suspect: full-width marking
 * makes the artifact go away and sub-width brings it back, with every
 * measurable layer in between reporting correct.
 *
 * So draw two bands of known geometry and look at the glass. Nothing else here
 * can answer it.
 */
static uint16_t g_bandcol;
static void solid_row(uint16_t *row, int y) { (void)y; vec_fill16(row, g_bandcol, ROW_VECTORS); }

static void put_ms(uint32_t cycles)
{
    uint32_t us = cycles / (CPU_HZ / 1000000u);
    con_dec((int32_t)(us / 1000u)); con_putc('.');
    con_dec((int32_t)((us % 1000u) / 100u)); con_puts("ms ");
}

void app_entry(void)
{
    int rc, trc;

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

    /* ---- touch: first contact ----
     *
     * Identity BEFORE coordinates, exactly as the panel was brought up. A
     * controller that ACKs 0x38 is not necessarily a FocalTech part - the V2
     * board's CST816 sits at the same address - and reading its registers as
     * if they were FT5x06's would yield plausible coordinates from nowhere. */
    i2c_init();
    delay_ms(10u);          /* let the bus settle before the first transaction */
    /* SEPARATE variable. `rc` is reassigned by every call between here and the
     * touch demo, so testing it there tested whatever ran last - and the demo
     * duly ran on a boot where touch_init had reported the controller absent. */
    con_puts("  i2c lines before="); con_dec((int32_t)i2c_dbg_lines_before());
    con_puts(" after="); con_dec((int32_t)i2c_dbg_lines_after());
    con_puts(" pulses="); con_dec((int32_t)i2c_dbg_pulses());
    con_puts("  (3 = both high = idle)\r\n");
    /* Identity, then 20 more identity reads. If the controller answers once it
     * should answer every time; a mixed result means the bus is marginal rather
     * than the part being absent, and that is a different bug. Measured here
     * rather than across reboots, because reboot-based measurement was really
     * measuring whether the USB console re-enumerated in time. */
    trc = touch_init();
    con_puts("touch: rc="); con_dec((int32_t)trc);
    con_puts(" vendor="); con_hex32(touch_vendor_id());
    con_puts(" chip=");   con_hex32(touch_chip_id());
    /* Captured BEFORE the scan: the scan's own probes overwrite these, and a
     * wedged bus makes every one of them fail, so reading them afterwards
     * describes the scan rather than the failure being diagnosed. */
    con_puts(" int="); con_hex32(i2c_dbg_int());
    con_puts(" sr=");  con_hex32(i2c_dbg_sr());
    {
        int k, good = 0;
        uint8_t v;
        for (k = 0; k < 20; k++)
            if (i2c_read(0x38u, 0xA8u, &v, 1u) == I2C_OK && v == 0x11u) good++;
        con_puts("  identity reads: "); con_dec((int32_t)good);
        con_puts("/20 returned vendor 0x11\r\n");
    }
    con_puts(trc == TOUCH_OK ? "  FT3168 present\r\n"
                            : "  (-1 absent, -2 wrong vendor, -3 i2c)\r\n");
    /* ALWAYS scan, not only on failure. With correct timing the bus should show
     * exactly two devices - the FT3168 at 0x38 and the TCA9554 expander - and
     * anything else means the timing is wrong again rather than the part being
     * missing. An earlier build with sda_sample set wrong reported seven. */
    {
        uint8_t found[8];
        uint32_t n = i2c_scan(found, 8u), k;
        con_puts("  i2c scan: "); con_dec((int32_t)n); con_puts(" device(s)");
        for (k = 0u; k < n; k++) { con_puts(" "); con_hex32(found[k]); }
        con_puts("  int="); con_hex32(i2c_dbg_int());
        con_puts(" sr="); con_hex32(i2c_dbg_sr());
        con_puts("\r\n");
    }

    /*
     * WINDOW GEOMETRY, ON GLASS. Two seconds, every boot.
     *
     * The ledger proves which bytes reached the peripheral and is structurally
     * blind to where the panel puts them - the same gap that let banded DMA
     * pass every check while looking wrong (DESIGN.md 6.6l). This is the
     * cheapest thing that can see it: three bands of known geometry, two of
     * them deliberately narrow and on opposite edges.
     *
     * It earned its place. It is what showed that partial column windows ARE
     * honoured - killing the leading theory - and then showed colour from one
     * band leaking into the start of the next, which is what the bug actually
     * was.
     */
    {
        g_bandcol = BLACKCOL;
        (void)sh8601_write_frame(solid_row);
        g_bandcol = sh8601_rgb565(255, 60, 0);
        (void)sh8601_write_span_x(0u, 120u, (uint16_t)(SH8601_WIDTH - 1), 159u,
                                  solid_row);
        g_bandcol = sh8601_rgb565(0, 220, 120);
        (void)sh8601_write_span_x(0u, 220u, 63u, 259u, solid_row);
        g_bandcol = sh8601_rgb565(80, 140, 255);
        (void)sh8601_write_span_x(304u, 300u, 367u, 339u, solid_row);
        con_puts("window check: orange full width, green left sixth,"
                 " blue right sixth - edges should be clean\r\n");
        delay_ms(2000u);
    }

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
    /* CUMULATIVE, so a capture taken any time after a touch still carries the
     * evidence. Instantaneous counters meant the console had to be watched at
     * the exact moment a finger was down, which is not a workable way to
     * diagnose something only a human can trigger. */
    uint32_t fails = 0u, maxspans = 0u, lblframes = 0u, maxrows = 0u;
    char tbuf[16];
    touch_state ts;
    ts.n = 0u;
    tbuf[0] = '\0';

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
         * DESCRIBE THE WHOLE SCENE, EVERY FRAME.
         *
         * The previous version updated incrementally - erase the bar where it
         * was, draw it where it is, same for the touch box - and that is what
         * produced trails. Erasing the box painted BLACK over wherever it had
         * been, but what was underneath might have been the bar, so lifting a
         * finger punched a black hole in it that healed only when the bar next
         * scrolled over that row.
         *
         * That is the bug you get from doing the layer's job by hand. A
         * retained-mode model already knows what each row should look like;
         * telling it "erase this rectangle" is telling it about a STEP rather
         * than a STATE, and a step cannot know what it is covering.
         *
         * So: background, then bar, then box, in z-order, from scratch, every
         * frame. It costs nothing extra - gfx_present diffs against what the
         * panel actually holds (5.2), so a row described as orange that is
         * already orange marks nothing. The steady-state cost stays at the 8
         * rows the bar's leading and trailing edges represent, and the wrap
         * still marks exactly 192. Describing more does not transmit more.
         */
        {
            /* First STATIONARY_FRAMES (3s at 60Hz): bar STATIONARY. If the
             * screen shows one clean bar on black, the window/write path is
             * correct and any smearing afterwards is a MARKING problem. */
            int step  = (f < STATIONARY_FRAMES) ? 0 : 4;
            int old_y = g_bar_y;
            int new_y = (g_bar_y + step) % BAR_TRAVEL;
            /* THE WRAP IS THE FAILURE POINT. Everywhere else consecutive bar
             * positions overlap by 92 of 96 rows, so even a badly wrong mark
             * gets covered by its neighbour. At the wrap old and new stop being
             * adjacent, the cover fails, and a marking bug leaves colour behind
             * - which is exactly what DESIGN.md 6.6k records. Trace it
             * explicitly rather than hoping the every-60-frames telemetry lands
             * on one: the bar wraps every 88 frames, so it mostly does not. */
            wrapped = (new_y < old_y);
            g_bar_y = new_y;
        }

        if (trc == TOUCH_OK) (void)touch_poll(&ts);

        (void)gfx_solid(0u, SH8601_HEIGHT - 1u, BLACKCOL);
        (void)gfx_solid((uint16_t)g_bar_y,
                        (uint16_t)(g_bar_y + BAR_H - 1), BARCOL);
        /*
         * Both contacts, cleared on release.
         *
         * One label per slot rather than one line holding both: a label is a
         * description, so a contact that has not moved marks nothing, and
         * lifting one finger while the other stays down costs only the line
         * that actually went away.
         */
        if (ts.n > 0u) {
            fmt_point(tbuf, &ts.p[0]);
            (void)gfx_text(1, 16u, 56u, tbuf, TOUCHCOL, &share_mono_16x32);
        } else {
            gfx_text_clear(1);
        }
        if (ts.n > 1u) {
            fmt_point(tbuf, &ts.p[1]);
            (void)gfx_text(2, 16u, 96u, tbuf, TOUCHCOL, &share_mono_16x32);
        } else {
            gfx_text_clear(2);
        }

        rc = gfx_present();
        if (rc != SPI2_OK) {
            fails++;
            con_puts("  flush FAILED rc="); con_dec((int32_t)rc);
            con_puts(" (-5 sync -6 usr -7 dma)  gdma_raw=");
            con_hex32(gdma_last_status()); con_puts("\r\n");
            gfx_invalidate(); delay_ms(500u); continue;
        }
        e = elide_last();
        if (e->spans > maxspans)        maxspans = e->spans;
        if (e->rows_sent > maxrows)     maxrows  = e->rows_sent;
        if (gfx_last()->labels_changed) lblframes++;

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
            con_puts(" lbl="); con_dec((int32_t)gfx_last()->labels_changed);
            con_puts(ts.n > 0u ? "  [TOUCHED]" : "");
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
            con_puts(" lbl="); con_dec((int32_t)gfx_last()->labels_changed);
            con_puts(" maxsp="); con_dec((int32_t)maxspans);
            con_puts(" maxrows="); con_dec((int32_t)maxrows);
            con_puts(" lblfr="); con_dec((int32_t)lblframes);
            con_puts(" fails="); con_dec((int32_t)fails);
            con_puts(net_off ? " | resync=OFF\r\n" : " | resync=on\r\n");
        }
    }
    }
}
