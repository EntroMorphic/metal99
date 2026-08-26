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
 * DIFFED AGAINST WHAT WAS SENT, NOT AGAINST THE MODEL IN FLIGHT.
 *
 * The first version diffed at SET time, comparing each call against the current
 * model. That marked every intermediate state. Erasing a box and drawing it
 * 4 px lower takes the overlapping rows FG -> BG -> FG: net unchanged, but both
 * transitions marked them, and they were transmitted for nothing. Measured on
 * an 88x88 box: 92 rows and 8,096 px marked for 8 rows and 704 px of real
 * change - 11.5x.
 *
 * So the model is double-buffered. Callers write g_model freely; gfx_present
 * diffs it against g_sent - what the panel actually last received - and marks
 * only the difference. Intermediate states cost nothing, and a caller can no
 * longer make a frame expensive by describing it in an awkward order.
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
#include "font.h"

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

/*
 * TEXT IS DESCRIBED, NOT RASTERISED INTO THE MODEL.
 *
 * A glyph scanline like 0b01100110 is five runs. A line of twenty characters
 * would be a hundred runs in a row that holds eight, so rasterising text into
 * the run model is not an option - and punching a "this row is custom, call
 * back" hole in the model would give up the diffing that the whole layer exists
 * for.
 *
 * So a label is a DESCRIPTION - position, colour, font, and the string itself -
 * and it is diffed exactly like a run list. The string is COPIED in rather than
 * pointed at: a caller that formats into a reused buffer would otherwise change
 * the content without changing the pointer, and the diff would miss it. Owning
 * the bytes makes the comparison exact instead of a hash that can collide.
 *
 * Labels compose OVER the run model. The blit is transparent (vec_glyph_row),
 * so a label on a background that changes underneath repaints correctly: the
 * run diff marks the row and the row is re-rendered runs-then-text.
 */
#define GFX_MAX_LABELS  8
#define GFX_LABEL_CHARS 40

/*
 * Rows of clear space kept BELOW a label's glyphs.
 *
 * The font cell is exactly as tall as its tallest glyph, so a label's box ends
 * flush against the last row of ink. Text set immediately above other content
 * reads as cramped, and anything that lands in the row just under the glyphs
 * has nothing repainting it. These rows are part of the label's rectangle:
 * they are cleared when it changes and repainted when it moves.
 */
#define GFX_LABEL_PAD_BOTTOM 5

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
    uint32_t labels_changed; /* labels differing from what the panel holds      */
} gfx_stats;

void gfx_init(void);

/*
 * Fill a rectangle. x0/x1 are snapped OUTWARD to the grid so the requested area
 * is always covered, never clipped. Inclusive on all four edges.
 *
 * Returns the number of rows this call changed IN THE MODEL. That is not the
 * number that will be transmitted: a row changed here and changed back before
 * the next present costs nothing. Read gfx_last()->rows_sent for what actually
 * went to the panel.
 */
uint32_t gfx_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  uint16_t colour);

/* Full-width band: gfx_rect(0, y0, WIDTH-1, y1, colour). */
uint32_t gfx_solid(uint16_t y0, uint16_t y1, uint16_t colour);

/* Two colours split at x - a left band and a right band. Now literally two
 * gfx_rect calls; kept because a split reads better at a call site than two
 * rects whose bounds must agree. --gc-sections drops it when unused. */
uint32_t gfx_split(uint16_t y0, uint16_t y1, uint16_t left, uint16_t right,
                   uint16_t x);

/*
 * Place text in slot `id` (0 .. GFX_MAX_LABELS-1).
 *
 * x is snapped DOWN to the 8px grid so each glyph lands on a whole vector -
 * the blit then needs no masking. The string is copied, truncated at
 * GFX_LABEL_CHARS. A NULL or empty string clears the slot, and so does
 * gfx_text_clear().
 *
 * Returns 1 if the description changed, 0 if it was identical and the call
 * will therefore transmit nothing.
 */
int  gfx_text(int id, uint16_t x, uint16_t y, const char *s,
              uint16_t fg, const gfx_font *font);
void gfx_text_clear(int id);

/* Transmit whatever changed. */
int gfx_present(void);

/* Force a full repaint next present - the escape hatch for model drift. */
void gfx_invalidate(void);

/*
 * DIAGNOSTIC: force label rows to mark FULL WIDTH, as they did while the
 * span-boundary debris was unexplained.
 *
 * Exists so the workaround can be A/B'd against its replacement inside ONE
 * build, on identical content, instead of comparing two flashes from memory -
 * which has produced a wrong conclusion in this project more than once. Off by
 * default; labels mark their own columns and elide unions extents when that is
 * cheaper than another span.
 */
void gfx_dbg_label_full(int on);

const gfx_stats *gfx_last(void);

#endif /* GFX_H */
