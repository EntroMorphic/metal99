/*
 * RETIRED 2026-08-26 from metal99/src/sh8601.c + sh8601.h.
 *
 * MIPI DCS vertical scroll: 0x33 VSCRDEF (top-fixed / scroll-area / bottom-
 * fixed) and 0x37 VSCRSAR (scroll start row).
 *
 * WHY IT WAS WANTED. If the panel implemented these, scrolling a list would
 * cost ONE command instead of a full repaint - an asymptote change rather than
 * a multiplier, and by far the largest single win ever proposed for this
 * runtime.
 *
 * WHY IT IS HERE. It does not work. Tested on this panel: the bars did not
 * move (docs/DESIGN.md 4). The commands are accepted - sh8601_cmd returns
 * SPI2_OK - and nothing happens, which is the SH8601's usual way of declining.
 *
 * The code is correct as far as anyone can tell; it is the panel that does not
 * implement the feature. Kept because "we tried panel-side scroll and it does
 * nothing" is worth more as working code plus a result than as a sentence, and
 * because a future board revision might answer differently.
 */

int sh8601_scroll_def(uint16_t tfa, uint16_t vsa, uint16_t bfa)
{
    uint8_t VEC_ALIGN p[16];
    p[0] = (uint8_t)(tfa >> 8); p[1] = (uint8_t)(tfa & 0xFFu);
    p[2] = (uint8_t)(vsa >> 8); p[3] = (uint8_t)(vsa & 0xFFu);
    p[4] = (uint8_t)(bfa >> 8); p[5] = (uint8_t)(bfa & 0xFFu);
    return sh8601_cmd(0x33u, p, 6u);
}

int sh8601_scroll_start(uint16_t row)
{
    uint8_t VEC_ALIGN p[16];
    p[0] = (uint8_t)(row >> 8);
    p[1] = (uint8_t)(row & 0xFFu);
    return sh8601_cmd(0x37u, p, 2u);
}

/* Declarations as they stood in sh8601.h:
 *
 *   int sh8601_scroll_def(uint16_t tfa, uint16_t vsa, uint16_t bfa);
 *   int sh8601_scroll_start(uint16_t row);
 */
