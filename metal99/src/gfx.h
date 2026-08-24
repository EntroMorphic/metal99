/*
 * gfx - retained-mode graphics messaging layer. Pure ISO C99.
 *
 * WHAT THIS IS FOR. Not transport - that exists and is 200 lines. Its job is
 * to KNOW WHAT DID NOT CHANGE so we never send it. The panel keeps its own
 * framebuffer, so an untouched pixel costs nothing; the fastest pixel is the
 * one never transmitted.
 *
 * WHY RETAINED, NOT IMMEDIATE. NeoGPU streams opcodes into a backend that owns
 * a framebuffer. We have no framebuffer - 322 KB will not fit in 192 KB of
 * DRAM - and rows are streamed straight to the panel. So instead of replaying
 * a command list, we keep a tiny model of what each ROW should look like
 * (448 descriptors, 1792 bytes) and diff it.
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
    uint16_t x;             /* split column                         */
} gfx_row;

typedef struct {
    uint32_t rows_changed;  /* rows whose descriptor actually differed */
    uint32_t rows_sent;     /* rows transmitted (>= changed, span-coalesced) */
    uint32_t spans;
    uint32_t cycles;
} gfx_stats;

void gfx_init(void);

/* Set rows y0..y1 inclusive. Returns the number of rows that actually CHANGED
 * - zero means the call was fully elided and nothing will be transmitted. */
uint32_t gfx_solid(uint16_t y0, uint16_t y1, uint16_t colour);
uint32_t gfx_split(uint16_t y0, uint16_t y1, uint16_t left, uint16_t right,
                   uint16_t x);

/* Transmit whatever changed. */
int gfx_present(void);

/* Force a full repaint next present - the escape hatch for model drift. */
void gfx_invalidate(void);

const gfx_stats *gfx_last(void);

#endif /* GFX_H */
