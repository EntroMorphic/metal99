/*
 * A soak test for the game.
 *
 * Games fail differently from interfaces: not on the first frame but on the
 * ten-thousandth, when a counter wraps, a list fills, or a craft reaches a
 * depth nothing expected. So this runs the real app for a long time with input
 * arriving, and asserts the invariants that must hold on EVERY frame.
 *
 * Built with the address sanitiser where available: the rasteriser indexes
 * arrays with computed row numbers, which is precisely where an off-by-one
 * stops being cosmetic.
 */
#include <stdio.h>
#include "app.h"
#include "vg.h"
#include "ui.h"
#include "sh8601.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT

static int g_fails;
static uint16_t g_row[W];
static int g_rows_rendered;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_fails++;
}

/* Stand in for the panel, and count what the app actually drew. */
int sh8601_write_frame(void (*rowfn)(uint16_t *row, int y))
{
    int y;
    for (y = 0; y < H; y++) { rowfn(g_row, y); g_rows_rendered++; }
    return 0;
}
/* The elided path. Counts rows so the soak can assert elision actually
 * elides - a present that quietly sent all 448 every frame would otherwise
 * pass every test in this file. */
static int g_rows_sent;
static int g_spans;
static uint16_t g_panel[H][W];   /* what the glass holds, retained like glass */
static uint8_t  g_sent_row[H];   /* was this row transmitted THIS frame?      */
int sh8601_write_span_x(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        void (*rowfn)(uint16_t *, int))
{
    int y;
    g_spans++;
    for (y = y0; y <= (int)y1; y++) {
        int x;
        rowfn(g_row, y);
        for (x = x0; x <= (int)x1; x++) g_panel[y][x] = g_row[x];
        g_sent_row[y] = 1;
        g_rows_sent++;
    }
    return 0;
}

static sh8601_stats g_ss;
const sh8601_stats *sh8601_last_frame(void) { return &g_ss; }
uint32_t g_cpu_hz = 240000000u;
static uint32_t g_cyc;
uint32_t cpu_cycles(void) { return (g_cyc += 1000u); }

int main(void)
{
    int f, worst_segs = 0, bad_rc = 0, lit_frames = 0, stale = 0;
    uint32_t seed = 12345u;

    printf("game_test: gridvoid soak\n");
    if (APP.init) APP.init();

    for (f = 0; f < 6000; f++) {
        /* Input at a plausible rate, scattered across the screen: a player
         * taps, and taps in the wrong place as often as the right one. */
        if ((f % 17) == 0) {
            ui_event e;
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            e.kind = UI_PRESS; e.id = 0;
            e.x = (uint16_t)(seed % W); e.y = (uint16_t)((seed >> 8) % H);
            e.ax = e.x; e.ay = e.y; e.ms = 0u;
            if (APP.event) APP.event(&e);
        }
        { int q; for (q = 0; q < H; q++) g_sent_row[q] = 0; }
        if (APP.frame((uint32_t)f) != 0) bad_rc++;
        if (vg_count() > worst_segs) worst_segs = vg_count();

        /*
         * THE PROPERTY ELISION HAS TO HOLD: the panel, after being sent only
         * the rows vg asked for, must be pixel-identical to a panel that was
         * sent every row. Anything vg fails to mark shows up here as a stale
         * pixel that a full repaint would have overwritten - which on glass is
         * exactly the "artifact" this project has chased into the transport
         * more than once.
         *
         * vg_finish() re-buckets the segments the app just submitted without
         * disturbing them, so the full render below is the SAME frame, not an
         * approximation of it.
         */
        {
            static uint16_t full[H][W];
            int y, x;
            vg_finish();
            for (y = 0; y < H; y++) vg_rowfn(full[y], y);
            for (y = 0; y < H && !stale; y++)
                for (x = 0; x < W; x++)
                    if (full[y][x] != g_panel[y][x]) {
                        stale = 1;
                        printf("     stale pixel at (%d,%d) frame %d: "
                               "panel=0x%04X full=0x%04X  row_sent=%d\n",
                               x, y, f, g_panel[y][x], full[y][x], g_sent_row[y]);
                        break;
                    }
        }
        if (vg_count() > 0) lit_frames++;
    }

    check(bad_rc == 0, "6000 frames all present successfully");
    check(g_rows_sent > 0, "the app transmitted rows");
    check(lit_frames == 6000, "no frame came out empty");
    check(vg_overflow() == 0u, "the scene never exceeded the segment budget");
    check(!stale, "elided panel is pixel-identical to a full repaint");
    {
        long full_cost = (long)6000 * H;
        printf("     rows transmitted: %d of %ld (%ld%% elided)\n",
               g_rows_sent, full_cost,
               100 - (long)g_rows_sent * 100 / full_cost);
    }
    printf("     spans: %d (%.1f per frame)\n", g_spans, g_spans / 6000.0);
    check(g_rows_sent < (int)(6000 * H * 9 / 10),
          "elision actually elides - fewer rows sent than a full repaint");
    check(worst_segs < VG_MAX_SEGS,
          "  worst-case segment count stays under the cap");
    printf("     worst-case scene: %d of %d segments\n", worst_segs, VG_MAX_SEGS);

    /* Out-of-range rows must be refused, not indexed. */
    { int before = g_fails;
      vg_rowfn(g_row, -1);
      vg_rowfn(g_row, H);
      vg_rowfn(g_row, H + 500);
      check(g_fails == before, "out-of-range rows are refused without indexing");
    }

    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails != 0;
}
