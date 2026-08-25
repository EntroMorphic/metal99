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
#include "elide.h"
#include "font.h"
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

/*
 * A SIMULATED PANEL.
 *
 * Every other check here asks what the MODEL says. This one asks the question
 * that actually matters: after a present, does the glass match the model?
 *
 * The panel only receives the marked rectangles, so if marking misses anything
 * the model changed, the difference persists on screen until something else
 * happens to cover it - which is exactly what an artifact is. Reported from
 * hardware as "text over the moving bar leaves debris while touching", and not
 * findable by reading the model, because the model was right the whole time.
 */
static uint16_t g_panel[SH8601_HEIGHT][W];

static void panel_apply(void)
{
    int i, y, x;
    for (i = 0; i < g_nmarks; i++) {
        for (y = g_marks[i].y0; y <= g_marks[i].y1; y++) {
            stub_render(y, g_row);
            for (x = g_marks[i].x0; x <= g_marks[i].x1; x++)
                g_panel[y][x] = g_row[x];
        }
    }
}

/* Rows where the glass disagrees with a full render of the model. */
static int panel_mismatch_rows(void)
{
    int y, x, bad = 0;
    for (y = 0; y < SH8601_HEIGHT; y++) {
        stub_render(y, g_row);
        for (x = 0; x < W; x++)
            if (g_panel[y][x] != g_row[x]) { bad++; break; }
    }
    return bad;
}

int main(void)
{
    const uint16_t BG = 0x1111u, FG = 0x2222u, C3 = 0x3333u;
    int x0, x1;
    uint32_t n;

    printf("gfx_test: metal99 run-model\n");
    gfx_init();
    /*
     * SAFETY NET OFF for the whole suite.
     *
     * The real elide is linked now, and its rolling resync adds four
     * full-width rows to every flush. That makes "this call transmits nothing"
     * untestable, and worse, it would SCRUB any marking bug the panel-match
     * check exists to find - the same masking that hid a marking leak on
     * hardware until the net was switched off (DESIGN.md 6.6k). Off here for
     * exactly that reason.
     */
    elide_set_resync(0u);
    (void)gfx_present();            /* hands the rowfn to the stub */

    /* ---- 1. THE CASE THE OLD MODEL COULD NOT EXPRESS ---- */
    gfx_solid(0, 100, BG);
    (void)gfx_present();            /* sync g_sent so the next diff is clean */
    stub_reset();
    n = gfx_rect(80, 10, 167, 20, FG);
    check(n == 11u, "centred rect changes exactly its 11 rows");
    check(band_is(15, 0, 79, BG),    "  columns left of a centred rect are bg");
    check(band_is(15, 80, 167, FG),  "  columns inside a centred rect are fg");
    check(band_is(15, 168, W - 1, BG), "  columns right of a centred rect are bg");
    check(band_is(5, 0, W - 1, BG),  "  rows outside the rect are untouched");

    /* ---- 2. the dirty extent is the COLUMNS, not the row ---- */
    (void)gfx_present();
    check(marked(15, &x0, &x1) && x0 == 80 && x1 == 167,
          "dirty extent is the rect's columns, not 0..367");

    /* ---- 3. redundant draw is fully elided ---- */
    stub_reset();
    n = gfx_rect(80, 10, 167, 20, FG);
    (void)gfx_present();
    check(n == 0u, "redrawing an identical rect changes 0 rows");
    check(g_nmarks == 0, "  and marks nothing");

    /* ---- 4. a rect of the same colour inside a solid field is a no-op ---- */
    gfx_solid(200, 210, BG);
    (void)gfx_present();
    stub_reset();
    n = gfx_rect(100, 200, 200, 210, BG);
    (void)gfx_present();
    check(n == 0u, "same-colour rect inside a solid field is elided");
    check(g_nmarks == 0, "  and transmits nothing");

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
    (void)gfx_present();
    check(marked(425, &x0, &x1) && x0 == 80 && x1 == 175,
          "a moved rect marks only the columns that changed");
    check((x1 - x0 + 1) < W / 2, "  which is under half the row");

    /* ---- 10. erase-then-draw costs only the NET change ----
     *
     * This is what present-time diffing buys. gfx used to diff at SET time
     * against the model in flight, so erasing a box and drawing it 4 px lower
     * took the overlapping rows FG -> BG -> FG and marked them twice: 92 rows
     * and 8,096 px for 8 rows and 704 px of real change, 11.5x. Diffing at
     * present against what the panel actually holds makes the intermediate
     * state free. */
    gfx_solid(0, 447, BG);
    (void)gfx_rect(136, 100, 223, 187, FG);
    (void)gfx_present();
    stub_reset();
    (void)gfx_rect(136, 100, 223, 187, BG);      /* erase */
    (void)gfx_rect(136, 104, 223, 191, FG);      /* draw 4px lower */
    (void)gfx_present();
    {
        int y, rows = 0, px = 0, a, b;
        for (y = 0; y < SH8601_HEIGHT; y++)
            if (marked(y, &a, &b)) { rows++; px += (b - a + 1); }
        check(rows == 8,  "erase-then-draw marks only the 8 net-changed rows");
        check(px == 8 * 88, "  and only their 704 pixels (was 8,096)");
        check(marked(100, &a, &b) && marked(103, &a, &b) &&
              marked(188, &a, &b) && marked(191, &a, &b),
              "  the rows that did change are all present");
        check(!marked(150, &a, &b),
              "  a row that went FG->BG->FG is not transmitted");
    }

    /* ---- 10b. gfx_split composes as two rects ---- */
    gfx_solid(440, 440, BG);
    (void)gfx_split(440, 440, FG, C3, 160);
    check(band_is(440, 0, 159, FG) && band_is(440, 160, W - 1, C3),
          "gfx_split renders as a left band and a right band");

    /* ---- 11. a change reverted before present costs nothing at all ---- */
    gfx_solid(0, 447, BG);
    (void)gfx_present();
    stub_reset();
    (void)gfx_rect(0, 40, 367, 60, FG);          /* draw ... */
    (void)gfx_rect(0, 40, 367, 60, BG);          /* ... and undo it */
    (void)gfx_present();
    check(g_nmarks == 0, "a change reverted before present transmits nothing");

    /* ---- 12. diff_extent edge cases ----
     *
     * The merge-walk over two run lists is the most load-bearing new code: it
     * decides what gets transmitted. Boundaries are where a two-pointer walk
     * goes wrong, so probe them directly. */
    {
        int a, b;

        gfx_solid(0, 447, BG); (void)gfx_present();
        stub_reset();
        (void)gfx_rect(0, 50, 7, 50, FG);            /* first cell only */
        (void)gfx_present();
        check(marked(50, &a, &b) && a == 0 && b == 7,
              "difference confined to the FIRST column cell");

        gfx_solid(0, 447, BG); (void)gfx_present();
        stub_reset();
        (void)gfx_rect(360, 51, 367, 51, FG);        /* last cell only */
        (void)gfx_present();
        check(marked(51, &a, &b) && a == 360 && b == 367,
              "difference confined to the LAST column cell");

        gfx_solid(0, 447, BG); (void)gfx_present();
        stub_reset();
        (void)gfx_rect(0, 52, 367, 52, FG);          /* whole row */
        (void)gfx_present();
        check(marked(52, &a, &b) && a == 0 && b == 367,
              "difference spanning the whole row");

        /* Differences at BOTH ends, identical in the middle. The extent is a
         * single range by construction, so it must cover both - conservative,
         * never incorrect. */
        gfx_solid(0, 447, BG); (void)gfx_present();
        stub_reset();
        (void)gfx_rect(0, 53, 7, 53, FG);
        (void)gfx_rect(360, 53, 367, 53, FG);
        (void)gfx_present();
        check(marked(53, &a, &b) && a == 0 && b == 367,
              "differences at both ends give one covering extent");

        /* A row already at the run cap, then changed again. */
        gfx_solid(0, 447, BG); (void)gfx_present();
        {
            int i;
            for (i = 0; i < GFX_MAX_RUNS + 4; i++)
                (void)gfx_rect((uint16_t)(i * 32), 54, (uint16_t)(i * 32 + 15), 54,
                               (uint16_t)(0x5000u + i));
        }
        (void)gfx_present();
        stub_reset();
        (void)gfx_rect(0, 54, 367, 54, C3);          /* flatten it again */
        (void)gfx_present();
        check(marked(54, &a, &b), "a row at the run cap still diffs and marks");
        check(band_is(54, 0, W - 1, C3), "  and renders correctly afterwards");
    }

    /* ---- 13. TEXT ---- */
    {
        const gfx_font *F = &share_mono_8x16;
        int a, b, y, ok, r, c;

        /* Glyph bits reach the panel in the right places. Compared against the
         * font data itself rather than a hand-copied pattern, so the test
         * cannot drift from the generator. */
        gfx_solid(0, 447, BG);
        (void)gfx_text(0, 0, 100, "A", FG, F);
        (void)gfx_present();
        ok = 1;
        for (r = 0; r < (int)F->h; r++) {
            uint8_t bits = F->bits[(('A' - F->first) * F->h) + r];
            stub_render(100 + r, g_row);
            for (c = 0; c < 8; c++) {
                uint16_t want = (bits & (0x80u >> c)) ? FG : BG;
                if (g_row[c] != want) ok = 0;
            }
        }
        check(ok, "a glyph renders exactly the font's bits");

        /* Transparent: clear bits keep whatever the runs drew. */
        gfx_solid(0, 447, BG);
        (void)gfx_rect(0, 100, 63, 115, C3);
        (void)gfx_text(0, 0, 100, "A", FG, F);
        (void)gfx_present();
        {
            uint8_t bits = F->bits[('A' - F->first) * F->h + 1];
            stub_render(101, g_row);
            ok = 1;
            for (c = 0; c < 8; c++)
                if (g_row[c] != ((bits & (0x80u >> c)) ? FG : C3)) ok = 0;
        }
        check(ok, "clear glyph bits keep the background beneath");

        /* Identical text is elided. */
        stub_reset();
        check(gfx_text(0, 0, 100, "A", FG, F) == 0,
              "setting identical text reports no change");
        (void)gfx_present();
        check(g_nmarks == 0, "  and transmits nothing");

        /* A changed string marks the label's rectangle, and only that. */
        stub_reset();
        (void)gfx_text(0, 0, 100, "B", FG, F);
        (void)gfx_present();
        /* Its own columns, NOT full width. Labels marked full rows for as long
         * as the span-boundary leak was unexplained; the leak was a too-narrow
         * AFIFO reset pulse (spi2_flush_afifo) and the 1.8x is back. */
        check(marked(100, &a, &b) && a == 0 && b < W - 1,
              "changed text marks its own columns, not the whole row");
        /* Expressed via the constant rather than a literal, so the padding can
         * change without silently making this assertion vacuous. */
        check(!marked(99, &a, &b) &&
              !marked(100 + (int)F->h + GFX_LABEL_PAD_BOTTOM, &a, &b),
              "  and only its rows, plus the bottom padding");
        check(marked(100 + (int)F->h + GFX_LABEL_PAD_BOTTOM - 1, &a, &b),
              "  the padded rows below the glyphs belong to the label");

        /* Moving marks old and new. */
        gfx_solid(0, 447, BG); (void)gfx_present();
        (void)gfx_text(0, 0, 200, "Hi", FG, F);
        (void)gfx_present();
        stub_reset();
        (void)gfx_text(0, 64, 200, "Hi", FG, F);
        (void)gfx_present();
        /* Old position at x=0 and new at x=64 are unioned into one row extent,
         * so it spans from the old left edge to the new right edge - and still
         * stops well short of the panel. */
        check(marked(205, &a, &b) && a == 0 && b < W - 1,
              "a moved label marks both positions, still not the whole row");

        /* Clearing repaints the background where it was. */
        stub_reset();
        (void)gfx_text_clear(0);
        (void)gfx_present();
        /* The label was moved to x=64 before this, so the clear marks exactly
         * the columns it occupied there - 64..79 for "Hi" - and nothing else.
         * Under the old full-width workaround this row cost 368 columns. */
        check(marked(205, &a, &b) && a == 64 && b < 128,
              "clearing a label marks only the columns where it was");
        stub_render(205, g_row);
        check(g_row[64] == BG && g_row[70] == BG,
              "  and the row renders as pure background again");

        /* x snaps down to the grid. */
        gfx_solid(0, 447, BG); (void)gfx_present();
        (void)gfx_text(1, 13, 300, "A", FG, F);
        (void)gfx_present();
        check(marked(300, &a, &b), "a label's rows are marked when it changes");

        /* Clipped at the right edge rather than overrunning the row. */
        gfx_solid(0, 447, BG); (void)gfx_present();
        (void)gfx_text(2, 360, 320, "ABCD", FG, F);
        (void)gfx_present();
        stub_render(321, g_row);
        ok = 1;
        for (y = 0; y < W; y++) if (y < 360 && g_row[y] != BG) ok = 0;
        check(ok, "a label running past the edge is clipped, not wrapped");

        gfx_text_clear(1); gfx_text_clear(2); (void)gfx_present();
    }

    /* ---- 14. the glass must match the model, frame after frame ----
     *
     * Reproduces the reported artifact: a moving bar, static title text, and a
     * label whose contents change every frame - the shape a touch readout over
     * an animation takes. */
    {
        const uint16_t BLACK = 0x0000u, ORANGE = 0xE003u, CYAN = 0x7F1Fu;
        char lbl[16];
        int frame, bar = 0, worst = 0;

        gfx_init();
        (void)gfx_present();
        (void)gfx_solid(0, SH8601_HEIGHT - 1, BLACK);
        (void)gfx_text(0, 16, 8, "metal99 60Hz", CYAN, &share_mono_16x32);
        stub_reset();
        (void)gfx_present();
        /* Seed the panel from a full render: the first present repaints all. */
        {
            int y, x;
            for (y = 0; y < SH8601_HEIGHT; y++) {
                stub_render(y, g_row);
                for (x = 0; x < W; x++) g_panel[y][x] = g_row[x];
            }
        }

        /* EXACTLY main.c's paced scene: 96-row bar stepping 4 px, a static
         * 12-char title at y=8, a 13-char label at y=56 whose contents change
         * every frame, and slot 2 cleared - the shape that leaves a remnant on
         * hardware. */
        for (frame = 0; frame < 400; frame++) {
            int m, k;
            bar = (bar + 4) % (SH8601_HEIGHT - 96);
            (void)gfx_solid(0, SH8601_HEIGHT - 1, BLACK);
            (void)gfx_solid((uint16_t)bar, (uint16_t)(bar + 95), ORANGE);
            (void)gfx_text(0, 16, 8, "metal99 60Hz", CYAN, &share_mono_16x32);
            /* "<id> X:nnn Y:nnn", 13 chars, as fmt_point produces */
            lbl[0] = '0'; lbl[1] = ' '; lbl[2] = 'X'; lbl[3] = ':';
            lbl[4] = (char)('0' + (frame / 100) % 10);
            lbl[5] = (char)('0' + (frame / 10) % 10);
            lbl[6] = (char)('0' + frame % 10);
            lbl[7] = ' '; lbl[8] = 'Y'; lbl[9] = ':';
            for (k = 10; k < 13; k++) lbl[k] = (char)('0' + (frame + k) % 10);
            lbl[13] = '\0';
            (void)gfx_text(1, 16, 56, lbl, CYAN, &share_mono_16x32);
            gfx_text_clear(2);

            stub_reset();
            (void)gfx_present();
            panel_apply();
            m = panel_mismatch_rows();
            if (m > worst) worst = m;
        }
        check(worst == 0, "panel matches the model after every frame");
        if (worst) printf("     %d row(s) on the glass disagree with the model\n", worst);
    }

    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails != 0;
}
