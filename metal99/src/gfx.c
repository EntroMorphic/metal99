#include <stddef.h>
#include "gfx.h"
#include "io.h"
#include "vec.h"
#include "elide.h"
#include "spi2.h"

#define ROWS        SH8601_HEIGHT
#define ROW_VECTORS (SH8601_WIDTH * 2 / VEC_BYTES)

static gfx_row   g_model[ROWS];      /* what each row SHOULD look like */
static gfx_stats g_stats;

/* Rows whose descriptor actually differed since the last present. Accumulated
 * here rather than returned-and-forgotten: gfx_stats.rows_changed was declared
 * and documented but never assigned, so it read 0 forever while callers had no
 * way to tell an elided frame from a broken one. */
static uint32_t  g_changed_pending;

void gfx_init(void)
{
    uint32_t i;
    for (i = 0u; i < (uint32_t)ROWS; i++) {
        g_model[i].kind = GFX_ROW_SOLID;
        g_model[i].a = 0u; g_model[i].b = 0u; g_model[i].x = 0u;
    }
    g_changed_pending = 0u;
    elide_init();          /* marks everything dirty: first present repaints */
}

void gfx_invalidate(void) { elide_reset(); }

static int row_differs(const gfx_row *m, const gfx_row *n)
{
    return (m->kind != n->kind) || (m->a != n->a)
        || (m->b != n->b) || (m->x != n->x);
}

/* Single place where the model is written, so change detection cannot be
 * bypassed by a caller that forgets to mark. */
static uint32_t set_rows(uint16_t y0, uint16_t y1, const gfx_row *want)
{
    uint32_t changed = 0u;
    int y;

    if (y1 >= (uint16_t)ROWS) y1 = (uint16_t)(ROWS - 1);
    for (y = (int)y0; y <= (int)y1; y++) {
        if (row_differs(&g_model[y], want)) {
            g_model[y] = *want;
            elide_mark(y, y);
            changed++;
        }
    }
    g_changed_pending += changed;
    return changed;
}

uint32_t gfx_solid(uint16_t y0, uint16_t y1, uint16_t colour)
{
    gfx_row w;
    w.kind = GFX_ROW_SOLID; w.a = colour; w.b = colour; w.x = 0u;
    return set_rows(y0, y1, &w);
}

uint32_t gfx_split(uint16_t y0, uint16_t y1, uint16_t left, uint16_t right,
                   uint16_t x)
{
    gfx_row w;

    /*
     * SNAP THE SPLIT TO WHAT CAN ACTUALLY BE RENDERED.
     *
     * gfx_rowfn draws the row with two vec_fill16 calls, and vec_fill16 works
     * in whole 128-bit vectors - VEC_PIX16 (8) pixels at a time. A split at
     * column 100 therefore renders at 96 no matter what the model says.
     *
     * Storing the caller's exact x meant the model claimed a precision the
     * renderer did not have: x=100 and x=103 compared as DIFFERENT, so the row
     * was marked dirty and retransmitted, and the panel received bytes
     * identical to the ones already on it. That is the model-drifts-from-render
     * bug this whole layer exists to make impossible, reintroduced one level up.
     *
     * Quantising here keeps the two exactly in step: what the model records is
     * what the renderer will produce, so a no-op change is correctly elided.
     * Splitting inside a vector would need a scalar tail, which the project's
     * no-scalar rule forbids on the render path.
     */
    if (x > (uint16_t)SH8601_WIDTH) x = (uint16_t)SH8601_WIDTH;
    x = (uint16_t)(x & ~(uint16_t)(VEC_PIX16 - 1));

    w.kind = GFX_ROW_SPLIT; w.a = left; w.b = right; w.x = x;
    return set_rows(y0, y1, &w);
}

/* Renders a row from the model. This is the ONLY interpreter of gfx_row, so
 * transmitted pixels and the change-detection model cannot drift apart. */
static void gfx_rowfn(uint16_t *row, int y)
{
    const gfx_row *m = &g_model[y];

    if (m->kind == GFX_ROW_SPLIT) {
        /* Exact by construction: gfx_split() already snapped x to a multiple
         * of VEC_PIX16, so this division loses nothing. */
        uint32_t lv = (uint32_t)m->x / (uint32_t)VEC_PIX16;
        if (lv > (uint32_t)ROW_VECTORS) lv = (uint32_t)ROW_VECTORS;
        if (lv > 0u) vec_fill16(row, m->a, lv);
        if (lv < (uint32_t)ROW_VECTORS) {
            vec_fill16(row + (lv * (uint32_t)VEC_PIX16), m->b,
                       (uint32_t)ROW_VECTORS - lv);
        }
    } else {
        vec_fill16(row, m->a, (uint32_t)ROW_VECTORS);
    }
}

int gfx_present(void)
{
    uint32_t t0 = cpu_cycles();
    int rc;
    const elide_stats *e;

    rc = elide_flush(gfx_rowfn);
    e  = elide_last();

    g_stats.rows_changed = g_changed_pending;
    g_stats.rows_sent    = e->rows_sent;
    g_stats.spans        = e->spans;
    g_stats.cycles       = cpu_cycles() - t0;

    /* Only clear once the rows have actually gone out. elide_flush() leaves
     * failed rows dirty for the next attempt, and the change count must stay
     * consistent with that or a caller sees "0 changed" for a frame that never
     * reached the panel. */
    if (rc == SPI2_OK) g_changed_pending = 0u;
    return rc;
}

const gfx_stats *gfx_last(void) { return &g_stats; }
