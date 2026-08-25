#!/usr/bin/env python3
"""Rasterise a TTF into a 1bpp bitmap font for metal99.

WHY BUILD-TIME. metal99 could not rasterise TrueType on the device: a scan
converter wants malloc, libc and floating point, none of which exist here, and
outline filling is irreducibly scalar per-element work. DESIGN.md 3.0 says
compute has to stay negligible or the wire-bound thesis stops holding. So the
outlines are rasterised once, here, and the device blits bits - 1.375
instructions per pixel, measured (DESIGN.md 6.9a).

Doing it at build time also means the typeface is a CHOICE rather than whatever
console font happened to be installed, and its licence is one we picked.

  ./tools/mkfont.py --list                    what will be generated
  ./tools/mkfont.py                           regenerate metal99/src/font_share.c
"""
import argparse, sys
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow missing: pip install Pillow")

FIRST, LAST = 0x20, 0x7E          # printable ASCII
PROBE = 48                        # scratch margin when measuring ink extent

# (C identifier, ttf, pixel size, cell width, cell height)
# Sizes chosen by rendering and LOOKING at the 1bpp result, not from metrics:
# advance lands within a hair of the cell and stems stay unbroken.
FONTS = [
    ("share_mono_8x16",  "tools/fonts/ShareTechMono-Regular.ttf", 15,  8, 16),
    ("share_mono_16x32", "tools/fonts/ShareTechMono-Regular.ttf", 29, 16, 32),
]

# tools/fonts/ShareTech-Regular.ttf is tracked but NOT generated, deliberately.
# It is proportional, and gfx's label layer places glyph c at x + c*font->w -
# one uniform advance. Proportional text needs a per-glyph advance table, and
# that breaks the property the blit depends on: every glyph starting on the 8px
# grid, so it occupies whole 128-bit vectors with no masking and no unaligned
# path. Supporting it means either accepting sub-grid placement (a masked blit,
# which the ISA can do - see DESIGN.md 6.9b) or quantising advances to 8px and
# accepting loose spacing. Neither is hard; both are decisions, so the font sits
# here until one is made.


def ink_box(font):
    """Union ink bbox over printable ASCII, relative to the pen origin.

    MEASURED, NOT TAKEN FROM METRICS. The first version placed the baseline
    from font.getmetrics() - ascent if it fitted, else ch - descent. Those are
    NOMINAL values that need not match where the outlines actually put ink, and
    for Share Tech Mono at 15 px they did not: ascent+descent came to 18 against
    a 16-row cell, the fallback put the baseline at row 12, and eleven glyphs -
    $ ( ) / [ \\ ] ^ { | } - lost their top row. Silently. The bitmaps were
    wrong and nothing said so, because a clipped bracket still looks like a
    bracket until you compare it with one that is not.
    """
    lo_x = hi_x = lo_y = hi_y = None
    for cp in range(FIRST, LAST + 1):
        img = Image.new("L", (PROBE * 3, PROBE * 3), 0)
        ImageDraw.Draw(img).text((PROBE, PROBE), chr(cp), font=font, fill=255,
                                 anchor="ls")
        bb = img.getbbox()
        if bb is None:
            continue                                  # blank, e.g. space
        x0, y0, x1, y1 = (bb[0] - PROBE, bb[1] - PROBE,
                          bb[2] - PROBE, bb[3] - PROBE)
        lo_x = x0 if lo_x is None else min(lo_x, x0)
        hi_x = x1 if hi_x is None else max(hi_x, x1)
        lo_y = y0 if lo_y is None else min(lo_y, y0)
        hi_y = y1 if hi_y is None else max(hi_y, y1)
    return lo_x, hi_x, lo_y, hi_y


def render(ttf, px, cw, ch):
    """Return [bytes] per glyph, row-major, (cw/8) bytes per row."""
    font = ImageFont.truetype(ttf, px)
    lo_x, hi_x, lo_y, hi_y = ink_box(font)
    adv = font.getlength("M")

    # Baseline placed so the TOPMOST ink in the whole set lands on row 0, and
    # the pen shifted right if any glyph reaches left of the origin.
    base = -lo_y
    xoff = -lo_x if lo_x < 0 else 0

    # REFUSE to generate a font that does not fit. Clipping here is invisible
    # downstream: the device blits whatever bytes it is given.
    ink_w, ink_h = hi_x - lo_x, hi_y - lo_y
    if ink_h > ch or ink_w > cw or adv > cw + 0.999:
        raise SystemExit(
            "%s at %dpx does not fit a %dx%d cell:\n"
            "  ink %dx%d, advance %.2f  (need ink <= %dx%d, advance <= %d)"
            % (ttf.split("/")[-1], px, cw, ch, ink_w, ink_h, adv, cw, ch, cw))

    bpr = cw // 8
    out = []
    for cp in range(FIRST, LAST + 1):
        img = Image.new("L", (cw, ch), 0)
        ImageDraw.Draw(img).text((xoff, base), chr(cp), font=font, fill=255,
                                 anchor="ls")
        px_ = img.load()
        g = bytearray()
        for y in range(ch):
            for b in range(bpr):
                v = 0
                for bit in range(8):
                    x = b * 8 + bit
                    # MSB first: bit 7 is the leftmost pixel, which is what the
                    # device's lanebit constant {0x80,0x40,...,0x01} expects.
                    if px_[x, y] >= 128:
                        v |= 0x80 >> bit
                g.append(v)
        out.append(bytes(g))
    return out


def emit(fh, name, ttf, px, cw, ch, glyphs):
    bpr = cw // 8
    fh.write("\n/* %s - %s at %d px, %dx%d cell, %d glyphs, %d bytes.\n"
             " * Baseline measured from actual ink extent, not font metrics;\n"
             " * the generator refuses to emit a font whose ink does not fit. */\n"
             % (name, ttf.split("/")[-1], px, cw, ch, len(glyphs),
                len(glyphs) * ch * bpr))
    fh.write("static const uint8_t %s_bits[] = {\n" % name)
    for cp, g in zip(range(FIRST, LAST + 1), glyphs):
        ch_repr = "space" if cp == 0x20 else chr(cp)
        fh.write("    /* 0x%02X %s */\n" % (cp, ch_repr))
        for r in range(ch):
            row = g[r * bpr:(r + 1) * bpr]
            fh.write("    " + "".join("0x%02X," % b for b in row) + "\n")
    fh.write("};\n")
    fh.write("const gfx_font %s = { %d, %d, 0x%02X, %d, %s_bits };\n"
             % (name, cw, ch, FIRST, len(glyphs), name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-o", "--out", default="metal99/src/font_share.c")
    a = ap.parse_args()

    if a.list:
        for name, ttf, px, cw, ch in FONTS:
            print("  %-18s %-40s %2dpx  %dx%d  %5d B"
                  % (name, ttf, px, cw, ch, (LAST - FIRST + 1) * ch * (cw // 8)))
        return

    with open(a.out, "w") as fh:
        fh.write("/* GENERATED by tools/mkfont.py - do not edit.\n"
                 " *\n"
                 " * Share Tech Mono, Copyright (c) The Share Tech Mono Project\n"
                 " * Authors, licensed under the SIL Open Font License 1.1.\n"
                 " * Licence text: tools/fonts/OFL.txt. The TTF sources are tracked\n"
                 " * in tools/fonts/ so this file is reproducible.\n"
                 " *\n"
                 " * Rasterised on the host because the device cannot: a TrueType\n"
                 " * scan converter wants malloc, libc and floating point, and\n"
                 " * outline filling is scalar per-element work. See tools/mkfont.py.\n"
                 " */\n#include \"font.h\"\n")
        for name, ttf, px, cw, ch in FONTS:
            emit(fh, name, ttf, px, cw, ch, render(ttf, px, cw, ch))
    print("wrote %s" % a.out)


if __name__ == "__main__":
    main()
