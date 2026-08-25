#include <stddef.h>
#include "vg.h"
#include "vec.h"
#include "sh8601.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT
#define ROW_VECTORS (W * 2 / VEC_BYTES)

typedef struct {
    int16_t  ybot;        /* last row this segment appears on, inclusive */
    int32_t  x;           /* 16.16 x at the current row                  */
    int32_t  dxdy;        /* 16.16 x step per row (or full dx if flat)   */
    uint16_t colour;
    int16_t  next;        /* index into g_seg, -1 = end of list          */
} vseg;

static vseg     g_seg[VG_MAX_SEGS];
static int      g_nseg;
static int16_t  g_bucket[H];      /* head index of segments starting on a row */
static int16_t  g_active;         /* head of the active list                 */
static int      g_cur;            /* next row vg_rowfn expects               */
static uint16_t g_bg;
static uint32_t g_over;

void vg_set_bg(uint16_t colour) { g_bg = colour; }
uint32_t vg_overflow(void)      { return g_over; }

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

    /* Bucketed by top row; vg_finish links them. Stash the row in `next` for
     * now - it is not a list index until then. */
    s->next = (int16_t)y0;
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
        int top = g_seg[i].next;
        g_seg[i].next = g_bucket[top];
        g_bucket[top] = (int16_t)i;
    }
    g_active = -1;
    g_cur    = 0;
}

void vg_rowfn(uint16_t *row, int y)
{
    int16_t i, prev;

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
            g_seg[j].x += g_seg[j].dxdy;
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
        int xa = (int)(s->x >> 16);
        int xb = (int)((s->x + s->dxdy) >> 16);
        int t, x;

        if (xa > xb) { t = xa; xa = xb; xb = t; }
        if (xa < 0) xa = 0;
        if (xb > W - 1) xb = W - 1;
        /* Scalar per pixel, deliberately: a span here is typically one or two
         * pixels wide, and a vector call would cost more than it saves. The
         * bulk fill above is where the vector unit earns its place. */
        for (x = xa; x <= xb; x++) row[x] = s->colour;

        s->x += s->dxdy;
        if (s->ybot <= y) {
            if (prev < 0) g_active = s->next;
            else g_seg[prev].next = s->next;
        } else prev = i;
        i = s->next;
    }
    g_cur = y + 1;
}
