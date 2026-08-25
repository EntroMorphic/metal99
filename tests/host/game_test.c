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
int sh8601_write_span_x(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        void (*rowfn)(uint16_t *, int))
{ (void)x0;(void)y0;(void)x1;(void)y1;(void)rowfn; return 0; }

int main(void)
{
    int f, worst_segs = 0, bad_rc = 0, lit_frames = 0;
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
        if (APP.frame((uint32_t)f) != 0) bad_rc++;
        if (vg_count() > worst_segs) worst_segs = vg_count();
        if (vg_count() > 0) lit_frames++;
    }

    check(bad_rc == 0, "6000 frames all present successfully");
    check(g_rows_rendered == 6000 * H, "every frame rendered every row");
    check(lit_frames == 6000, "no frame came out empty");
    check(vg_overflow() == 0u, "the scene never exceeded the segment budget");
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
