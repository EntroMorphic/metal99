/*
 * Assertions for the vector rasteriser.
 *
 * The picture in out_vg.png catches gross errors; these catch the ones an eye
 * slides over. The important one is CONTINUITY: a line with a one-pixel gap
 * every few rows still looks like a line at a glance, and looks like rain once
 * it is moving.
 */
#include <stdio.h>
#include "vg.h"
#include "trig.h"
#include "sh8601.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT
#define INK 0x1F7Fu

static int g_fails;
static uint16_t g_row[W];

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_fails++;
}

/* Rows on which a segment painted at least one pixel, and the x-extent. */
static int scan(int *first_row, int *last_row, int *minx, int *maxx, int *gaps)
{
    int y, x, painted = 0, lastpaint = -1;
    *first_row = -1; *last_row = -1; *minx = W; *maxx = -1; *gaps = 0;
    vg_finish();
    for (y = 0; y < H; y++) {
        int any = 0;
        vg_rowfn(g_row, y);
        for (x = 0; x < W; x++)
            if (g_row[x] != 0x0000u) {
                any = 1; painted++;
                if (x < *minx) *minx = x;
                if (x > *maxx) *maxx = x;
            }
        if (any) {
            if (*first_row < 0) *first_row = y;
            if (lastpaint >= 0 && y - lastpaint > 1) (*gaps)++;
            *last_row = y; lastpaint = y;
        }
    }
    return painted;
}

int main(void)
{
    int fr, lr, mn, mx, gaps, n, i;

    printf("vg_test: metal99 vector rasteriser\n");
    vg_set_bg(0x0000u);

    /* ---- exact horizontal ---- */
    vg_begin(); vg_line(20, 100, 60, 100, INK);
    n = scan(&fr, &lr, &mn, &mx, &gaps);
    check(fr == 100 && lr == 100, "a horizontal line occupies exactly one row");
    check(mn == 20 && mx == 60,   "  and exactly the requested columns");
    check(n == 41,                "  41 pixels for a 41-pixel span, no more");

    /* ---- exact vertical ---- */
    vg_begin(); vg_line(50, 10, 50, 200, INK);
    n = scan(&fr, &lr, &mn, &mx, &gaps);
    check(fr == 10 && lr == 200,  "a vertical line spans exactly its rows");
    check(mn == 50 && mx == 50,   "  and occupies exactly one column");
    check(gaps == 0,              "  with no gaps");

    /* ---- diagonals at every slope are CONTINUOUS ---- */
    {
        int worst = 0, a;
        for (a = 0; a < TRIG_FULL; a += 4) {
            vg_begin();
            vg_line(184, 224,
                    184 + (int)((icos(a) * 150) >> 16),
                    224 + (int)((isin(a) * 150) >> 16), INK);
            (void)scan(&fr, &lr, &mn, &mx, &gaps);
            if (gaps > worst) worst = gaps;
        }
        check(worst == 0, "no slope produces a row gap (64 angles tested)");
    }

    /* ---- clipping ---- */
    vg_begin();
    vg_line(-500, 50, 900, 60, INK);          /* off both sides   */
    vg_line(100, -300, 120, 900, INK);        /* off top and bottom */
    n = scan(&fr, &lr, &mn, &mx, &gaps);
    check(mn >= 0 && mx <= W - 1, "clipped segments never write outside the row");
    check(fr >= 0 && lr <= H - 1, "  nor outside the screen vertically");

    /* ---- a fully off-screen segment draws nothing ---- */
    vg_begin(); vg_line(-50, -50, -10, -10, INK);
    n = scan(&fr, &lr, &mn, &mx, &gaps);
    check(n == 0, "a segment entirely off-screen paints nothing");

    /* ---- reversed endpoints draw the same line ---- */
    {
        int pa, pb;
        vg_begin(); vg_line(30, 40, 200, 300, INK);
        pa = scan(&fr, &lr, &mn, &mx, &gaps);
        vg_begin(); vg_line(200, 300, 30, 40, INK);
        pb = scan(&fr, &lr, &mn, &mx, &gaps);
        check(pa == pb && pa > 0, "endpoint order does not change the line");
    }

    /* ---- overflow is counted, not silent ---- */
    vg_begin();
    for (i = 0; i < VG_MAX_SEGS + 20; i++) vg_line(0, i % H, 10, i % H, INK);
    check(vg_overflow() == 20u, "segments past the cap are counted, not dropped silently");

    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails != 0;
}
