/*
 * RETIRED 2026-08-26 from metal99/src/vg.c + vg.h.
 *
 * ROW-GRANULAR ELISION for vector scenes. vg tracked which rows carried a
 * segment this frame and last, marked the union into elide, and let
 * elide_flush transmit the contiguous runs. Superseded by tile_present, which
 * does the same job at tile granularity and measured strictly better on the
 * same scene:
 *
 *     vg_present    88.7% of pixels   1.6 spans/frame   18.9 ms
 *     tile_present  27.6% of pixels    29 spans/frame   15.5 ms
 *
 * It was correct. game_test proved the elided panel pixel-identical to a full
 * repaint across 6000 frames, and three real defects were found and fixed
 * inside it along the way - a destructive vg_finish, a walk that consumed the
 * frame, and gridvoid presenting a blank frame on every death.
 *
 * WHAT IS NOT SAFE TO REUSE ARE THE TWO CONSTANTS. VG_MIN_RUN and
 * VG_MERGE_GAP were derived from the belief that SHORT SPANS put debris on the
 * glass - measured, reproducible, and genuinely fixed by padding runs and
 * merging across gaps. tile_present then ran 29 spans of EIGHT rows on the
 * same panel with no debris at all. Both observations stand and no account of
 * the transport explains both (docs/DESIGN.md 11.4). So these numbers work,
 * and nobody knows why. Treat them as a recorded result, not as knowledge.
 *
 * Kept because the approach may suit a scene whose lit rows are genuinely
 * clustered, where tiling's per-tile hashing would be the more expensive of
 * the two - and because the constants above are a live open question.
 */

/*
 * WHICH ROWS HAVE ANYTHING ON THEM - this frame and last.
 *
 * This is the whole of vg's contribution to elision. It does not diff pixels
 * and it does not keep a framebuffer; it records the one fact a vector scene
 * knows for free, which is that most rows are empty. Everything after that -
 * coalescing runs into spans, the rolling resync, retrying rows a failed flush
 * left dirty, the statistics - is elide's, unchanged.
 *
 * LAST FRAME'S ROWS MUST BE SENT TOO. A craft that moved off row 200 leaves
 * its pixels on the panel until something repaints them; the panel retains
 * what it was given. So the rows to transmit are (lit now UNION lit last
 * frame), and the second half of that union is what erases.
 */
/*
 * MINIMUM MARKED-RUN HEIGHT - an experiment, and possibly a fix.
 *
 * Measured over 6000 frames: every frame containing a burst produces at least
 * one span shorter than 8 rows, minimum 1, isolated in the empty sky with
 * large untransmitted gaps on both sides. Quiet frames produce one only 16% of
 * the time. The device reports debris ONLY when anomalies explode, and that is
 * the only structural difference those frames have.
 *
 * spanlab never reproduced it across four rounds, and this is why: it had
 * 1-row spans, but always ADJACENT to their neighbours, so the window's Y
 * address always advanced contiguously. It never made the panel jump.
 *
 * Padding a short run costs a handful of rows and makes the span ordinary. If
 * the debris goes away, the trigger is short and/or isolated windows, and this
 * is the shape of the fix.
 */
#define VG_MIN_RUN 48

/* Merge runs separated by fewer than this many clean rows: sending a few rows
 * that did not change is cheaper than another isolated window. */
#define VG_MERGE_GAP 64

#define LWORDS ((H + 31) / 32)
static uint32_t g_lit[LWORDS];       /* rows carrying a segment this frame */
static uint32_t g_lit_prev[LWORDS];  /* ...and last frame                  */
static int      g_presented;         /* has a full repaint happened yet?   */

/* Rows that must go out: something is on them now, or something was on them
 * last frame and has to be painted over. */
static int row_wanted(int y)
{
    uint32_t m = 1u << (y & 31);
    return ((g_lit[y >> 5] | g_lit_prev[y >> 5]) & m) != 0u;
}

int vg_present(void)
{
    int y, i, rc;

    /*
     * The first frame repaints everything. elide's model of the panel starts
     * out claiming the whole screen is dirty, but only if someone called
     * elide_init - and vg has no way to know whether an app did. Forcing it
     * here means a vector app is correct whether or not it also uses gfx.
     */
    if (!g_presented) { elide_reset(); g_presented = 1; }

    /*
     * Build the lit-row map HERE, not in vg_finish.
     *
     * It lived in vg_finish and cost every caller, including the ones that
     * present a full frame and never read it - which put ~0.7 ms on a path
     * that had none of the benefit and pushed a 40 Hz app into missing every
     * deadline. Work belongs to the function that needs it.
     */
    for (i = 0; i < LWORDS; i++) g_lit[i] = 0u;
    for (i = 0; i < g_nseg; i++) {
        int top = g_seg[i].ytop, bot = g_seg[i].ybot;
        /* vg_line clamps both ends to the panel, so this cannot run off the
         * bitmap - but it is indexing an array with numbers it did not choose,
         * which is the shape of bug this file has already had once. */
        for (y = top; y <= bot; y++) g_lit[y >> 5] |= 1u << (y & 31);
    }

    /*
     * FULL-WIDTH MARKS, deliberately, though elide_mark_rect is right there.
     *
     * Span-boundary debris scales with the number of spans and with variety in
     * their extents - elide coalesces adjacent rows only when their x-extents
     * match exactly, so mixed extents split one span into several and every
     * extra boundary is another chance to leak (spi2.h). Full-width rows all
     * share an extent and collapse into a handful of spans.
     *
     * It also costs almost nothing HERE: below the horizon a vector scene's
     * lit rows span nearly the whole width anyway, so a per-row x-extent would
     * save little and buy the debris risk with it. The win being collected is
     * the empty sky, and that is a row-granular fact. If a scene ever appears
     * whose lit rows are genuinely narrow, mark_rect is the upgrade and the
     * measurement should come first.
     */
    /*
     * One mark per contiguous run. Bridging short gaps to save span setups was
     * tried and measured: 3.1 spans per frame either way, and the worst frame
     * did not move off 25.5 ms. The fragmentation it was meant to fix is not
     * there - a vector scene's wanted rows come out in a few long runs, not
     * many short ones - so the heuristic was removed rather than kept as
     * complexity with a plausible story and no effect.
     */
    y = 0;
    while (y < H) {
        if (row_wanted(y)) {
            int y0 = y, y1;
            /* Absorb the next run too if the clean gap between them is small. */
            for (;;) {
                int probe;
                while (y < H && row_wanted(y)) y++;
                y1 = y - 1;
                probe = y;
                while (probe < H && probe - y < VG_MERGE_GAP && !row_wanted(probe)) probe++;
                if (probe < H && row_wanted(probe)) { y = probe; continue; }
                break;
            }
            /* Pad short runs outward, symmetrically, clamped to the panel. The
             * extra rows are rendered and sent normally, so the picture is
             * identical either way - only the window geometry changes. */
            while ((y1 - y0 + 1) < VG_MIN_RUN && (y0 > 0 || y1 < H - 1)) {
                if (y0 > 0)     y0--;
                if (y1 < H - 1) y1++;
            }
            elide_mark(y0, y1);
        } else {
            y++;
        }
    }

    rc = elide_flush(vg_rowfn);

    /*
     * Advance the history even if the flush failed. elide leaves failed rows
     * dirty and retries them, so they are not lost; keeping a stale g_lit_prev
     * as well would mark them twice and hide the failure rather than fix it.
     */
    for (i = 0; i < LWORDS; i++) g_lit_prev[i] = g_lit[i];
    return rc;
}
