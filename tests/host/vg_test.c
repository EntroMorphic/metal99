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


/*
 * elide stubs. vg.c calls into elide from vg_present(); this file tests the
 * RASTERISER, and linking the real elide would drag in sh8601, spi2 and gdma
 * to test none of them. vg_present's integration is covered on device and by
 * the sparse-walk equivalence below, which is the property it depends on.
 */
void elide_reset(void) {}
void elide_mark(int y0, int y1) { (void)y0; (void)y1; }
int  elide_flush(void (*rowfn)(uint16_t *row, int y)) { (void)rowfn; return 0; }

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


    /*
     * SPARSE WALK EQUIVALENCE - the property vg_present() is built on.
     *
     * Elision calls vg_rowfn only for the rows it intends to transmit, so the
     * rasteriser sees an increasing but GAPPY sequence and has to advance the
     * active-edge list across the rows it never renders. If the catch-up were
     * wrong, skipped rows would corrupt every row after them, and the picture
     * would be wrong only in the elided case - which on hardware looks like
     * "artifacts" and gets blamed on the transport. It has been blamed on the
     * transport before.
     *
     * So: render a scene fully, then render it again visiting only a subset,
     * and require the visited rows to be identical pixel for pixel.
     */
    {
        static uint16_t full[H][W], sparse[W];
        int y, bad = 0, visited = 0, step;

        for (step = 2; step <= 7; step++) {
            vg_begin();
            vg_line(20, 5, 340, 430, INK);       /* steep diagonal   */
            vg_line(340, 30, 20, 400, INK);      /* the other way    */
            vg_line(0, 220, 367, 220, INK);      /* flat             */
            vg_line(180, 0, 184, 447, INK);      /* near-vertical    */
            vg_finish();
            for (y = 0; y < H; y++) vg_rowfn(full[y], y);

            vg_begin();
            vg_line(20, 5, 340, 430, INK);
            vg_line(340, 30, 20, 400, INK);
            vg_line(0, 220, 367, 220, INK);
            vg_line(180, 0, 184, 447, INK);
            vg_finish();
            for (y = 0; y < H; y += step) {
                int x;
                vg_rowfn(sparse, y);
                visited++;
                for (x = 0; x < W; x++)
                    if (sparse[x] != full[y][x]) { bad++; break; }
            }
        }
        check(bad == 0 && visited > 500,
              "sparse row walk renders identically to the full walk");
    }

    /* A single row rendered alone must still be right: the catch-up has to
     * cross every preceding row in one go. */
    {
        static uint16_t full[H][W], one[W];
        int y, bad = 0, x;
        vg_begin();
        vg_line(10, 0, 350, 447, INK);
        vg_line(350, 0, 10, 447, INK);
        vg_finish();
        for (y = 0; y < H; y++) vg_rowfn(full[y], y);

        for (y = 400; y < 405; y++) {
            vg_begin();
            vg_line(10, 0, 350, 447, INK);
            vg_line(350, 0, 10, 447, INK);
            vg_finish();
            vg_rowfn(one, y);
            for (x = 0; x < W; x++) if (one[x] != full[y][x]) bad++;
        }
        check(bad == 0, "a row rendered in isolation matches the full walk");
    }

    /* A row with nothing on it is exactly background - this is what makes it
     * safe for elision to skip it. */
    {
        static uint16_t row[W];
        int x, bad = 0;
        vg_set_bg(0x1234u);
        vg_begin(); vg_line(0, 300, 367, 300, INK); vg_finish();
        vg_rowfn(row, 100);
        for (x = 0; x < W; x++) if (row[x] != 0x1234u) bad++;
        vg_set_bg(0u);
        check(bad == 0, "an unlit row renders as pure background");
    }

    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails != 0;
}
