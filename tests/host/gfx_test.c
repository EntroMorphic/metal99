/*
 * Assertions for the gfx run model.
 *
 * The headline case is the one the previous model could NOT express: a
 * rectangle that does not touch a screen edge. A row crossing a centred box is
 * bg|fg|bg - two transitions - and GFX_ROW_SPLIT offered one. Everything else
 * here guards the machinery that makes that work: run insertion, the outward
 * grid snap, per-column diffing, and the overflow cap.
 *
 * Links the real metal99/src/gfx.c. Only the layers BELOW it are stubbed.
 *
 * Run: make -C tests/host test
 */
#include <stdio.h>
#include <string.h>
#include "gfx.h"
#include "gfx_stubs.h"

#define W SH8601_WIDTH
static int g_fails;
static uint16_t g_row[W];

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_fails++;
}

/* Render row y and confirm every column in [x0,x1] is `c`. */
static int band_is(int y, int x0, int x1, uint16_t c)
{
    int x;
    stub_render(y, g_row);
    for (x = x0; x <= x1; x++) if (g_row[x] != c) return 0;
    return 1;
}

int main(void)
{
    const uint16_t BG = 0x1111u, FG = 0x2222u, C3 = 0x3333u;
    int x0, x1;
    uint32_t n;

    printf("gfx_test: metal99 run-model\n");
    gfx_init();
    (void)gfx_present();            /* hands the rowfn to the stub */

    /* ---- 1. THE CASE THE OLD MODEL COULD NOT EXPRESS ---- */
    gfx_solid(0, 100, BG);
    stub_reset();
    n = gfx_rect(80, 10, 167, 20, FG);
    check(n == 11u, "centred rect marks exactly its 11 rows");
    check(band_is(15, 0, 79, BG),    "  columns left of a centred rect are bg");
    check(band_is(15, 80, 167, FG),  "  columns inside a centred rect are fg");
    check(band_is(15, 168, W - 1, BG), "  columns right of a centred rect are bg");
    check(band_is(5, 0, W - 1, BG),  "  rows outside the rect are untouched");

    /* ---- 2. the dirty extent is the COLUMNS, not the row ---- */
    check(marked(15, &x0, &x1) && x0 == 80 && x1 == 167,
          "dirty extent is the rect's columns, not 0..367");

    /* ---- 3. redundant draw is fully elided ---- */
    stub_reset();
    n = gfx_rect(80, 10, 167, 20, FG);
    check(n == 0u, "redrawing an identical rect changes 0 rows");
    check(g_nmarks == 0, "  and marks nothing");

    /* ---- 4. a rect of the same colour inside a solid field is a no-op ---- */
    gfx_solid(200, 210, BG);
    stub_reset();
    n = gfx_rect(100, 200, 200, 210, BG);
    check(n == 0u, "same-colour rect inside a solid field is elided");

    /* ---- 5. outward grid snap: cover, never clip ---- */
    gfx_solid(300, 300, BG);
    stub_reset();
    (void)gfx_rect(3, 300, 12, 300, FG);
    check(band_is(300, 3, 12, FG),   "requested columns are all covered");
    check(band_is(300, 0, 15, FG),   "  snapped outward to the 8px grid");
    check(band_is(300, 16, W - 1, BG), "  and no further");

    /* ---- 6. three stacked rects compose (the old model held one edge) ---- */
    gfx_solid(400, 400, BG);
    (void)gfx_rect(40,  400, 79,  400, FG);
    (void)gfx_rect(160, 400, 199, 400, C3);
    check(band_is(400, 0, 39, BG) && band_is(400, 40, 79, FG) &&
          band_is(400, 80, 159, BG) && band_is(400, 160, 199, C3) &&
          band_is(400, 200, W - 1, BG),
          "three disjoint rects in one row all render");

    /* ---- 7. overlap: later rect wins ---- */
    gfx_solid(401, 401, BG);
    (void)gfx_rect(40, 401, 119, 401, FG);
    (void)gfx_rect(80, 401, 159, 401, C3);
    check(band_is(401, 40, 79, FG) && band_is(401, 80, 159, C3),
          "an overlapping rect overwrites, and the seam is exact");

    /* ---- 8. overflow is bounded and COUNTED, not silent ---- */
    gfx_solid(402, 402, BG);
    {
        int i;
        for (i = 0; i < 12; i++)
            (void)gfx_rect((uint16_t)(i * 24), 402, (uint16_t)(i * 24 + 7), 402,
                           (uint16_t)(0x4000u + i));
    }
    (void)gfx_present();
    check(gfx_last()->run_overflows > 0u,
          "exceeding GFX_MAX_RUNS is reported, not absorbed silently");

    /* ---- 9. a moving element marks only what moved ---- */
    gfx_solid(420, 430, BG);
    (void)gfx_rect(80, 420, 167, 430, FG);
    (void)gfx_present();
    stub_reset();
    (void)gfx_rect(80, 420, 167, 430, BG);      /* erase */
    (void)gfx_rect(88, 420, 175, 430, FG);      /* redraw 8px right */
    check(marked(425, &x0, &x1) && x0 == 80 && x1 == 175,
          "a moved rect marks only the columns that changed");
    check((x1 - x0 + 1) < W / 2, "  which is under half the row");

    /* ---- 10. KNOWN LIMIT: erase-then-draw marks the overlap twice ----
     *
     * gfx diffs against the CURRENT model, not against what was last
     * presented. An erase followed by a draw takes the overlapping rows
     * FG -> BG -> FG: net unchanged, but both transitions mark them, so they
     * are transmitted for nothing.
     *
     * Marking is a strict SUPERSET of what changed, so this is a waste, never a
     * correctness bug - the panel is right, we just paid too much. Asserted as
     * a superset here, with the cost printed, so the day present-time diffing
     * lands the improvement is visible rather than assumed. */
    gfx_solid(0, 447, BG);
    (void)gfx_rect(136, 100, 223, 187, FG);
    (void)gfx_present();
    stub_reset();
    (void)gfx_rect(136, 100, 223, 187, BG);      /* erase */
    (void)gfx_rect(136, 104, 223, 191, FG);      /* draw 4px lower */
    {
        int y, rows = 0, px = 0, a, b;
        for (y = 0; y < SH8601_HEIGHT; y++)
            if (marked(y, &a, &b)) { rows++; px += (b - a + 1); }
        /* the 8 rows that NET changed must all be marked */
        check(marked(100, &a, &b) && marked(103, &a, &b) &&
              marked(188, &a, &b) && marked(191, &a, &b),
              "every net-changed row is marked (superset, so correct)");
        printf("     note: %d rows / %d px marked for %d rows / %d px of real\n"
               "           change - %.1fx, awaiting present-time diffing\n",
               rows, px, 8, 8 * 88, (double)px / (8 * 88));
    }

    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails != 0;
}
