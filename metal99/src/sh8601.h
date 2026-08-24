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
 * Full power-up sequence. There is NO reset pin on this board, so 0x11
 * (sleep out) is the only reset path and its 120 ms settle is a MINIMUM.
 */
int sh8601_init(void);

/* Force the panel down: display off, then sleep in. Used to prove sh8601_init
 * actually brings the panel up rather than inheriting a working state. */
int sh8601_sleep(void);

/*
 * MIPI DCS vertical scroll. If the panel implements these, scrolling costs ONE
 * command instead of a repaint - an asymptote change, not a multiplier.
 *   0x33 VSCRDEF  : top-fixed, scroll-area, bottom-fixed (rows)
 *   0x37 VSCRSAR  : scroll start row
 */
int sh8601_scroll_def(uint16_t tfa, uint16_t vsa, uint16_t bfa);
int sh8601_scroll_start(uint16_t row);

/*
 * Telemetry from the REAL workload - no synthetic probe traffic. Populated by
 * sh8601_write_frame(); read after each frame. Measuring the actual work is
 * both more honest and structurally incapable of corrupting the panel, which
 * synthetic probing did.
 */
typedef struct {
    uint32_t render_cycles;   /* time inside the caller's rowfn        */
    uint32_t flush_cycles;    /* time pushing bytes to the panel       */
    uint32_t total_cycles;    /* whole frame, including command setup  */
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

#endif /* SH8601_PANEL_H */
