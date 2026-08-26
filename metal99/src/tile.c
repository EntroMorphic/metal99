/* Tile-granular present. See tile.h for the measurements that justify it. */
#include <stddef.h>
#include "tile.h"
#include "sh8601.h"
#include "spi2.h"
#include "vec.h"
#include "io.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT

#define TCOLS ((W + TILE_W - 1) / TILE_W)
#define TROWS ((H + TILE_H - 1) / TILE_H)

/* The band: one tile-row of pixels, 16-byte aligned so a span starting on any
 * tile boundary is a legal FIFO source (spi2.h alignment contract). */
/* Named g_tileband, not g_band: sh8601.c already has a g_band - its 46 KB DMA
 * staging buffer - and two file-static arrays sharing a name in one project is
 * a trap for whoever reads a linker map next. */
static uint16_t VEC_ALIGN g_tileband[TILE_H * W];
static int      g_tileband_y0;                  /* panel row of g_tileband[0]        */

static uint32_t g_hash[TROWS][TCOLS][8];    /* what the panel holds          */
static uint8_t  g_dirty[TCOLS];             /* this band only                */
static uint32_t g_resync = TILE_RESYNC_ON;
static uint32_t g_frames;
static tile_stats g_stats;

/*
 * A tile is a COLUMN of the band: TILE_W pixels wide, TILE_H rows tall, with
 * the rows W pixels apart. At TILE_W 8 that is exactly one 128-bit vector per
 * row, so hashing one is a strided walk of TILE_H vectors and vec_hash16 does
 * it eight lanes at a time.
 *
 * THE FIRST VERSION WAS SCALAR - h = h * K + pixel, over every pixel of every
 * tile - and measured 8 ms a frame on the device, against 12 ms of transmit it
 * existed to reduce. It made the whole scheme slower than the row elision it
 * was meant to beat, and it was a plain violation of the project's no-scalar-
 * per-element rule sitting in the middle of a new module.
 *
 * A COLLISION IS A MISSED UPDATE: two different tiles hashing the same means a
 * stale tile is never sent. The rolling resync below bounds how long that could
 * persist, which is the same argument elide makes about drift it cannot see.
 */
typedef char tile_w_is_one_vector[(TILE_W * 2 == VEC_BYTES) ? 1 : -1];

/*
 * THE PANEL MUST DIVIDE EVENLY INTO TILES. Both of these hold today (368/8,
 * 448/16) and neither is checked anywhere else, so they are landmines rather
 * than bugs - which is exactly the kind this file should not be allowed to
 * grow.
 *
 * A partial band would leave the tail of g_band holding the PREVIOUS band's
 * rows, and tile_hash always folds TILE_H of them: the hash would depend on
 * stale pixels, and tiles would be called clean or dirty for reasons that have
 * nothing to do with their contents. A partial column would read past the end
 * of a row into the next one. Neither failure is visible in the timing or the
 * stats; both would show up as debris, and debris in this project has a
 * history of being blamed on the transport.
 */
typedef char tile_h_divides_panel[(H % TILE_H == 0) ? 1 : -1];
typedef char tile_w_divides_panel[(W % TILE_W == 0) ? 1 : -1];

/* 16-byte aligned: vec_hash16 stores the accumulator with a vector store. */
static uint32_t VEC_ALIGN g_h[8];

static void tile_hash(int tcol)
{
    vec_hash16(&g_tileband[(size_t)tcol * TILE_W],
               (uint32_t)TILE_H, (uint32_t)(W * 2), g_h);
}

void tile_set_resync(uint32_t frames) { g_resync = frames; }
const tile_stats *tile_last(void)     { return &g_stats; }

/*
 * A FLAG, NOT A SENTINEL.
 *
 * This used to poison the stored hashes - flip word 0 and trust that no real
 * tile would match. It can: after tile_init zeroes everything, the poisoned
 * value is {0xFFFFFFFF,0,0,0}, which is a perfectly reachable hash, and a tile
 * that lands on it is declared clean and never painted. It cost a full
 * afternoon-shaped debugging detour with one hash constant and not others,
 * because whether any tile hits it depends on the multiplier.
 *
 * The comment on the old version even said "a value the hash cannot produce is
 * not available" - and then used one anyway. A flag has no such failure mode.
 */
static int g_force;

void tile_reset(void) { g_force = 1; }

void tile_init(void)
{
    int r, c;
    for (r = 0; r < TROWS; r++)
        for (c = 0; c < TCOLS; c++) {
            int k;
            for (k = 0; k < 8; k++) g_hash[r][c][k] = 0u;
        }
    g_frames = 0u;
    tile_reset();                       /* first present repaints everything */
}

/* Serves rows out of the band. sh8601 asks for absolute panel rows. */
/*
 * Serves rows out of the band. sh8601 asks for absolute panel rows.
 *
 * The bounds check cannot fire: every span this file issues lies inside the
 * band that was just rendered. It is here because the alternative to returning
 * is transmitting whatever the caller's buffer happened to hold, and a silent
 * wrong-pixels failure is the worst kind this project has had. If it ever does
 * fire the frame is visibly wrong rather than subtly wrong, which is the
 * better of the two.
 */
static void band_rowfn(uint16_t *row, int y)
{
    int r = y - g_tileband_y0;
    if (r < 0 || r >= TILE_H) { vec_zero(row, (uint32_t)(W * 2 / VEC_BYTES)); return; }
    vec_copy(row, &g_tileband[(size_t)r * W], (uint32_t)(W * 2 / VEC_BYTES));
}

int tile_present(void (*rowfn)(uint16_t *row, int y))
{
    int tr, c, rc;
    uint32_t t_mark;
    int resync_row = -1;

    if (rowfn == NULL) return SPI2_E_NULL;

    g_stats.tiles_dirty = 0u;
    g_stats.tiles_total = 0u;
    g_stats.spans       = 0u;
    g_stats.px_sent     = 0u;
    g_stats.render_cycles = 0u;
    g_stats.flush_cycles  = 0u;
    g_stats.hash_cycles   = 0u;

    if (g_resync != 0u) {
        resync_row = (int)(g_frames % (uint32_t)TROWS);
        g_frames++;
    }

    for (tr = 0; tr < TROWS; tr++) {
        int y0 = tr * TILE_H;
        int y1 = y0 + TILE_H - 1;
        int r, any = 0;

        if (y1 > H - 1) y1 = H - 1;

        /* Render the band. Every row, every frame - that is the trade this
         * path makes, and tile.h says so out loud. */
        g_tileband_y0 = y0;          /* before anything can read the band */
        t_mark = cpu_cycles();
        for (r = y0; r <= y1; r++) rowfn(&g_tileband[(size_t)(r - y0) * W], r);
        g_stats.render_cycles += cpu_cycles() - t_mark;

        t_mark = cpu_cycles();
        for (c = 0; c < TCOLS; c++) {
            uint32_t *st = g_hash[tr][c];
            int same = 1, k;
            tile_hash(c);
            for (k = 0; k < 8; k++) if (st[k] != g_h[k]) { same = 0; break; }
            g_stats.tiles_total++;
            g_dirty[c] = (uint8_t)(!same || g_force || (tr == resync_row));
            if (g_dirty[c]) { any = 1; g_stats.tiles_dirty++; }
            for (k = 0; k < 8; k++) st[k] = g_h[k];
        }
        g_stats.hash_cycles += cpu_cycles() - t_mark;
        if (!any) continue;

        /* Contiguous dirty tiles become one span. Adjacent columns share a
         * window; isolated ones do not, which is the cost this scheme trades
         * pixels for. */
        c = 0;
        while (c < TCOLS) {
            int a, b;
            if (!g_dirty[c]) { c++; continue; }
            a = c;
            while (c + 1 < TCOLS && g_dirty[c + 1]) c++;
            b = c;
            c++;

            {
                uint16_t x0 = (uint16_t)(a * TILE_W);
                uint16_t x1 = (uint16_t)((b + 1) * TILE_W - 1);
                if (x1 > (uint16_t)(W - 1)) x1 = (uint16_t)(W - 1);

                t_mark = cpu_cycles();
                rc = sh8601_write_span_x(x0, (uint16_t)y0, x1, (uint16_t)y1,
                                         band_rowfn);
                g_stats.flush_cycles += cpu_cycles() - t_mark;
                if (rc != SPI2_OK) {
                    /*
                     * Every hash up to here has already been updated to match
                     * what we INTENDED to send, and some of it did not go out.
                     * The stored hashes are therefore a lie about the panel,
                     * and no partial repair is trustworthy - so force the next
                     * present to repaint everything, which is the one recovery
                     * that cannot be subtly wrong. elide does the narrower
                     * thing (leave failed rows dirty) because it can: its
                     * model is per-row and it knows exactly which rows failed.
                     */
                    g_force = 1;
                    return rc;
                }
                g_stats.spans++;
                g_stats.px_sent += (uint32_t)(x1 - x0 + 1)
                                 * (uint32_t)(y1 - y0 + 1);
            }
        }
    }
    g_force = 0;
    return SPI2_OK;
}
