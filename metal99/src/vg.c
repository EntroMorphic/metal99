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
    int32_t  x;           /* 16.16 x at the segment's FIRST row          */
    int32_t  xr;          /* 16.16 x at the row being walked now          */
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
 *
 * `xr` is the same story with a price. The walk used to advance `x` in place,
 * which CONSUMED the frame: a second walk drew every segment from wherever the
 * first left it. Invisible on device, where nothing re-renders a frame, and
 * fatal to the only test that can prove eliding rows changes nothing. Holding
 * the running value in a separate ARRAY fixed correctness but cost an extra
 * base register in the innermost loop - 0.4 ms a frame, enough to push a 40 Hz
 * app past its deadline. As a field it is one deref through a pointer the loop
 * already holds. The struct grows 16 -> 20 bytes, 10 KB of BSS for the segment
 * pool, which this memory map has in abundance and that deadline does not.
 */
typedef char vg_seg_still_fits[(sizeof(vseg) <= 20) ? 1 : -1];

static vseg     g_seg[VG_MAX_SEGS];

static int      g_nseg;
static int16_t  g_bucket[H];      /* head index of segments starting on a row */
static int16_t  g_active;         /* head of the active list                 */
static int      g_cur;            /* next row vg_rowfn expects               */
static uint16_t g_bg;
static uint32_t g_over;


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
    /* Reverse order so the bucket ends up in insertion order; the picture does
     * not depend on it, but a stable order makes a rendered frame reproducible
     * and therefore comparable between runs. */
    for (i = g_nseg - 1; i >= 0; i--) {
        int top = g_seg[i].ytop;
        g_seg[i].next = g_bucket[top];
        g_bucket[top] = (int16_t)i;
    }
    for (i = 0; i < g_nseg; i++) g_seg[i].xr = g_seg[i].x;
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
            g_seg[j].xr += g_seg[j].dxdy;
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
        int xa = (int)(s->xr >> 16);
        int xb = (int)((s->xr + s->dxdy) >> 16);
        int t, x;

        if (xa > xb) { t = xa; xa = xb; xb = t; }
        if (xa < 0) xa = 0;
        if (xb > W - 1) xb = W - 1;
        /* Scalar per pixel, deliberately: a span here is typically one or two
         * pixels wide, and a vector call would cost more than it saves. The
         * bulk fill above is where the vector unit earns its place. */
        for (x = xa; x <= xb; x++) row[x] = s->colour;

        s->xr += s->dxdy;
        if (s->ybot <= y) {
            if (prev < 0) g_active = s->next;
            else g_seg[prev].next = s->next;
        } else prev = i;
        i = s->next;
    }
    g_cur = y + 1;
}

