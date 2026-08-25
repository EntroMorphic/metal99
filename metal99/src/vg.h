/*
 * vg - vector graphics: line segments, rendered by scanline. Pure ISO C99.
 *
 * WHY NOT gfx. The retained-mode run model is built for rectangles and text:
 * eight runs per row, diffed against what the panel holds, so an interface
 * transmits only what changed. A vector scene is its opposite. A dozen lines
 * crossing a row is a dozen short runs - past the cap immediately - and in a
 * game everything moves every frame, so elision has nothing to elide. Forcing
 * one through the other would lose both.
 *
 * So vg renders IMMEDIATE, straight into the row the transport is about to
 * send. vg_rowfn has exactly the signature sh8601_write_frame wants, which is
 * not a coincidence: row-at-a-time streaming with no framebuffer is what this
 * hardware already does, and a scanline vector renderer is the shape that fits
 * it. 322 KB of framebuffer would fit (DESIGN.md 5.1) and would buy nothing.
 *
 * THE ALGORITHM is a classic active-edge list. Segments are bucketed by their
 * top row; walking rows downward, each becomes active at its top and retires
 * at its bottom, carrying an x that steps by dxdy each row. Cost is O(segments
 * + rows covered) rather than O(segments x rows).
 *
 * Per row a segment covers x .. x+dxdy - the span it sweeps crossing that row.
 * That one formula draws every case: near-vertical sweeps a pixel, shallow
 * sweeps a long run, and a horizontal segment is stored as a single row whose
 * dxdy is its whole length. No special cases, which matters because special
 * cases in a rasteriser are where the gaps in diagonal lines come from.
 *
 * FIXED POINT, 16.16. No floating point: the project has never used the FPU,
 * and a rasteriser is exactly the kind of hot loop where that decision should
 * not be revisited casually.
 */
#ifndef VG_H
#define VG_H

#include <stdint.h>

#define VG_MAX_SEGS 512

/* Start a new frame: forget every segment. */
void vg_begin(void);

/* Add a segment. Coordinates are screen pixels and may lie off-screen; the
 * renderer clips. Returns 0 if the segment list is full, 1 otherwise. */
int  vg_line(int x0, int y0, int x1, int y1, uint16_t colour);

/* Prepare for rendering. Call once after the last vg_line and before the first
 * vg_rowfn: this is where segments are bucketed by row. */
void vg_finish(void);

/* Render one row. Signature matches sh8601_write_frame's rowfn. Rows MUST be
 * requested in increasing order - the active-edge list is a forward walk. */
void vg_rowfn(uint16_t *row, int y);

/* Background, painted before segments. Defaults to black. */
void vg_set_bg(uint16_t colour);

/* Segments dropped this frame because the list was full. Zero is the only
 * acceptable value in a shipping scene; anything else is a silently thinner
 * picture, so it is counted rather than ignored. */
uint32_t vg_overflow(void);

/* Segments submitted this frame. Worth watching: a scene that creeps up on
 * VG_MAX_SEGS starts losing lines quietly, and the picture just gets thinner. */
int vg_count(void);

#endif /* VG_H */
