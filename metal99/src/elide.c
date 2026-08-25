#include <stddef.h>
#include "elide.h"
#include "io.h"
#include "spi2.h"

#define ROWS      SH8601_HEIGHT
#define WORDS     ((ROWS + 31) / 32)

static uint32_t    g_dirty[WORDS];
/* x-extent per dirty row, inclusive. Only meaningful where the dirty bit is
 * set; nothing clears these, because nothing reads them when the bit is 0. */
static uint16_t    g_dx0[ROWS];
static uint16_t    g_dx1[ROWS];
static uint32_t    g_frames;
static uint32_t    g_resync_period = ELIDE_RESYNC_FRAMES;
static elide_stats g_stats;

void elide_set_resync(uint32_t frames) { g_resync_period = frames; }

void elide_init(void)
{
    g_frames = 0u;
    elide_reset();
}

void elide_reset(void)
{
    uint32_t i;
    for (i = 0u; i < (uint32_t)WORDS; i++) g_dirty[i] = 0xFFFFFFFFu;
    for (i = 0u; i < (uint32_t)ROWS; i++) {
        g_dx0[i] = 0u;
        g_dx1[i] = (uint16_t)(SH8601_WIDTH - 1);
    }
}

void elide_mark_rect(int x0, int y0, int x1, int y1)
{
    int y;
    if (x0 < 0) x0 = 0;
    if (x1 > (SH8601_WIDTH - 1)) x1 = SH8601_WIDTH - 1;
    if (y0 < 0) y0 = 0;
    if (y1 > (ROWS - 1)) y1 = ROWS - 1;
    if (x0 > x1) return;

    for (y = y0; y <= y1; y++) {
        uint32_t w = (uint32_t)y >> 5, b = 1u << ((uint32_t)y & 31u);
        if ((g_dirty[w] & b) != 0u) {          /* already dirty: union      */
            if ((uint16_t)x0 < g_dx0[y]) g_dx0[y] = (uint16_t)x0;
            if ((uint16_t)x1 > g_dx1[y]) g_dx1[y] = (uint16_t)x1;
        } else {
            g_dirty[w] |= b;
            g_dx0[y] = (uint16_t)x0;
            g_dx1[y] = (uint16_t)x1;
        }
    }
}

void elide_mark(int y0, int y1)
{
    elide_mark_rect(0, y0, SH8601_WIDTH - 1, y1);
}

static int row_dirty(int y)
{
    return (g_dirty[(uint32_t)y >> 5] >> ((uint32_t)y & 31u)) & 1u;
}

int elide_flush(void (*rowfn)(uint16_t *row, int y))
{
    uint32_t t0 = cpu_cycles();
    int y = 0, rc;

    if (rowfn == NULL) return SPI2_E_NULL;

    /* Rolling resync: refresh a rotating slice every frame rather than the
     * whole screen occasionally. The model of remote state still cannot be
     * verified, so it is still rewritten on a schedule - just without the
     * 16.6 ms spike that made every resync frame miss its deadline. */
    g_stats.resync_rows = 0u;
    if (g_resync_period != 0u) {
        int r0 = (int)((g_frames % g_resync_period) * ELIDE_RESYNC_ROWS);
        int r1 = r0 + (int)ELIDE_RESYNC_ROWS - 1;
        if (r0 < ROWS) {
            if (r1 > (ROWS - 1)) r1 = ROWS - 1;
            elide_mark(r0, r1);            /* resync is deliberately full width */
            g_stats.resync_rows = (uint32_t)(r1 - r0 + 1);
        }
        g_frames++;
    }

    g_stats.rows_sent     = 0u;
    g_stats.spans         = 0u;
    g_stats.bytes         = 0u;
    g_stats.px_sent       = 0u;
    g_stats.render_cycles = 0u;
    g_stats.flush_cycles  = 0u;

    while (y < ROWS) {
        int y0, y1;
        uint16_t x0, x1;
        if (!row_dirty(y)) { y++; continue; }

        /*
         * Coalesce a contiguous run of rows sharing the SAME x-extent.
         *
         * Each span costs a window command plus a pixel-write preamble (20 B
         * measured), so merging adjacent rows matters. But merging rows with
         * DIFFERENT extents would force their union on all of them - two rows
         * dirty at opposite edges would union to full width and send more than
         * two separate spans would. Identical-extent is the rule because the
         * common cases land on it exactly: a moving rect has one extent for all
         * its rows, and a full-width repaint has one extent for all of them.
         */
        y0 = y; x0 = g_dx0[y]; x1 = g_dx1[y];
        while ((y < ROWS) && row_dirty(y) && g_dx0[y] == x0 && g_dx1[y] == x1) y++;
        y1 = y - 1;

        rc = sh8601_write_span_x(x0, (uint16_t)y0, x1, (uint16_t)y1, rowfn);
        if (rc != SPI2_OK) {
            /* Record the cost of the attempt too. Leaving cycles at the
             * previous frame's value made a failing frame look healthy. */
            g_stats.cycles = cpu_cycles() - t0;
            return rc;
        }

        {
            const sh8601_stats *s = sh8601_last_frame();
            g_stats.render_cycles += s->render_cycles;
            g_stats.flush_cycles  += s->flush_cycles;
        }
        {
            uint32_t rows = (uint32_t)(y1 - y0 + 1);
            uint32_t cols = (uint32_t)(x1 - x0 + 1);
            g_stats.spans++;
            g_stats.rows_sent += rows;
            g_stats.px_sent   += rows * cols;
            g_stats.bytes     += rows * cols * 2u;
        }
    }

    /* Only clear once every span went out. A failed flush leaves the rows
     * dirty so the next attempt retries them rather than losing the update. */
    {
        uint32_t i;
        for (i = 0u; i < (uint32_t)WORDS; i++) g_dirty[i] = 0u;
    }
    g_stats.cycles = cpu_cycles() - t0;
    return SPI2_OK;
}

const elide_stats *elide_last(void) { return &g_stats; }
