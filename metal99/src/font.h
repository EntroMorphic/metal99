/*
 * 1bpp bitmap fonts. Pure ISO C99.
 *
 * Rasterised from TrueType at BUILD time by tools/mkfont.py, never on the
 * device. A TrueType scan converter wants malloc, libc and floating point -
 * none of which exist here - and outline filling is irreducibly scalar
 * per-element work, which DESIGN.md 3.0 says must stay negligible or the
 * wire-bound thesis stops holding. Bits blit at 1.375 instructions per pixel
 * (DESIGN.md 6.9a); outlines would not come close.
 *
 * LAYOUT. Row-major, MSB first: bit 7 of the first byte is the leftmost pixel.
 * That is the order the device's lanebit constant {0x80,0x40,...,0x01} expects,
 * so no reversal happens at run time.
 *
 *   glyph g, row r, byte b  ->  bits[((g * h) + r) * (w / 8) + b]
 *
 * WIDTH IS A MULTIPLE OF 8 by construction. A glyph then occupies whole
 * 128-bit vectors when placed on the 8px grid, so the blit needs no masking
 * and no unaligned path.
 */
#ifndef FONT_H
#define FONT_H

#include <stdint.h>

typedef struct {
    uint8_t        w;      /* glyph width in px, multiple of 8 */
    uint8_t        h;      /* glyph height in rows             */
    uint8_t        first;  /* codepoint of glyph 0             */
    uint8_t        count;  /* number of glyphs                 */
    const uint8_t *bits;
} gfx_font;

/* Share Tech Mono, SIL OFL 1.1 - see tools/fonts/OFL.txt. */
extern const gfx_font share_mono_8x16;
extern const gfx_font share_mono_16x32;

#endif /* FONT_H */
