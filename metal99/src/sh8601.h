/*
 * SH8601 AMOLED controller over QSPI. Pure ISO C99.
 *
 * Wire framing (from the panel's QSPI mode, not the SPI command phase):
 *   command word = { opcode, 0x00, cmd, 0x00 }  sent on ONE line
 *     opcode 0x02 -> parameter write, params follow on one line
 *     opcode 0x32 -> pixel write   , pixels follow on FOUR lines (cmd 0x2C)
 * CS must stay asserted from the command word through the last data byte.
 * Every function here owns that, so no caller ever composes CS by hand.
 */
#ifndef SH8601_PANEL_H
#define SH8601_PANEL_H

#include <stdint.h>

#define SH8601_WIDTH  368
#define SH8601_HEIGHT 448

/* Send a command with optional parameters. params may be NULL when n == 0. */
int sh8601_cmd(uint8_t cmd, const uint8_t *params, uint32_t n);

/* 0x51 - display brightness, 0..255. */
int sh8601_brightness(uint8_t level);

/* 0x2A / 0x2B - address window. Inclusive coordinates. */
int sh8601_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* Pack to the panel's wire format: RGB565, big-endian. */
uint16_t sh8601_rgb565(uint8_t r, uint8_t g, uint8_t b);

/*
 * Stream one full frame. rowfn fills a SH8601_WIDTH-pixel row for each y.
 *
 * Owns the entire CS sequence: the 0x32/0x2C command word, every 64-byte
 * chunk with CS held, and the release on the final chunk. Callers never
 * compose CS, and a full framebuffer is never required - only one row.
 */
int sh8601_write_frame(void (*rowfn)(uint16_t *row, int y));

/*
 * Write ONE horizontal span, rows y0..y1 inclusive, full width.
 *
 * This is the elision primitive. The panel keeps its own framebuffer, so rows
 * outside the span keep whatever they held - untouched pixels cost nothing.
 * Full width because our renderer produces whole rows and the address window
 * is a rectangle; sub-width tiles would need row extraction and are a later
 * refinement.
 */
int sh8601_write_span(uint16_t y0, uint16_t y1, void (*rowfn)(uint16_t *row, int y));

/*
 * Write a RECTANGLE: rows y0..y1, columns x0..x1, all inclusive.
 *
 * The address window (0x2A/0x2B) has always taken x0/x1 - this driver simply
 * never used it, passing 0 and WIDTH-1 on every call, so a 10x10 element cost
 * ten full-width rows. Sub-width costs the element's own pixels instead.
 *
 * ALIGNMENT: x0 and the width must be multiples of GFX_XGRID (8 px = 16 B).
 * The FIFO is loaded with vector stores, so the sub-row's byte offset has to be
 * 16-byte aligned (spi2.h). Snapped outward here rather than rejected, because
 * a silently clipped rectangle is far worse than a slightly wide one.
 *
 * Banded DMA only applies to full-width spans - a sub-width band is not
 * contiguous in memory and would need one descriptor per row, past the chain
 * capacity. Sub-width spans therefore always take the FIFO path, which is the
 * shipping transport anyway (DESIGN.md 6.6l).
 */
int sh8601_write_span_x(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        void (*rowfn)(uint16_t *row, int y));

/*
 * Full power-up sequence. There is NO reset pin on this board, so 0x11
 * (sleep out) is the only reset path and its 120 ms settle is a MINIMUM.
 */
int sh8601_init(void);

/* Force the panel down: display off, then sleep in. Used to prove sh8601_init
 * actually brings the panel up rather than inheriting a working state. */
int sh8601_sleep(void);

/*
 * NO PANEL-SIDE SCROLL. sh8601_scroll_def() and sh8601_scroll_start() used to
 * live here, wrapping MIPI DCS 0x33 VSCRDEF and 0x37 VSCRSAR, under a comment
 * that began "If the panel implements these..." - which was written before it
 * was tested and never updated after. It was tested: the bars did not move
 * (DESIGN.md 4). This panel does not implement them.
 *
 * Removed rather than left in place, because dead code for a DISPROVEN feature
 * is worse than no code at all: the header was inviting the next person to
 * spend an afternoon on an idea this project had already closed.
 */

/*
 * Telemetry from the REAL workload - no synthetic probe traffic. Measuring the
 * actual work is both more honest and structurally incapable of corrupting the
 * panel, which synthetic probing did.
 *
 * PER SPAN, NOT PER FRAME. This is reset and refilled by every
 * sh8601_write_span() call, and elide_flush() issues one span per contiguous
 * dirty run - so reading it after a frame gives you the LAST span only. The
 * header used to say "read after each frame", which was true when write_frame()
 * was the only caller and quietly stopped being true when elision landed.
 * elide_stats accumulates these across a frame; use that for frame figures.
 */
typedef struct {
    uint32_t render_cycles;   /* time inside the caller's rowfn        */
    uint32_t flush_cycles;    /* time pushing bytes to the panel       */
    uint32_t total_cycles;    /* whole span, including command setup   */
    uint32_t bytes;           /* pixel bytes actually transmitted      */
} sh8601_stats;

const sh8601_stats *sh8601_last_frame(void);

/*
 * Select the pixel transport. Both are kept so the same workload can be
 * measured through each - a controlled comparison beats a remembered number.
 *   0 = 64-byte FIFO (vectorised MMIO load)
 *   1 = GDMA descriptor chain
 */
void sh8601_set_dma(int on);

/*
 * Enable/disable render-DMA overlap. Exists to VERIFY the overlap claim:
 * the normal telemetry cannot distinguish overlap from no-overlap, because
 * flush is measured as time spent waiting, which rendering already shortened.
 * With overlap off, each band's DMA is collected before the next is rendered.
 */
void sh8601_set_overlap(int on);

#endif /* SH8601_PANEL_H */
