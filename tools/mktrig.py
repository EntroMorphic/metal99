#!/usr/bin/env python3
"""Regenerate metal99/src/trig.c's quarter-turn sine table.

Tracked rather than inline so the table can be checked rather than trusted -
the same rule that put mkfont.py in here. 256 units to a full turn, 64 steps
per quadrant, Q16 output.
"""
import math
vals = [int(round(math.sin(i * (math.pi / 2) / 64) * 65536)) for i in range(65)]
for i in range(0, 65, 8):
    print("    " + " ".join("%6d," % v for v in vals[i:i + 8]))
