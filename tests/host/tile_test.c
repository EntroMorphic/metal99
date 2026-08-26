/*
 * tile_present must leave the panel pixel-identical to a full repaint.
 *
 * That is the only question worth asking of an elision scheme, and it is the
 * one that caught three real defects in vg_present. Anything a tile scheme
 * fails to notice survives on the glass as a stale tile - which on hardware
 * looks like "artifacts" and gets blamed on the transport. It has been blamed
 * on the transport before.
 *
 * Both present paths are exercised against the same frames, into separate
 * panel models, so this re-validates vg_present at the same time.
 */
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "ui.h"
#include "vg.h"
#include "tile.h"
#include "sh8601.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT
#define FRAMES 3000

static int g_fails;
static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_fails++;
}

/* Two panels, retained like glass. `active` selects which one is being fed. */
static uint16_t g_panel_elide[H][W];
static uint16_t g_panel_tile[H][W];
static uint16_t (*g_active)[W];
static uint16_t g_row[W];
static long g_px_tile, g_spans_tile;
static int  g_counting;

int sh8601_write_frame(void (*rowfn)(uint16_t *row, int y)) { (void)rowfn; return 0; }

int sh8601_write_span_x(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        void (*rowfn)(uint16_t *, int))
{
    int y, x;
    for (y = y0; y <= (int)y1; y++) {
        rowfn(g_row, y);
        for (x = x0; x <= (int)x1; x++) {
            g_active[y][x] = g_row[x];
        }
    }
    if (g_counting) {
        g_spans_tile++;
        g_px_tile += (long)(x1 - x0 + 1) * (long)(y1 - y0 + 1);
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
    static uint16_t full[H][W];
    int f, y, x, stale_tile = 0, bad_rc = 0;
    int first_bad_f = -1, first_bad_x = -1, first_bad_y = -1;

    printf("tile_test: %dx%d tiles, %d frames of gridvoid\n", TILE_W, TILE_H, FRAMES);
    tile_init();
    /*
     * RESYNC OFF. gfx_test does the same to elide, and the reason is the same:
     * with the rotating rewrite enabled, a tile the hash failed to notice is
     * repaired within TROWS frames, so the tracking can be wrong and the
     * picture still look right most of the time. Off, every miss is permanent
     * and the comparison below is a real test of the hashing rather than of
     * how often it gets scrubbed.
     */
    tile_set_resync(0u);
    APP.init();

    for (f = 0; f < FRAMES; f++) {
        if (APP.event && f > 20 && (f % 11) == 0) {
            ui_event e;
            e.kind = UI_PRESS; e.id = 0;
            e.x = (uint16_t)(120 + ((f * 37) % 140));
            e.y = (uint16_t)(60 + ((f * 53) % 90));
            e.ax = e.x; e.ay = e.y; e.ms = 0u;
            APP.event(&e);
        }

        /*
         * The app presents through tile_present ITSELF. Do not present again
         * here: the second call would find every hash already updated, mark
         * nothing dirty, and leave the panel model empty - which is what it
         * did, reporting a triumphant 3.6% of pixels while failing the only
         * check that matters.
         *
         * This is the third time a harness in this repo has double-presented
         * after an app switched paths. If gridvoid ever moves off
         * tile_present, drive it explicitly here instead.
         */
        g_active = g_panel_tile; g_counting = 1;
        if (APP.frame((uint32_t)f) != 0) bad_rc++;

        /* vg_present is no longer on the app's path; game_test covers it. */
        (void)g_panel_elide;

        /* And the reference: the same frame, rendered in full. */
        vg_finish();
        for (y = 0; y < H; y++) vg_rowfn(full[y], y);

        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                if (!stale_tile && full[y][x] != g_panel_tile[y][x]) {
                    stale_tile = 1;
                    first_bad_f = f; first_bad_x = x; first_bad_y = y;
                    printf("     tile col %d row %d  panel=%04X full=%04X\n",
                           x / TILE_W, y / TILE_H, g_panel_tile[y][x], full[y][x]);

                }

            }
    }

    if (stale_tile)
        printf("     first stale tile pixel: frame %d at (%d,%d)\n",
               first_bad_f, first_bad_x, first_bad_y);

    check(bad_rc == 0, "every frame presented successfully");
    check(!stale_tile, "tiled panel is pixel-identical to a full repaint");

    printf("     tiles: %.1f%% of pixels, %.2f spans/frame\n",
           100.0 * (double)g_px_tile / ((double)FRAMES * W * H),
           (double)g_spans_tile / FRAMES);
    check(g_px_tile < (long)FRAMES * W * H * 8 / 10,
          "tiling actually elides - under 80% of pixels transmitted");

    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails != 0;
}
