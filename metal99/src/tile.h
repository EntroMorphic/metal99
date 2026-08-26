/*
 * TILE PRESENT - elision at tile granularity, without a framebuffer.
 *
 * elide diffs a DESCRIPTION of the scene against what the panel holds, at row
 * granularity with an x-extent. That is exact and cheap, and it is weakest
 * exactly where a small object moves through empty space: the object dirties
 * whole rows that are almost entirely unchanged.
 *
 * Measured on gridvoid over 6000 frames, transmitted pixels:
 *
 *      row elision (ships today)   88.7%   1.59 spans/frame
 *      16x16 tiles                 46.4%   6.60
 *      8x8 tiles                   27.6%  15.27
 *
 * The extra spans are affordable: a span costs 11.8 us, measured on hardware
 * by sending the same 448 rows split 1, 2, 4 ... 64 ways (apps/spancost.c).
 * Fifteen spans is 0.18 ms against several milliseconds saved.
 *
 * NO FRAMEBUFFER IS REQUIRED, which is the point. Diffing needs to DETECT
 * change, not STORE the frame, and rows are already produced strictly top to
 * bottom. One band buffer the height of a tile row, plus a hash per tile, is
 * enough:
 *
 *      band   TILE_H rows x 368 px x 2 B   11.5 KB at 8x16
 *      hashes 46 x 28 tiles x 8 words       40.2 KB
 *                                           -------
 *                                           51.8 KB
 *
 * against 322 KB for a framebuffer. No memory-map change, no IRAM resize, no
 * going near the ROM stack floor.
 *
 * The hashes are eight words per tile rather than one because a single word
 * collides in practice on sparse content - see vec.h, where the arithmetic is.
 * An earlier draft of this comment said 5 KB, from back when it was one word;
 * the number is measured from the linker map now.
 *
 * WHAT IT COSTS, stated plainly: elide SKIPS RENDERING for rows nothing
 * marked. Hashing a tile requires rendering it first, so this path renders
 * every row, every frame, and only the transmit shrinks. It wins when
 * transmit dominates render, which is the case here and may not be in an app
 * with an expensive rowfn.
 */
/* Guard is TILE_H_INCLUDED, not TILE_H: the tile height below is TILE_H, and
 * the obvious guard name silently redefines it. */
#ifndef TILE_H_INCLUDED
#define TILE_H_INCLUDED

#include <stdint.h>

/*
 * Tile width must be a multiple of the transport's 8-px alignment grid: a span
 * starts at a 16-byte boundary so the FIFO's vector load reads exactly the
 * bytes it sends.
 *
 * Height is the interesting knob, and the risky one. Savings come almost
 * entirely from height - 8x8 sends 27.6% of pixels, 8x64 sends 69.1% - but a
 * short span is also what put debris on the glass (DESIGN.md 11.4), and a tile
 * row IS a span's height. Start at 16: a real saving, and twice the height of
 * anything that misbehaved.
 */
#define TILE_W 8
#define TILE_H 8

/*
 * Rolling resync: one tile ROW is retransmitted unconditionally each frame, so
 * the whole panel is rewritten every TROWS frames - 28 here, about 0.7 s at
 * 40 Hz. Set to 0 with tile_set_resync() to disable, which is a TEST ONLY
 * move: it removes the safety net so drift becomes visible instead of being
 * scrubbed.
 *
 * Same reasoning as elide - the model of remote state cannot be verified, so
 * it is rewritten on a schedule - plus one this scheme needs and elide does
 * not: two different tiles CAN hash the same, and resync bounds how long such
 * a miss survives.
 *
 * THIS IS A FLAG, NOT A PERIOD. It used to be spelled 120, as if it were a
 * frame count, and nothing ever read the magnitude - the code tested only
 * whether it was non-zero and then swept one row per frame regardless. The
 * number was documentation of something that was not happening.
 */
#define TILE_RESYNC_ON 1u

/*
 * Per-frame measurements, read with tile_last().
 *
 * NOT FREE, and worth saying so: filling this costs about 150 cpu_cycles()
 * calls a frame, roughly 6 us, or 0.03% of an 18 ms frame. That is a
 * deliberate trade, not an oversight - the render/hash/flush split is how the
 * scalar hash was caught costing 8 ms a frame, more than the transmit it
 * existed to reduce, and without it that would still be shipping.
 *
 * DESIGN.md records the opposite case: sh8601_last_frame was dropped by
 * --gc-sections while two cpu_cycles() pairs kept running per span to fill a
 * struct nobody read. The difference is that this cost is measured and stated
 * rather than accidental.
 */
typedef struct {
    uint32_t tiles_dirty;    /* tiles transmitted this frame        */
    uint32_t tiles_total;    /* tiles considered                    */
    uint32_t spans;          /* window setups issued                */
    uint32_t px_sent;        /* pixels transmitted                  */
    uint32_t render_cycles;  /* time inside the caller's rowfn      */
    uint32_t flush_cycles;   /* time pushing bytes to the panel     */
    uint32_t hash_cycles;    /* time deciding what changed          */
} tile_stats;

void tile_init(void);

/* Mark everything dirty - the next present is a full repaint. */
void tile_reset(void);

void tile_set_resync(uint32_t frames);

/*
 * Render through `rowfn` and transmit only the tiles that changed.
 *
 * `rowfn` is called for EVERY row, in increasing order, exactly once per
 * frame - which is what vg_rowfn's forward-only active-edge walk requires.
 * Returns SPI2_OK or the transport's error.
 */
int tile_present(void (*rowfn)(uint16_t *row, int y));

const tile_stats *tile_last(void);

#endif /* TILE_H_INCLUDED */
