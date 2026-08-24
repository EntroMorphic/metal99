# archive

**Nothing here is dead.** These are superseded stages kept for reference, and
both are still load-bearing as documentation.

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
