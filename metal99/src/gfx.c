#include <stddef.h>
#include "gfx.h"
#include "io.h"
#include "vec.h"
#include "elide.h"
#include "spi2.h"

#define ROWS        SH8601_HEIGHT
#define ROW_VECTORS (SH8601_WIDTH * 2 / VEC_BYTES)

/*
 * g_model  what the caller has described
 * g_sent   what the panel last actually received
 *
 * The pair is the whole point: dirtiness is derived at PRESENT time by diffing
 * them, so intermediate states a caller passes through cost nothing. See gfx.h.
 */
static gfx_row VEC_ALIGN g_model[ROWS];
static gfx_row VEC_ALIGN g_sent[ROWS];

/* Rows whose model changed since the last present. Only these can differ from
 * g_sent, so only these are worth diffing - the walk is O(touched), not O(448). */
#define TWORDS ((ROWS + 31) / 32)
static uint32_t g_touched[TWORDS];

static void touch(int y)      { g_touched[(uint32_t)y >> 5] |= 1u << ((uint32_t)y & 31u); }
static int  is_touched(int y) { return (g_touched[(uint32_t)y >> 5] >> ((uint32_t)y & 31u)) & 1u; }

/*
 * Labels are double-buffered for the same reason rows are: the diff has to be
 * against what the PANEL holds, not against the description in flight.
 */
typedef struct {
    const gfx_font *font;
    uint16_t x, y, fg;
    uint8_t  len;                    /* 0 = slot unused */
    char     s[GFX_LABEL_CHARS];
} gfx_label;

/*
 * Copied field by field, NOT by struct assignment - that emits a memcpy call
 * and there is no libc.
 *
 * gfx_row solves the same problem by padding to whole vectors and using
 * vec_copy. That does not port here: gfx_row holds only uint16_t, but a label
 * holds a POINTER, which is 4 bytes on the device and 8 on the host that runs
 * the tests. Any fixed padding is wrong on one of them - the size assertion
 * caught exactly that. Field-wise copy is size-agnostic, and at 8 labels per
 * present the cost does not register.
 */
static void label_copy(gfx_label *d, const gfx_label *s)
{
    int i;
    d->font = s->font; d->x = s->x; d->y = s->y; d->fg = s->fg; d->len = s->len;
    for (i = 0; i < GFX_LABEL_CHARS; i++) d->s[i] = s->s[i];
}

static gfx_label g_label[GFX_MAX_LABELS];
static gfx_label g_label_sent[GFX_MAX_LABELS];

static int label_differs(const gfx_label *a, const gfx_label *b)
{
    int i;
    if (a->len != b->len) return 1;
    if (a->len == 0u) return 0;                  /* both unused */
    if (a->font != b->font || a->x != b->x || a->y != b->y || a->fg != b->fg)
        return 1;
    for (i = 0; i < (int)a->len; i++) if (a->s[i] != b->s[i]) return 1;
    return 0;
}

/* Screen rectangle a label occupies. Width is len * font->w, a multiple of 8
 * because font widths are, so the extent lands on the grid the transport wants. */
static void label_rect(const gfx_label *l, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = (int)l->x;
    *y0 = (int)l->y;
    *x1 = (int)l->x + (int)l->len * (int)l->font->w - 1;
    /* Includes the bottom padding: the padded rows belong to the label, so they
     * are cleared and repainted with it rather than left to whatever was there. */
    *y1 = (int)l->y + (int)l->font->h - 1 + GFX_LABEL_PAD_BOTTOM;
}

/*
 * Scratch wide enough for the WORST case before normalisation.
 *
 * insert_run can emit every existing run plus the new one plus a clipped tail.
 * Writing that straight into a gfx_row overflowed x[GFX_MAX_RUNS] - it indexed
 * up to 2*GFX_MAX_RUNS into an 8-entry array. Build wide, normalise, then
 * store; the cap is enforced in one place instead of assumed in three.
 */
#define RUNBUF_MAX (GFX_MAX_RUNS * 2 + 2)

/*
 * insert_run emits at most: every run left of xa, the new run, and every run
 * right of xb - so 2*GFX_MAX_RUNS + 1. The loops have no bounds check because
 * this assertion is the bound. C99 has no _Static_assert; CONTRIBUTING.md's
 * idiom instead.
 */
typedef char runbuf_must_hold_worst_case[
    (RUNBUF_MAX >= GFX_MAX_RUNS * 2 + 1) ? 1 : -1];

typedef struct {
    uint16_t x[RUNBUF_MAX];
    uint16_t c[RUNBUF_MAX];
    int      n;
} runbuf;

static uint16_t buf_end(const runbuf *b, int i)
{
    return (i + 1 < b->n) ? b->x[i + 1] : (uint16_t)SH8601_WIDTH;
}
static gfx_stats g_stats;

/* Accumulated across set_* calls, published and cleared by a successful
 * present. Returned-and-forgotten counters read 0 forever, which is what
 * rows_changed used to do. */
static uint32_t g_changed_pending;
static uint32_t g_overflow_pending;

/* End of run i (exclusive). The last run always reaches the right edge. */
static uint16_t run_end(const gfx_row *r, int i)
{
    return (i + 1 < (int)r->n) ? r->x[i + 1] : (uint16_t)SH8601_WIDTH;
}

void gfx_init(void)
{
    uint32_t i;
    for (i = 0u; i < (uint32_t)ROWS; i++) {
        g_model[i].n = 1u; g_model[i].x[0] = 0u; g_model[i].c[0] = 0u;
        /* vec_copy, never `g_sent[i] = g_model[i]` - a struct assignment at
         * this size makes GCC emit a memcpy call, and there is no libc. */
        vec_copy(&g_sent[i], &g_model[i], GFX_ROW_VECTORS);
    }
    for (i = 0u; i < (uint32_t)TWORDS; i++) g_touched[i] = 0u;
    for (i = 0u; i < (uint32_t)GFX_MAX_LABELS; i++) {
        g_label[i].len = 0u;      g_label[i].font = NULL;
        g_label_sent[i].len = 0u; g_label_sent[i].font = NULL;
    }
    g_changed_pending = 0u;
    g_overflow_pending = 0u;
    elide_init();          /* marks everything dirty: first present repaints */
}

int gfx_text(int id, uint16_t x, uint16_t y, const char *str,
             uint16_t fg, const gfx_font *font)
{
    gfx_label w;
    int i = 0;

    if (id < 0 || id >= GFX_MAX_LABELS) return 0;

    w.font = font;
    /* Snap DOWN to the grid: every glyph then starts on a vector boundary and
     * the blit needs no masking or unaligned path. */
    w.x  = (uint16_t)(x & ~(uint16_t)(GFX_XGRID - 1));
    w.y  = y;
    w.fg = fg;
    if (str != NULL && font != NULL) {
        while (str[i] != '\0' && i < GFX_LABEL_CHARS) { w.s[i] = str[i]; i++; }
    }
    w.len = (uint8_t)i;
    while (i < GFX_LABEL_CHARS) { w.s[i] = '\0'; i++; }
    if (w.len == 0u) { w.font = NULL; w.x = 0u; w.y = 0u; w.fg = 0u; }

    if (!label_differs(&g_label[id], &w)) return 0;
    label_copy(&g_label[id], &w);
    return 1;
}

void gfx_text_clear(int id) { (void)gfx_text(id, 0u, 0u, NULL, 0u, NULL); }

void gfx_invalidate(void)
{
    uint32_t i;
    /* Touch every row as well as marking every row. The marks force the
     * repaint; the touches make present() re-sync g_sent afterwards, so the
     * two models cannot be left disagreeing about what the panel holds. */
    for (i = 0u; i < (uint32_t)TWORDS; i++) g_touched[i] = 0xFFFFFFFFu;
    elide_reset();
}

/*
 * Merge adjacent runs of equal colour, then enforce the run cap by merging the
 * NARROWEST adjacent pair into its left neighbour.
 *
 * That last step is lossy, so it is counted. Silently absorbing it would make a
 * too-busy row render wrong with no signal, which is the failure mode this
 * project keeps paying for.
 */
static void buf_normalise(runbuf *b)
{
    int i, j;

    j = 0;
    for (i = 1; i < b->n; i++) {
        if (b->c[i] != b->c[j]) {
            j++;
            b->x[j] = b->x[i];
            b->c[j] = b->c[i];
        }
    }
    b->n = j + 1;

    while (b->n > GFX_MAX_RUNS) {
        int best = 1;
        uint32_t best_w = 0xFFFFFFFFu;
        for (i = 1; i < b->n; i++) {
            uint32_t w = (uint32_t)(buf_end(b, i) - b->x[i]);
            if (w < best_w) { best_w = w; best = i; }
        }
        for (i = best; i + 1 < b->n; i++) {
            b->x[i] = b->x[i + 1];
            b->c[i] = b->c[i + 1];
        }
        b->n--;
        g_overflow_pending++;
    }
}

/* Paint [xa, xb) of `src` with colour c, leaving the result in `dst`. */
static void insert_run(const gfx_row *src, gfx_row *dst,
                       uint16_t xa, uint16_t xb, uint16_t c)
{
    runbuf b;
    int i;

    b.n = 0;
    for (i = 0; i < (int)src->n; i++) {
        if (src->x[i] >= xa) break;
        b.x[b.n] = src->x[i];
        b.c[b.n] = src->c[i];
        b.n++;
    }
    b.x[b.n] = xa; b.c[b.n] = c; b.n++;

    for (i = 0; i < (int)src->n; i++) {
        uint16_t s = src->x[i];
        if (run_end(src, i) <= xb) continue;        /* fully covered */
        if (s < xb) s = xb;                         /* clip to the new run */
        if (s >= (uint16_t)SH8601_WIDTH) break;
        if (s == b.x[b.n - 1]) { b.c[b.n - 1] = src->c[i]; continue; }
        b.x[b.n] = s;
        b.c[b.n] = src->c[i];
        b.n++;
    }
    b.x[0] = 0u;                                    /* invariant */
    buf_normalise(&b);

    for (i = 0; i < b.n; i++) { dst->x[i] = b.x[i]; dst->c[i] = b.c[i]; }
    dst->n = (uint16_t)b.n;
}

/*
 * Column range over which two rows differ, or 0 if identical.
 *
 * This is what makes sub-width transmission possible: the dirty extent is the
 * columns that actually changed, not the whole row. A merge-walk over both run
 * lists, so it is O(runs) rather than O(pixels).
 */
static int diff_extent(const gfx_row *a, const gfx_row *b,
                       uint16_t *dx0, uint16_t *dx1)
{
    int ia = 0, ib = 0, found = 0;
    uint16_t x = 0u, lo = 0u, hi = 0u;

    while (x < (uint16_t)SH8601_WIDTH) {
        uint16_t ea = run_end(a, ia), eb = run_end(b, ib);
        uint16_t nx = (ea < eb) ? ea : eb;
        if (a->c[ia] != b->c[ib]) {
            if (!found) { lo = x; found = 1; }
            hi = (uint16_t)(nx - 1u);
        }
        x = nx;
        if (ea <= x && ia + 1 < (int)a->n) ia++;
        if (eb <= x && ib + 1 < (int)b->n) ib++;
    }
    if (found) { *dx0 = lo; *dx1 = hi; }
    return found;
}

uint32_t gfx_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  uint16_t colour)
{
    uint32_t changed = 0u;
    uint16_t xa, xb;
    int y;

    if (y1 >= (uint16_t)ROWS) y1 = (uint16_t)(ROWS - 1);
    if (x1 >= (uint16_t)SH8601_WIDTH) x1 = (uint16_t)(SH8601_WIDTH - 1);
    if (y0 > y1 || x0 > x1) return 0u;

    /* Snap OUTWARD: the requested area is always covered, never clipped. */
    xa = (uint16_t)(x0 & ~(uint16_t)(GFX_XGRID - 1));
    xb = (uint16_t)((x1 + GFX_XGRID) & ~(uint16_t)(GFX_XGRID - 1));
    if (xb > (uint16_t)SH8601_WIDTH) xb = (uint16_t)SH8601_WIDTH;

    for (y = (int)y0; y <= (int)y1; y++) {
        gfx_row VEC_ALIGN want;
        uint16_t dx0, dx1;
        insert_run(&g_model[y], &want, xa, xb, colour);
        if (diff_extent(&g_model[y], &want, &dx0, &dx1)) {
            /* vec_copy, not struct assignment: at 34 bytes GCC emitted a call
             * to memcpy and there is no libc to satisfy it. */
            vec_copy(&g_model[y], &want, GFX_ROW_VECTORS);
            touch(y);          /* NOT elide_mark: the panel-facing diff is
                                * taken at present time, against g_sent. */
            changed++;
        }
    }
    g_changed_pending += changed;
    return changed;
}

uint32_t gfx_solid(uint16_t y0, uint16_t y1, uint16_t colour)
{
    return gfx_rect(0u, y0, (uint16_t)(SH8601_WIDTH - 1), y1, colour);
}

uint32_t gfx_split(uint16_t y0, uint16_t y1, uint16_t left, uint16_t right,
                   uint16_t x)
{
    uint32_t n = 0u;
    if (x > (uint16_t)SH8601_WIDTH) x = (uint16_t)SH8601_WIDTH;
    if (x > 0u) n += gfx_rect(0u, y0, (uint16_t)(x - 1u), y1, left);
    if (x < (uint16_t)SH8601_WIDTH)
        n += gfx_rect(x, y0, (uint16_t)(SH8601_WIDTH - 1), y1, right);
    return n;
}

/* The ONLY interpreter of gfx_row, so transmitted pixels and the change-
 * detection model cannot drift apart. Fills the whole row; the transport takes
 * the sub-range it needs. */
static void gfx_rowfn(uint16_t *row, int y)
{
    const gfx_row *m = &g_model[y];
    int i;

    /* Background first. */
    for (i = 0; i < (int)m->n; i++) {
        uint16_t s = m->x[i], e = run_end(m, i);
        vec_fill16(row + s, m->c[i], (uint32_t)(e - s) / (uint32_t)GFX_XGRID);
    }

    /* Then text over it. The blit is transparent, so a glyph's clear bits keep
     * whatever the runs just drew - which is what lets a label sit on a
     * changing background without either layer knowing about the other. */
    for (i = 0; i < GFX_MAX_LABELS; i++) {
        const gfx_label *l = &g_label[i];
        const gfx_font *f;
        int gr, c, bpr;
        if (l->len == 0u) continue;
        f = l->font;
        gr = y - (int)l->y;
        if (gr < 0 || gr >= (int)f->h) continue;
        bpr = (int)f->w / 8;
        for (c = 0; c < (int)l->len; c++) {
            int gx = (int)l->x + c * (int)f->w;
            unsigned ch = (unsigned char)l->s[c];
            if (gx + (int)f->w > SH8601_WIDTH) break;      /* clip at the edge */
            if (ch < f->first || ch >= (unsigned)(f->first + f->count))
                ch = (unsigned)f->first;                   /* unmapped -> space */
            vec_glyph_row(row + gx,
                          &f->bits[(((unsigned)(ch - f->first) * f->h)
                                    + (unsigned)gr) * (unsigned)bpr],
                          (uint32_t)bpr, l->fg);
        }
    }
}

int gfx_present(void)
{
    uint32_t t0 = cpu_cycles();
    int rc, y;
    const elide_stats *e;

    /*
     * THE DIFF THAT MATTERS. Only touched rows can differ from what was sent,
     * and a touched row that came back to where it started marks nothing.
     */
    for (y = 0; y < ROWS; y++) {
        uint16_t dx0, dx1;
        if (!is_touched(y)) continue;
        if (diff_extent(&g_sent[y], &g_model[y], &dx0, &dx1))
            elide_mark_rect(dx0, y, dx1, y);
    }

    /*
     * Labels diff the same way rows do, and a changed label marks the union of
     * where it WAS and where it IS - the old rectangle so the background can be
     * repainted over it, the new one so the glyphs land.
     */
    g_stats.labels_changed = 0u;
    for (y = 0; y < GFX_MAX_LABELS; y++) {
        int x0, y0, x1, y1;
        if (!label_differs(&g_label_sent[y], &g_label[y])) continue;
        g_stats.labels_changed++;
        /*
         * Labels mark THEIR OWN COLUMNS, like every other element.
         *
         * They marked full width for a long time, and the reason is worth
         * keeping: a narrow label mark sitting among full-width bar marks
         * splits one span into three or four, because elide coalesces
         * contiguous rows only when their extents match - and every extra span
         * boundary was another chance for debris. Measured then: 2 spans for a
         * static scene, 4 while a label updated, and artifacts that tracked
         * span count exactly.
         *
         * The comment here used to say "this is a WORKAROUND, not an
         * explanation... keeping it, with the reason, beats a fix nobody can
         * find later". The fix was found: the output AFIFO reset in
         * spi2_flush_afifo was a ~12.5 ns pulse against a 25 ns clock domain
         * and could be missed entirely, which is why draining the FIFO
         * appeared not to help. Span boundaries were never the fault; they
         * were how often the fault got a chance to fire.
         *
         * So the 1.8x that label rows were paying - 368 columns instead of 208
         * for a 13-character label - goes back.
         */
        if (g_label_sent[y].len != 0u) {
            label_rect(&g_label_sent[y], &x0, &y0, &x1, &y1);
            elide_mark_rect(x0, y0, x1, y1);
        }
        if (g_label[y].len != 0u) {
            label_rect(&g_label[y], &x0, &y0, &x1, &y1);
            elide_mark_rect(x0, y0, x1, y1);
        }
    }

    rc = elide_flush(gfx_rowfn);
    e  = elide_last();

    g_stats.rows_changed  = g_changed_pending;
    g_stats.run_overflows = g_overflow_pending;
    g_stats.rows_sent     = e->rows_sent;
    g_stats.spans         = e->spans;
    g_stats.px_sent       = e->px_sent;
    g_stats.cycles        = cpu_cycles() - t0;

    /*
     * Only now is g_sent true. elide_flush leaves failed rows dirty so the next
     * attempt retries them; advancing g_sent regardless would tell the next
     * diff those rows are already on the panel, and the update would be lost
     * for good - the exact "model drifts from reality" failure this layer
     * exists to prevent.
     */
    if (rc == SPI2_OK) {
        uint32_t i;
        for (y = 0; y < ROWS; y++)
            if (is_touched(y)) vec_copy(&g_sent[y], &g_model[y], GFX_ROW_VECTORS);
        for (i = 0u; i < (uint32_t)TWORDS; i++) g_touched[i] = 0u;
        for (y = 0; y < GFX_MAX_LABELS; y++) label_copy(&g_label_sent[y], &g_label[y]);
        g_changed_pending = 0u;
        g_overflow_pending = 0u;
    }
    return rc;
}

const gfx_stats *gfx_last(void) { return &g_stats; }
