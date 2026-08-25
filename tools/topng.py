#!/usr/bin/env python3
"""Convert the host renderer's PPM output to PNG.

PPM is what the C side can write without a dependency; PNG is what anything
else can open. Kept as a tracked tool rather than an inline one-liner, for the
same reason capture.py is.
"""
import sys
try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow missing: pip install Pillow")

for p in sys.argv[1:]:
    out = p.rsplit(".", 1)[0] + ".png"
    Image.open(p).save(out)
    print("  %s" % out)
