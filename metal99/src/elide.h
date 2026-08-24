/*
 * Elision: send only what changed. Pure ISO C99.
 *
 * WHY THIS EXISTS. 40 MHz is the panel's ceiling (docs/DESIGN.md 6.6h), so a
 * full frame costs 16.49 ms of wire time against a 16.67 ms budget for 60 Hz.
 * Full-frame 60 fps is arithmetically impossible. But the panel keeps its own
 * framebuffer - the exact property that produced ghost images and fooled us
 * three times - so untouched pixels cost NOTHING. The fastest pixel is the one
 * never sent.
 *
 * MODEL. The renderer DECLARES which rows it touched. There is no per-pixel
 * diffing: we have no framebuffer to diff against, and at any CPU clock the
 * comparison would cost more than the transfer it saves.
 *
 * RESYNC IS NOT OPTIONAL. A dirty-region scheme is a model of remote state we
 * cannot read back. If the model drifts from reality we get precisely the
 * failure that has caught this project five times: looks right, isn't. So a
 * full repaint is forced every ELIDE_RESYNC_FRAMES, and elide_reset() is
 * always available as an escape hatch.
 */
#ifndef ELIDE_H
#define ELIDE_H

#include <stdint.h>
#include "sh8601.h"

/*
 * ROLLING RESYNC.
 *
 * The first design forced a FULL repaint every 120 frames. That works, but a
 * full frame costs 16.6 ms against a 16.67 ms budget, so every resync frame
 * missed its 60 Hz deadline - measured as 19 late frames against 14 resyncs.
 *
 * Instead, refresh a rotating slice every frame. The whole screen is still
 * rewritten within ELIDE_RESYNC_FRAMES, so drift still cannot persist, but the
 * cost is spread evenly instead of arriving as a 16.6 ms spike.
 */
#define ELIDE_RESYNC_FRAMES 120u
#define ELIDE_RESYNC_ROWS   ((SH8601_HEIGHT + ELIDE_RESYNC_FRAMES - 1) / ELIDE_RESYNC_FRAMES)

typedef struct {
    uint32_t rows_sent;      /* rows actually transmitted this frame  */
    uint32_t spans;          /* contiguous dirty runs                 */
    uint32_t bytes;          /* pixel bytes transmitted               */
    uint32_t cycles;         /* whole update                          */
    uint32_t resync_rows;    /* rows added by the rolling resync      */
} elide_stats;

void elide_init(void);

/*
 * Resync period in frames; 0 disables it entirely.
 *
 * Disabling is a TEST ONLY: it removes the safety net so that dirty-tracking
 * drift becomes visible instead of being scrubbed every two seconds. If the
 * display stays correct with resync off, the tracking is genuinely right
 * rather than merely being repaired often enough to look right.
 */
void elide_set_resync(uint32_t frames);

/* Mark rows y0..y1 inclusive as needing transmission. */
void elide_mark(int y0, int y1);

/* Mark everything dirty - full repaint next flush. */
void elide_reset(void);

/* Transmit only the dirty spans. Clears them on success. */
int  elide_flush(void (*rowfn)(uint16_t *row, int y));

const elide_stats *elide_last(void);

#endif /* ELIDE_H */
