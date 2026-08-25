#include <stddef.h>
#include "vg.h"
#include "vec.h"
#include "sh8601.h"
#include "elide.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT
#define ROW_VECTORS (W * 2 / VEC_BYTES)

typedef struct {
    int16_t  ybot;        /* last row this segment appears on, inclusive */
    int16_t  ytop;        /* first row, inclusive                        */
    int32_t  x;           /* 16.16 x at the current row                  */
    int32_t  dxdy;        /* 16.16 x step per row (or full dx if flat)   */
    uint16_t colour;
    int16_t  next;        /* index into g_seg, -1 = end of list          */
} vseg;
/*
 * ytop used to be stashed in `next` until vg_finish turned it into a list
 * index. That made vg_finish DESTRUCTIVE: calling it twice read a list index
 * as a row number, and once the lit-row map was added that became an
 * out-of-bounds write on g_lit[-1], caught by the sanitiser the first time a
 * test re-finished a frame to render it a second way. Storing the row costs
 * nothing - it lands in padding the struct already had, which a static assert
 * below keeps true - and vg_finish is now idempotent, which is what a test
 * that wants to render the same frame twice actually needs.
 */
typedef char vg_seg_still_fits[(sizeof(vseg) <= 16) ? 1 : -1];

static vseg     g_seg[VG_MAX_SEGS];
/*
 * The running x of each active segment, 16.16, reset by vg_finish from the
 * segment's starting x.
 *
 * It used to live in g_seg[].x, which vg_rowfn advanced in place - so a walk
 * CONSUMED the frame and a second walk rendered the segments from wherever the
 * first one left them, silently and wrongly. Nothing on the device re-renders
 * a frame, so this never showed there; it showed the moment a test tried to
 * render one frame two ways to check them against each other, which is the
 * only way to verify that eliding rows changes nothing about the picture.
 * Same memory traffic on the hot path, one array instead of one field.
 */
static int32_t  g_x[VG_MAX_SEGS];
static int      g_nseg;
static int16_t  g_bucket[H];      /* head index of segments starting on a row */
static int16_t  g_active;         /* head of the active list                 */
static int      g_cur;            /* next row vg_rowfn expects               */
static uint16_t g_bg;
static uint32_t g_over;

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
#define LWORDS ((H + 31) / 32)
static uint32_t g_lit[LWORDS];       /* rows carrying a segment this frame */
static uint32_t g_lit_prev[LWORDS];  /* ...and last frame                  */
static int      g_presented;         /* has a full repaint happened yet?   */

void vg_set_bg(uint16_t colour) { g_bg = colour; }
uint32_t vg_overflow(void)      { return g_over; }
int      vg_count(void)         { return g_nseg; }

void vg_begin(void)
{
    g_nseg = 0;
    g_over = 0u;
}

int vg_line(int x0, int y0, int x1, int y1, uint16_t colour)
{
    vseg *s;
    int dy;

    if (g_nseg >= VG_MAX_SEGS) { g_over++; return 0; }

    /* Normalise downward: the walk only ever goes one way. */
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 < 0 || y0 > H - 1) return 1;          /* entirely off-screen */

    s  = &g_seg[g_nseg];
    dy = y1 - y0;

    if (dy == 0) {
        /* Flat: one row, and dxdy carries the whole run. The span formula
         * below then draws it without a special case. */
        s->dxdy = (int32_t)(x1 - x0) << 16;
        s->x    = (int32_t)x0 << 16;
    } else {
        s->dxdy = (((int32_t)(x1 - x0)) << 16) / dy;
        s->x    = (int32_t)x0 << 16;
        if (y0 < 0) {                            /* clip the top */
            s->x += s->dxdy * (int32_t)(-y0);
            y0 = 0;
        }
    }
    s->ybot   = (int16_t)((y1 > H - 1) ? (H - 1) : y1);
    s->colour = colour;

    s->ytop = (int16_t)y0;      /* bucketed by this; vg_finish links `next` */
    s->next = -1;
    g_nseg++;
    return 1;
}

void vg_finish(void)
{
    int i, y;

    for (y = 0; y < H; y++) g_bucket[y] = -1;
    for (i = 0; i < LWORDS; i++) g_lit[i] = 0u;
    /* Reverse order so the bucket ends up in insertion order; the picture does
     * not depend on it, but a stable order makes a rendered frame reproducible
     * and therefore comparable between runs. */
    for (i = g_nseg - 1; i >= 0; i--) {
        int top = g_seg[i].ytop;
        int bot = g_seg[i].ybot;
        /* vg_line clamps both ends to the panel, so this cannot run off the
         * bitmap - but it is indexing an array with numbers it did not choose,
         * which is the shape of bug this file has already had once. */
        for (y = top; y <= bot; y++) g_lit[y >> 5] |= 1u << (y & 31);
        g_seg[i].next = g_bucket[top];
        g_bucket[top] = (int16_t)i;
    }
    for (i = 0; i < g_nseg; i++) g_x[i] = g_seg[i].x;
    g_active = -1;
    g_cur    = 0;
}

void vg_rowfn(uint16_t *row, int y)
{
    int16_t i, prev;

    /*
     * BOUNDS FIRST. y indexes g_bucket, and it arrives from whoever is driving
     * the transport - sh8601_write_frame today, a span walk tomorrow. The
     * header asks for rows in range and in order; asking is not enforcing, and
     * an out-of-range read here is a rasteriser indexing an array with a number
     * it did not choose.
     */
    if (y < 0 || y >= H) return;

    /* Background first. Vector scenes are mostly unlit, so this is the bulk of
     * the work and it goes through the vector unit like every other fill. */
    vec_fill16(row, g_bg, ROW_VECTORS);

    /* Rows must arrive in order: the active list is a forward walk and cannot
     * be rewound. Skipping ahead would silently drop every segment in between,
     * so catch up rather than render a wrong row. */
    while (g_cur < y) {
        int16_t j = g_bucket[g_cur], nx;
        while (j >= 0) { nx = g_seg[j].next; g_seg[j].next = g_active;
                         g_active = j; j = nx; }
        j = g_active; prev = -1;
        while (j >= 0) {
            g_x[j] += g_seg[j].dxdy;
            if (g_seg[j].ybot <= g_cur) {
                if (prev < 0) g_active = g_seg[j].next;
                else g_seg[prev].next = g_seg[j].next;
            } else prev = j;
            j = g_seg[j].next;
        }
        g_cur++;
    }
    if (y < g_cur) return;                      /* already passed: nothing */

    /* Admit segments that start here. */
    { int16_t j = g_bucket[y], nx;
      while (j >= 0) { nx = g_seg[j].next; g_seg[j].next = g_active;
                       g_active = j; j = nx; } }

    /* Draw each active segment's span across this row, then step and retire. */
    i = g_active; prev = -1;
    while (i >= 0) {
        vseg *s = &g_seg[i];
        int xa = (int)(g_x[i] >> 16);
        int xb = (int)((g_x[i] + s->dxdy) >> 16);
        int t, x;

        if (xa > xb) { t = xa; xa = xb; xb = t; }
        if (xa < 0) xa = 0;
        if (xb > W - 1) xb = W - 1;
        /* Scalar per pixel, deliberately: a span here is typically one or two
         * pixels wide, and a vector call would cost more than it saves. The
         * bulk fill above is where the vector unit earns its place. */
        for (x = xa; x <= xb; x++) row[x] = s->colour;

        g_x[i] += s->dxdy;
        if (s->ybot <= y) {
            if (prev < 0) g_active = s->next;
            else g_seg[prev].next = s->next;
        } else prev = i;
        i = s->next;
    }
    g_cur = y + 1;
}

/* Rows that must go out: something is on them now, or something was on them
 * last frame and has to be painted over. */
static int row_wanted(int y)
{
    uint32_t m = 1u << (y & 31);
    return ((g_lit[y >> 5] | g_lit_prev[y >> 5]) & m) != 0u;
}

int vg_present(void)
{
    int y = 0, i, rc;

    /*
     * The first frame repaints everything. elide's model of the panel starts
     * out claiming the whole screen is dirty, but only if someone called
     * elide_init - and vg has no way to know whether an app did. Forcing it
     * here means a vector app is correct whether or not it also uses gfx.
     */
    if (!g_presented) { elide_reset(); g_presented = 1; }

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
    while (y < H) {
        if (row_wanted(y)) {
            int y0 = y;
            while (y < H && row_wanted(y)) y++;
            elide_mark(y0, y - 1);
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
