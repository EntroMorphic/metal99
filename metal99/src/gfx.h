/*
 * gfx - retained-mode graphics messaging layer. Pure ISO C99.
 *
 * WHAT THIS IS FOR. Not transport - that exists and is 200 lines. Its job is
 * to KNOW WHAT DID NOT CHANGE so we never send it. The panel keeps its own
 * framebuffer, so an untouched pixel costs nothing; the fastest pixel is the
 * one never transmitted.
 *
 * WHY RETAINED, NOT IMMEDIATE. NeoGPU streams opcodes into a backend that owns
 * a framebuffer. We have no framebuffer, and rows are streamed straight to the
 * panel, so instead of replaying a command list we keep a tiny model of what
 * each ROW should look like and diff it. The reason is not that a framebuffer
 * would not fit - it fits (DESIGN.md 5.1) - it is that the panel is wire-bound
 * and storing pixels does not send fewer of them.
 *
 * WHY DIFFED, NOT DECLARED. An earlier demo declared which rows it had touched
 * and got it subtly wrong: it marked a position two frames stale, which worked
 * only because consecutive positions overlapped, and leaked permanently at the
 * wrap. Deriving dirtiness from an actual before/after comparison removes that
 * entire class of bug - a caller cannot mismark what it does not mark.
 *
 * ROWS ARE RUNS, NOT BANDS.
 *
 * A row was previously one of two things: a single colour, or two colours with
 * ONE transition. That could express a full-width band and a single vertical
 * edge - and could NOT express a rectangle anywhere but against a screen edge,
 * because a row crossing a centred box is bg|fg|bg, which is two transitions.
 * A row is now a list of runs, so arbitrary rectangles compose.
 *
 * The diff is per-COLUMN, not per-row: moving a 10x10 box marks 10 rows of 16
 * pixels, not 10 rows of 368. That is the difference between 1.38 ms and
 * 0.04 ms on the measured wire (DESIGN.md 3.0).
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stddef.h>
#include "sh8601.h"
#include "vec.h"

/*
 * X GRID. Run boundaries snap to VEC_PIX16 (8 pixels).
 *
 * Not arbitrary: vec_fill16 works in whole 128-bit vectors, and the transport's
 * alignment contract needs the byte offset of a sub-width span to be a multiple
 * of 16 (spi2.h). 8 pixels is 16 bytes, so a grid-aligned run is exactly what
 * both layers can express. SH8601_WIDTH is 368 = 46 * 8, so the grid divides
 * the screen exactly with no ragged last column.
 *
 * The model stores the SNAPPED value, never the caller's. Storing a precision
 * the renderer does not have is what made gfx_split retransmit rows identical
 * to those already on the glass.
 *
 * Sub-grid placement is possible - the same broadcast/compare/select the glyph
 * path uses can mask a partial vector - and is deliberately not in this layer
 * yet. See DESIGN.md 6.9a.
 */
#define GFX_XGRID   VEC_PIX16
#define GFX_COLS    (SH8601_WIDTH / GFX_XGRID)

/*
 * Runs per row. Each rectangle drawn into a row costs at most two boundaries,
 * so 8 runs holds three overlapping rectangles plus a background. On overflow
 * the two narrowest adjacent runs are merged - lossy, bounded, and counted in
 * gfx_stats.run_overflows rather than silently absorbed.
 */
#define GFX_MAX_RUNS 8

/*
 * PADDED TO A WHOLE NUMBER OF VECTORS, deliberately.
 *
 * At 34 bytes GCC emitted a call to memcpy() for `g_model[y] = want` - and
 * there is no libc to link it against, so the build failed at the link step.
 * Padding to 48 bytes (3 vectors) lets vec_copy() move a row descriptor in
 * three instructions, which is both the fix and the idiomatic answer here: the
 * no-scalar rule already says bulk moves go through the vector unit.
 */
#define GFX_ROW_VECTORS 3

typedef struct {
    uint16_t x[GFX_MAX_RUNS];   /* run i starts at x[i]; x[0] is always 0 */
    uint16_t c[GFX_MAX_RUNS];   /* wire-order RGB565                      */
    uint16_t n;                 /* 1 .. GFX_MAX_RUNS                      */
    uint16_t pad[7];
} gfx_row;

/* C99 has no _Static_assert (C11); CONTRIBUTING.md's idiom instead. If this
 * fails, GFX_MAX_RUNS changed without GFX_ROW_VECTORS following it. */
typedef char gfx_row_must_be_whole_vectors[
    (sizeof(gfx_row) == (size_t)(GFX_ROW_VECTORS * VEC_BYTES)) ? 1 : -1];

typedef struct {
    /*
     * rows_changed counts MODEL WRITES since the last present: a row set twice
     * before presenting counts twice. rows_sent counts DISTINCT rows put on the
     * wire, because elide's dirty set is a set. Neither bounds the other -
     * moving a 96-row bar by 4 px gives changed=192, sent=100.
     */
    uint32_t rows_changed;
    uint32_t rows_sent;
    uint32_t spans;
    uint32_t cycles;
    uint32_t px_sent;        /* pixels transmitted - the figure sub-width moves */
    uint32_t run_overflows;  /* rows that lost a run to the MAX_RUNS cap        */
} gfx_stats;

void gfx_init(void);

/*
 * Fill a rectangle. x0/x1 are snapped OUTWARD to the grid so the requested area
 * is always covered, never clipped. Inclusive on all four edges.
 *
 * Returns the number of rows whose model actually CHANGED - zero means the call
 * was fully elided and nothing will be transmitted.
 */
uint32_t gfx_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  uint16_t colour);

/* Full-width band: gfx_rect(0, y0, WIDTH-1, y1, colour). */
uint32_t gfx_solid(uint16_t y0, uint16_t y1, uint16_t colour);

/* Two colours split at x. Now just two rects; kept because it reads better at
 * the call site and because the demo and tests use it. */
uint32_t gfx_split(uint16_t y0, uint16_t y1, uint16_t left, uint16_t right,
                   uint16_t x);

/* Transmit whatever changed. */
int gfx_present(void);

/* Force a full repaint next present - the escape hatch for model drift. */
void gfx_invalidate(void);

const gfx_stats *gfx_last(void);

#endif /* GFX_H */
