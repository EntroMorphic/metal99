/*
 * TILELAB - what would a framebuffer and tile-level diffing actually save?
 *
 * The runtime elides at ROW granularity with an x-extent. A framebuffer would
 * let it diff TILES instead, which should win exactly where row marking is
 * weakest: a small object moving through empty space currently dirties rows
 * that are mostly unchanged.
 *
 * That is the claim. This measures it, on the real game, before any memory map
 * is touched.
 *
 * IT MEASURES SPANS AS WELL AS PIXELS, because pixels are not the cost. Each
 * span carries a window command and a ~20-byte preamble, and - established the
 * hard way - short or isolated spans are what put debris on the glass. A tile
 * scheme that halves the bytes while tripling the spans is not obviously a win,
 * and might not be shippable at all. Reporting only the byte saving would be
 * the flattering half of the answer.
 *
 * Dirty tiles are grouped the way the transport would want them: contiguous
 * runs within a tile-row become one span, and vertically adjacent runs sharing
 * an x-extent coalesce, which is exactly what elide_flush already does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "ui.h"
#include "vg.h"
#include "sh8601.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT
#define FRAMES 6000

static uint16_t g_fb[H][W];
static uint16_t g_row[W];

/* The app presents through elide; this harness wants the whole frame, so it
 * re-finishes and walks every row. vg_finish is idempotent and the row walk is
 * non-destructive, so this is the same frame, not a re-simulation. */
int sh8601_write_frame(void (*rowfn)(uint16_t *row, int y)) { (void)rowfn; return 0; }

static int g_rows_sent, g_spans_row;
int sh8601_write_span_x(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        void (*rowfn)(uint16_t *, int))
{
    (void)x0; (void)x1; (void)rowfn;
    g_spans_row++;
    g_rows_sent += (int)y1 - (int)y0 + 1;
    return 0;
}
static sh8601_stats g_ss;
const sh8601_stats *sh8601_last_frame(void) { return &g_ss; }
uint32_t g_cpu_hz = 240000000u;
static uint32_t g_cyc;
uint32_t cpu_cycles(void) { return (g_cyc += 1000u); }

/* Polynomial rolling hash, same shape as fold.c - obviously correct beats
 * clever for an instrument. */
static uint32_t tile_hash(int tx, int ty, int tw, int th)
{
    uint32_t h = 2166136261u;
    int y, x;
    for (y = ty; y < ty + th && y < H; y++)
        for (x = tx; x < tx + tw && x < W; x++)
            h = h * 16777619u + g_fb[y][x];
    return h;
}

static void run(int tw, int th)
{
    static uint32_t sent[H][W];        /* oversized; indexed [ty][tx] */
    int cols = (W + tw - 1) / tw, rows_t = (H + th - 1) / th;
    long px = 0, spans = 0;
    int f, tx, ty, first = 1;
    static uint8_t dirty[H][W];

    memset(sent, 0, sizeof sent);
    APP.init();

    for (f = 0; f < FRAMES; f++) {
        int y;
        if (APP.event && f > 20 && (f % 11) == 0) {
            ui_event e;
            e.kind = UI_PRESS; e.id = 0;
            e.x = (uint16_t)(120 + ((f * 37) % 140));
            e.y = (uint16_t)(60 + ((f * 53) % 90));
            e.ax = e.x; e.ay = e.y; e.ms = 0u;
            APP.event(&e);
        }
        APP.frame((uint32_t)f);

        vg_finish();
        for (y = 0; y < H; y++) { vg_rowfn(g_row, y); memcpy(g_fb[y], g_row, sizeof g_row); }

        /* Which tiles changed since the panel last saw them? */
        for (ty = 0; ty < rows_t; ty++)
            for (tx = 0; tx < cols; tx++) {
                uint32_t h = tile_hash(tx * tw, ty * th, tw, th);
                dirty[ty][tx] = (uint8_t)(first || h != sent[ty][tx]);
                sent[ty][tx] = h;
            }
        first = 0;

        /* Group into spans: contiguous runs inside a tile-row, then merge
         * vertically when the run above has the same extent. */
        {
            int prev_a = -1, prev_b = -1;
            for (ty = 0; ty < rows_t; ty++) {
                int a = 0;
                while (a < cols) {
                    int b;
                    if (!dirty[ty][a]) { a++; continue; }
                    b = a;
                    while (b + 1 < cols && dirty[ty][b + 1]) b++;
                    px += (long)(b - a + 1) * tw * th;
                    if (!(a == prev_a && b == prev_b)) spans++;
                    prev_a = a; prev_b = b;
                    a = b + 1;
                }
                if (prev_a >= 0 && a >= cols) { /* row ended; keep for merge */ }
            }
        }
    }

    printf("  %2dx%-3d  pixels sent %6.1f%%   spans %5.2f/frame\n",
           tw, th, 100.0 * (double)px / ((double)FRAMES * W * H),
           (double)spans / FRAMES);
}

int main(void)
{
    printf("tilelab: gridvoid, %d frames\n\n", FRAMES);
    printf("ROW ELISION (what ships today)\n");
    APP.init();
    {
        int f, y;
        for (f = 0; f < FRAMES; f++) {
            if (APP.event && f > 20 && (f % 11) == 0) {
                ui_event e; e.kind = UI_PRESS; e.id = 0;
                e.x = (uint16_t)(120 + ((f * 37) % 140));
                e.y = (uint16_t)(60 + ((f * 53) % 90));
                e.ax = e.x; e.ay = e.y; e.ms = 0u; APP.event(&e);
            }
            APP.frame((uint32_t)f);
            vg_finish();
            for (y = 0; y < H; y++) vg_rowfn(g_row, y);
        }
        printf("  full-width rows: pixels sent %6.1f%%   spans %5.2f/frame\n\n",
               100.0 * (double)g_rows_sent / ((double)FRAMES * H),
               (double)g_spans_row / FRAMES);
    }

    printf("TILE DIFFING (what a framebuffer would allow)\n");
    printf("  square tiles - fine x granularity, but SHORT windows\n");
    run(8, 8);
    run(16, 16);
    run(32, 32);
    printf("\n  TALL AND NARROW - width buys the saving, height keeps the\n");
    printf("  window out of the range that produced debris\n");
    run(8, 32);
    run(8, 48);
    run(8, 64);
    run(16, 48);
    run(16, 64);
    run(24, 64);
    return 0;
}
