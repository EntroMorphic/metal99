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
 *      band   TILE_H rows x 368 px x 2 B    5.9 KB at TILE_H 8
 *      hashes 46 x 56 x 4 B                10.3 KB at 8x8
 *
 * 16 KB, against 322 KB for a framebuffer. No memory-map change, no IRAM
 * resize, no going near the ROM stack floor.
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
#define TILE_H 16

/*
 * Resync period in frames, 0 to disable. Same reasoning as elide: the model of
 * remote state cannot be verified, so a rotating slice is rewritten on a
 * schedule and drift cannot persist. It also covers the one way this scheme
 * can be wrong that elide's cannot - two different tiles hashing the same.
 */
#define TILE_RESYNC_FRAMES 120u

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
