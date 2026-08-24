#include <stddef.h>
#include "elide.h"
#include "io.h"
#include "spi2.h"

#define ROWS      SH8601_HEIGHT
#define WORDS     ((ROWS + 31) / 32)

static uint32_t    g_dirty[WORDS];
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
}

void elide_mark(int y0, int y1)
{
    int y;
    if (y0 < 0) y0 = 0;
    if (y1 > (ROWS - 1)) y1 = ROWS - 1;
    for (y = y0; y <= y1; y++) {
        g_dirty[(uint32_t)y >> 5] |= 1u << ((uint32_t)y & 31u);
    }
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
            elide_mark(r0, r1);
            g_stats.resync_rows = (uint32_t)(r1 - r0 + 1);
        }
        g_frames++;
    }

    g_stats.rows_sent = 0u;
    g_stats.spans     = 0u;
    g_stats.bytes     = 0u;

    while (y < ROWS) {
        int y0, y1;
        if (!row_dirty(y)) { y++; continue; }

        /* Coalesce a contiguous run. Each span costs a window command plus a
         * pixel-write preamble, so merging adjacent rows matters: many tiny
         * spans re-create exactly the per-transaction overhead that GDMA was
         * introduced to remove. */
        y0 = y;
        while ((y < ROWS) && row_dirty(y)) y++;
        y1 = y - 1;

        rc = sh8601_write_span((uint16_t)y0, (uint16_t)y1, rowfn);
        if (rc != SPI2_OK) return rc;

        g_stats.spans++;
        g_stats.rows_sent += (uint32_t)(y1 - y0 + 1);
        g_stats.bytes     += (uint32_t)(y1 - y0 + 1) * SH8601_WIDTH * 2u;
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
