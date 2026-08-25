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
 * panel. So instead of replaying a command list, we keep a tiny model of what
 * each ROW should look like (448 descriptors, 1792 bytes) and diff it.
 *
 * The reason is NOT that a framebuffer will not fit. It fits: 368x448x2 is
 * 322 KB, and right-sizing the 128 KB IRAM region (8 KB is used) leaves ~373 KB
 * of DRAM below the ROM stack. The 192 KB in link.ld is a self-imposed limit,
 * not silicon. This comment used to say otherwise.
 *
 * The reason is that the panel is WIRE-BOUND. Storing pixels does not send
 * fewer of them, so a framebuffer costs 322 KB and buys no throughput. What
 * reduces bytes on the wire is knowing what did not change, and that needs a
 * model - 1792 bytes of it - not a copy of the screen.
 *
 * WHY DIFFED, NOT DECLARED. An earlier demo declared which rows it had touched
 * and got it subtly wrong: it marked a position two frames stale, which worked
 * only because consecutive positions overlapped, and leaked permanently at the
 * wrap. Deriving dirtiness from an actual before/after comparison removes that
 * entire class of bug - a caller cannot mismark what it does not mark.
 *
 * It also elides redundantly: setting a row to the colour it already has
 * produces no transmission at all.
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "sh8601.h"

/* What a row looks like. Compared field-by-field to detect change. */
typedef enum {
    GFX_ROW_SOLID = 0,      /* one colour across the row            */
    GFX_ROW_SPLIT = 1       /* colour a left of x, colour b right    */
} gfx_kind;

typedef struct {
    uint8_t  kind;
    uint16_t a;             /* colour, wire-order RGB565            */
    uint16_t b;             /* second colour for SPLIT              */
    uint16_t x;             /* split column, ALREADY quantised      */
} gfx_row;

typedef struct {
    /*
     * These count DIFFERENT things and neither bounds the other.
     *
     * rows_changed counts MODEL WRITES since the last present: a row set twice
     * before presenting counts twice. rows_sent counts DISTINCT rows put on
     * the wire, because elide's dirty set is a set.
     *
     * So rows_sent can be well BELOW rows_changed. Measured on hardware,
     * moving a 96-row bar by 4 px: changed=192 (erase 96 + draw 96) but
     * sent=100 (the union of two ranges overlapping by 92). This field used to
     * be documented as "rows transmitted (>= changed)", which the first run
     * that actually populated rows_changed immediately disproved.
     */
    uint32_t rows_changed;  /* model writes since last present   */
    uint32_t rows_sent;     /* distinct rows transmitted         */
    uint32_t spans;
    uint32_t cycles;
} gfx_stats;

void gfx_init(void);

/* Set rows y0..y1 inclusive. Returns the number of rows that actually CHANGED
 * - zero means the call was fully elided and nothing will be transmitted. */
uint32_t gfx_solid(uint16_t y0, uint16_t y1, uint16_t colour);

/*
 * Split row: `left` up to column x, `right` from x on.
 *
 * x IS QUANTISED DOWN to a multiple of VEC_PIX16 (8 pixels) and the quantised
 * value is what gets stored. The renderer fills in whole 128-bit vectors, so
 * that is the only split position it can actually produce; recording the
 * caller's exact x instead would make the model claim a precision the panel
 * never receives, and every sub-8-pixel "change" would retransmit bytes
 * identical to the ones already on the glass.
 */
uint32_t gfx_split(uint16_t y0, uint16_t y1, uint16_t left, uint16_t right,
                   uint16_t x);

/* Transmit whatever changed. */
int gfx_present(void);

/* Force a full repaint next present - the escape hatch for model drift. */
void gfx_invalidate(void);

const gfx_stats *gfx_last(void);

#endif /* GFX_H */
