# metal99

A **bare-metal graphics runtime** for the Waveshare ESP32-S3-Touch-AMOLED-1.8,
in **pure ISO C99** with **zero dependencies**.

No ESP-IDF. No FreeRTOS. No libc. No ROM calls. The firmware is loaded by the
mask ROM from flash offset `0x0` straight into SRAM, brings up its own clocks,
drives SPI2 and the SH8601 panel from raw registers, and renders through the
LX7's 128-bit vector unit.

```
 16 KB image  ·  0 dependencies  ·  -std=c99 -pedantic-errors -Wshadow -Werror
```

| | |
|---|---|
| **Interface apps** | **60 Hz**, 1.9 ms frames, 8x headroom |
| **Full-screen vector scenes** | **36 Hz**, 18.9 ms frames |
| **Touch** | two contacts, tracking ids, press/drag/release/tap/long |
| **Text** | TrueType-derived bitmap fonts, 1.375 instructions per pixel |
| **Tests** | 101 assertions, no board required |

---

## Quickstart

```sh
./tools/flash.sh -c 20             # build, flash to 0x0, capture 20s of output
APP=gridvoid ./tools/flash.sh      # pick which application ships
./tools/capture.py -r -s 15        # reset and watch the console
make -C tests/host test            # 101 assertions + C99 conformance check
make -C tests/host gamepng         # render the app to a PNG, no board required
```

No toolchain environment to source. `build.sh` invokes gcc, ld and esptool
directly; esptool only packs the image and is never linked in. It selects the
newest installed `xtensa-esp-elf` toolchain, prints which one, and honours
`METAL99_TOOLCHAIN`. The serial port is auto-detected by USB vendor ID;
`PORT=/dev/ttyACMn` or `capture.py -p` overrides it.

Rendering to PNG links the **real** `gfx`, `elide`, `vg`, `ui` and the app, so a
layout can be inspected pixel by pixel on the desktop in about 200 ms.

To restore the stock Waveshare firmware, see [`backup/RESTORE.md`](backup/RESTORE.md).

---

## Writing an application

An application is a name, a cadence and three callbacks:

```c
#include "app.h"

static void my_init(void)               { /* draw what never changes */ }
static int  my_frame(uint32_t f)        { /* DESCRIBE the scene, then present */
                                          return gfx_present(); }
static void my_event(const ui_event *e) { /* press/drag/release/tap/long */ }

const app_t APP = { "mine", 60u, my_init, my_frame, my_event };
```

Drop it in `metal99/apps/`, then `APP=mine ./tools/flash.sh`. Exactly one app is
linked per image — every app defines `const app_t APP`, so an application is a
file you swap rather than a branch inside `main.c`.

**`frame()` describes the entire scene, every frame.** That is not wasteful:
`gfx_present()` diffs the description against what the panel actually holds, so
redescribing something unchanged transmits nothing. Describe *state*, never
*steps* — the runtime works out what changed.

Two front ends, and an app picks one:

| | | |
|---|---|---|
| `gfx_*` | retained-mode runs, rects, text | returns `gfx_present()` |
| `vg_*` | scanline vector lines | returns `vg_present()` |

Events arrive before `frame()`. `ui_anchored_in()` is usually what a button
wants: a press that began on one control and lifted on another activates
neither.

Declare the cadence you can actually hold. `main.c` paces to `APP.hz` and
reports late frames, so the number means something.

---

## How it goes fast

The SH8601 keeps its own framebuffer. So an untouched pixel costs nothing, and
the runtime's job is not to draw quickly — it is to **know exactly what did not
change**.

> **The fastest pixel is the one never sent.**

Four layers, each closing a measured cost:

| | |
|---|---|
| **Vector MMIO** | 128-bit PIE stores fill the FIFO and the framebuffer |
| **Elision** | present-time diff of the description against panel state |
| **Sub-width spans** | a changed element costs its own area, not full rows |
| **Cost-based coalescing** | adjacent rows merge when a union beats another span |

An interface frame lands in **1.9 ms** against a 16.67 ms budget. A full-screen
vector repaint is **22.2 ms** at 80 MHz; elision takes a typical game frame from
24.5 ms to **18.9 ms**, with a fifth of the rows never leaving the chip.

Dirtiness is **derived, never declared**. Callers cannot forget to invalidate,
because nothing asks them to.

---

## What's in the box

| Path | |
|---|---|
| `metal99/src/` | the runtime — 20 modules |
| `metal99/apps/` | applications; one links per image |
| `metal99/build.sh` | gcc + ld + esptool, no build system |
| `tools/` | instruments: capture, register lookup, ISA probe, font and trig generators |
| `tests/host/` | assertions, desktop renderers, ASAN soaks |
| `docs/DESIGN.md` | the engineering record — every measurement and method |
| `backup/` | verified stock flash image + restore instructions |

### Runtime modules

| | |
|---|---|
| `start` `clk` `wdt` `io` | boot, PLL, watchdogs, MMIO, console |
| `spi2` `gdma` `sh8601` | QSPI transport and the panel |
| `vec` `rgb565` `fold` | vector primitives, colour, digest |
| `elide` | the diff against panel state |
| `gfx` `font_share` | retained-mode layer and glyphs |
| `vg` `trig` | scanline vector renderer, fixed-point trig |
| `i2c` `touch` `ui` | FT3168 and gesture recognition |
| `selftest` | on-device verification, validated against injected faults |

### Applications

| | |
|---|---|
| `gridvoid` | a wireframe vector game — grid, craft, tracers, seven-segment HUD |
| `demo` | the interface showcase — runs, text, live touch readout |
| `uilab` | a layout bench for the retained-mode layer |
| `spanlab` `storeprobe` `readprobe` `tescan` | hardware instruments |

---

## Text

Fonts are rasterised from TrueType **at build time** by `tools/mkfont.py`, never
on the device. Bits blit at **1.375 instructions per pixel**, measured.

```sh
./tools/mkfont.py --list     # what gets generated
./tools/mkfont.py            # regenerate metal99/src/font_share.c
```

Build-time generation also makes the typeface a choice rather than whatever
console font happens to be installed — change size, weight or family by editing
one table in the generator. `Share Tech Mono` ships; the proportional `Share
Tech` is tracked alongside it.

---

## Verification

The panel cannot be read back — measured, not assumed: seven DCS read commands
across eight dummy-cycle counts and four reply paths, all silent. So the runtime
verifies **what the hardware was actually told to send**.

```sh
make -C tests/host test      # 101 assertions + clang C99 conformance check
```

- **On-device self-test** compares transmitted bytes against an independently
  computed digest on both transports, then proves the digest **rejects** injected
  1-bit, duplicate and shift faults.
- **Host harness** links the real firmware sources and stubs only the transport,
  so tests exercise shipping code.
- **ASAN soaks** run thousands of frames of real gameplay with input arriving.
- **Desktop renderers** turn any app into a PNG for pixel inspection.
- **`tools/c99check.sh`** compiles every source under a second, independent front
  end at strict ISO C99, and enumerates the extension surface so it cannot drift.

---

## Hardware notes

> **Board revision matters.** This targets the original **SH8601/FT3168**
> revision. Waveshare's BSP switched to CO5300/CST816 at 2.0.3, and every
> upstream example pins `^2.0.3`. Those build cleanly and drive the wrong
> controller — a dark or garbled screen with no error.

All measured on hardware:

- **80 MHz QSPI works.** The panel accepts it; every transport self-test passes,
  and a full repaint drops from 31.4 ms to 22.2 ms.
- **The panel keeps its framebuffer** across CPU resets *and* software reset —
  which is exactly why elision works.
- **Touch is FT3168 at 0x38**, I2C on GPIO14/15, INT active-low on GPIO21, no
  reset line. The V2 board has a CST816 at the same address, so the driver checks
  the vendor ID rather than assuming.
- **No TE pin is routed** on this board — confirmed by scanning every free pad
  for a refresh-rate signal — so cadence comes from our own timebase.

---

## Contributing

Two non-negotiable rules — pure C99, and no scalar per-element math — plus a
verification discipline earned the hard way. See
[CONTRIBUTING.md](CONTRIBUTING.md), and [`docs/DESIGN.md`](docs/DESIGN.md) for
the full engineering record.

## License

MIT — see [LICENSE](LICENSE).

The bundled bitmap font is derived from **Share Tech Mono**, © The Share Tech
Mono Project Authors, under the **SIL Open Font License 1.1** — see
[`tools/fonts/OFL.txt`](tools/fonts/OFL.txt). The TTF sources are tracked so
`font_share.c` is reproducible.
