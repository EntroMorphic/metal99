# archive

**Nothing here is dead.** These are superseded stages and retired code, kept
for reference, and still load-bearing as documentation.

## `retired/`

Code removed from the shipping tree, with the measurement that removed it. The
rule is that nothing leaves this project by deletion: a function that was
written, tested and then displaced is a recorded result, and the reason it went
is usually worth more than the code.

| | |
|---|---|
| `sh8601_scroll.c` | MIPI DCS `0x33`/`0x37` panel-side scroll. Correct code; the panel does not implement it. Tested - the bars did not move. |
| `vg_present.c` | Row-granular elision for vector scenes. Correct and proven pixel-identical over 6000 frames; superseded by `tile_present`, which sends 27.6% of pixels where this sent 88.7%. |

`vg_present.c` also carries the two tuning constants `VG_MIN_RUN` and
`VG_MERGE_GAP`, which are the live open question in this project: they were
derived from the belief that short spans cause debris, they demonstrably fixed
it, and `tile_present` then ran 29 spans of eight rows on the same panel with
no debris at all. Both results stand. See `docs/DESIGN.md` 11.4.

## `lvgl_demo_sh8601/`

Waveshare's LVGL demo with the BSP pinned to **2.0.0**. This is the ground truth
for the panel: the SH8601 QSPI init sequence, the FT3168 touch bring-up and the
pin map were all read out of here.

> **The pin matters.** Waveshare's BSP switched to a CO5300/CST816 panel at
> version **2.0.3**, and every upstream example pins `^2.0.3`. Those build
> cleanly and drive the wrong controller - a dark or garbled screen with no
> error. `dependencies.lock` is tracked to hold the 2.0.0 pin.

## `bare_metal_fb/`

The ESP-IDF stepping stone: own framebuffer and rasterisers, no LVGL, no
FreeRTOS, strict ISO C99 behind a hand-written `platform.h`. It proved the
project's C99 constraint was achievable before `metal99/` removed ESP-IDF
entirely.

It also holds the measurements the design was reasoned from - the ESP-IDF
transport numbers in `docs/DESIGN.md` §4 come from this build, which is exactly
why that section carries a provenance warning. A number without its transport is
not data.

## Rebuilding either

```sh
export PATH=/usr/bin:/bin:$PATH      # ahead of conda's python
. ~/esp/v5.5/export.sh
idf.py -p /dev/ttyACM1 flash
```

Both will overwrite `metal99` on the device. `backup/RESTORE.md` restores the
stock firmware.
