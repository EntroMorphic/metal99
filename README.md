# metal99

A **60 Hz graphics runtime** for the Waveshare ESP32-S3-Touch-AMOLED-1.8, in
**pure ISO C99** with **zero dependencies**.

No ESP-IDF. No FreeRTOS. No libc. No ROM calls. The firmware is loaded by the
mask ROM from flash offset `0x0` straight into SRAM, brings up its own clocks,
drives SPI2 and the SH8601 panel from raw registers, and renders through the
LX7's 128-bit vector unit.

```
 sub-10 KB boot image  ·  0 dependencies  ·  -std=c99 -pedantic-errors -Wshadow -Werror
```

## 60 Hz

**Locked 60 Hz on a 40 MHz bus, with 2.2x headroom, on a panel whose full-frame
wire time is 16.49 ms against a 16.67 ms budget.** The margin does not come from
a faster transport. It comes from not sending most of the frame.

The SH8601 keeps its own framebuffer — across CPU resets *and* software reset.
That property caused three "ghost image" misdiagnoses before it became the
foundation of the design: **an untouched pixel costs nothing, so the fastest
pixel is the one never transmitted.** The runtime's job is not to draw quickly.
It is to know exactly what did not change.

Four layers, each closing a measured cost:

| | |
|---|---|
| `gfx` | keeps two 448-row models — what the caller described, and what the panel last received — and **derives** dirtiness by diffing them at present time. A caller cannot mismark what it never marks, and cannot make a frame expensive by describing it in an awkward order. Rows are run lists, so rectangles compose. |
| `text` | labels are **described**, not rasterised into the model — position, colour, font and the string itself, diffed exactly like a run list. Glyphs blit transparently over whatever the runs drew. |
| `elide` | carries an x-extent per dirty row and coalesces rows into **rectangular spans**, so an update costs its own columns rather than whole rows. A rolling resync refreshes a rotating slice each frame, so model drift cannot persist. |
| `sh8601` | streams a span straight to the panel. There is **no framebuffer in RAM** — not because 322 KB will not fit (it does), but because storing pixels does not send fewer of them. |
| pacing | holds the 60 Hz cadence from our own timebase (no TE pin is wired) and counts every miss, rather than drifting quietly. |

Measured on hardware:

| | |
|---|---|
| Steady-state cadence | **60 Hz locked — zero late frames** |
| Moving a full-width 96-row bar | **8 rows, 2,944 px — 0.6 ms, 26x headroom** |
| Moving an 88x88 element | **1,968 px — 2% of the budget** |
| A **stationary** element | **0 rows transmitted** |
| Text set to the same string again | **0 px** — a static label is free |
| Updating a 5-digit 16x32 counter | **2,560 px** — 1.6% of a full screen |
| The same frame drawn twice | **0 rows transmitted** |
| A change made and reverted before present | **0 rows transmitted** |
| A full 448-row repaint | 31.2 ms — the one case that misses |

A moving element costs its **leading and trailing edges**, not its area: a
96-row bar stepping 4 px changes 8 rows, not 96. That is what diffing against
what the panel actually holds buys, and it is why the headroom on real motion is
26x rather than 2x.

**Elision is what makes 60 Hz reachable at all.** Repainting every frame costs
**31.2 ms** — 32 fps, measured, not extrapolated. Sending only what changed costs
**7.3 ms**. That 4.5x is the architecture, and nothing else in the stack closes
that gap: the bus is fixed at 40 MHz, the CPU is already at 160, and rendering is
**0.002 ms/row** — 1.4% of a frame. The panel is wire-bound, so the only lever
that matters is how few bytes go over the wire.

### Budgeting a design

Cost is **linear at 0.00019 ms per pixel transmitted**, so an interface can be
budgeted directly. One 60 Hz frame buys about **88,900 pixels** — 54% of the
screen. Measured, for one step of continuous motion:

| workload | px/frame | of the 60 Hz budget |
|---|---|---|
| 88x88 element moving | **1,968** | 2% |
| full-width 96-row bar moving | **4,931** | 6% |
| the rolling resync alone | 1,472 | 2% |
| 240 full rows | 88,320 | 99% — the boundary |
| 448 rows (everything) | 164,864 | 185% — misses |

Interfaces do not repaint whole screens. Only workloads touching most of the
screen every frame — fullscreen video, a scrolling background — exceed the
budget.

**The rolling resync is now the floor.** It refreshes 4 full-width rows every
frame — 1,472 px — which is 75% of that 88x88 element's cost. The safety net
outweighs the work, which is what elision being this cheap looks like. It is
tunable (`ELIDE_RESYNC_FRAMES`) and deliberately left alone: it is what makes
the dirty-tracking model self-correcting.

Banded GDMA closes that last case at 16.6 ms full-frame, and is **parked**: it
ships byte-identical data (the on-device self-test digests 448 rows to
`0xF5642645` through both transports) yet the panel looks visibly worse, so the
fault is delivery timing and is not understood. See
[`docs/DESIGN.md`](docs/DESIGN.md) §6.6l. Nothing above depends on it.

### The dirty tracking is correct, not merely repaired

A dirty-region scheme is a model of remote state that cannot be read back, so
"looks right" is worth little — the rolling resync rewrites the whole screen
every ~1.9 s while the bar wraps every ~1.5 s, which means a marking leak would
be scrubbed at about the rate it accumulates. That is exactly how an earlier
marking bug hid.

So the safety net is switched off and the load-bearing case is traced: over 20 s
with resync disabled, **all 13 wraps marked exactly 192 rows in 2 spans** — the
precise union of the element's old and new positions. Correct on its own, not
correct because something kept fixing it.

## Status

| | |
|---|---|
| Boot, clocks, watchdogs | verified |
| SPI2 QSPI @ 40 MHz, quad mode | verified by timing |
| SH8601 panel, cold start | verified by power cycle |
| 128-bit vector primitives | verified |
| Elision + 60 Hz pacing | **shipping** — 60 Hz locked, 2.2x headroom, zero late frames |
| Self-checking harness | validated against injected faults, on device and on host |
| `gfx` retained-mode layer | **shipping** — dirtiness derived, redundant repaints cost 0 rows |
| Text, TTF-derived bitmap fonts | **shipping** — static text costs 0 px |
| Touch, FT3168 over I2C | **shipping** — two contacts, tracking ids, INT-gated |
| Banded GDMA | parked — data proven identical, timing suspect |

## Quickstart

```sh
./tools/flash.sh -c 20        # build, flash to 0x0, capture 20s of output
./tools/capture.py -r -s 15   # reset and watch the console
make -C tests/host test       # 73 assertions, no board required
make -C tests/host png        # render the app to images, no board required
```

`make png` writes `out_ideal.png` (what the model says), `out_panel.png` (what
a panel receiving only the marked spans would hold, resync off) and
`out_diff.png` (disagreements in magenta). It links the **real** `gfx`, `elide`,
`ui` and the app, so a layout can be looked at without hardware — for most of
this project's life that was the one thing that could not be done.

The serial port is auto-detected by USB vendor ID; `PORT=/dev/ttyACMn` or
`capture.py -p` overrides it.

No toolchain environment to source. `build.sh` invokes gcc, ld and esptool
directly; esptool only packs the image and is not linked in. It selects the
newest installed `xtensa-esp-elf` toolchain, prints which one, and honours
`METAL99_TOOLCHAIN`.

To restore the stock Waveshare firmware, see [`backup/RESTORE.md`](backup/RESTORE.md).

## Layout

| Path | |
|---|---|
| `metal99/src/` | the firmware — ~2,500 lines |
| `metal99/build.sh` | gcc + ld + esptool, no build system |
| `tools/` | research instruments (capture, register lookup, ISA probe, font generator) |
| `tools/fonts/` | TTF sources, so the generated font is reproducible |
| `tests/host/` | digest assertions + desktop renderers, ~200 ms iteration |
| `docs/DESIGN.md` | the engineering record — every measurement and trap |
| `docs/lmm/` | design exploration that produced the plan |
| `backup/` | verified stock flash image + restore instructions |
| `archive/` | superseded stages, kept as reference |

### Firmware modules

| | |
|---|---|
| `start.c` `wdt.c` | ROM entry, watchdog disable, `.bss` clear, `CPENABLE` |
| `clk.c` | CPU/PLL — 20 MHz boot to 160 MHz |
| `io.c` | USB-Serial-JTAG console, timing |
| `vec.c` | 128-bit vector primitives (`EE.*`) |
| `fold.c` | transmit-ledger content digest — pure C99, also built by the host tests |
| `spi2.c` | SPI2 QSPI from registers, transmit ledger |
| `gdma.c` | GDMA descriptor chains (parked) |
| `sh8601.c` | panel init, address window, row/span writes |
| `elide.c` | dirty-row tracking, span coalescing, rolling resync |
| `gfx.c` | retained-mode layer — dirtiness derived, not declared; runs, rects and text labels |
| `font.h` `font_share.c` | 1bpp bitmap fonts, rasterised from TTF at build time |
| `app.h` `app_demo.c` | the application boundary — three callbacks and a name |
| `ui.c` | input events: press/drag/release/tap/long, hit testing |
| `i2c.c` | I2C0 master from registers, bus recovery, scan |
| `touch.c` | FT3168 capacitive touch, two contacts |
| `selftest.c` | on-device verification + fault injection |

## How it got here

| Stage | Frame | fps | |
|---|---|---|---|
| FIFO, 20 MHz CPU | 311 ms | 3.2 | |
| + vectorised MMIO | 82 ms | 12.2 | |
| + GDMA | 28 ms | 35.5 | |
| + PLL 160 MHz | 18.4 ms | 54.4 | |
| + banding & overlap | 16.6 ms | 60.2 | parked |
| FIFO + PLL, full repaint | 31.2 ms | 31.9 | ships |
| **+ elision, 104-row update** | **7.3 ms** | **60 Hz locked** | **ships** |

Every figure is measured on hardware at 368x448 RGB565, 40 MHz QSPI, 160 MHz
CPU. `docs/DESIGN.md` carries the method and the wrong turns.

## Hardware notes

> **Board revision matters.** This targets the **original SH8601/FT3168**
> revision. Waveshare's BSP switched to CO5300/CST816 at **2.0.3**, and every
> upstream example pins `^2.0.3`. Those build cleanly and drive the wrong
> controller — a dark or garbled screen with **no error**.

Other findings, all measured rather than assumed:

- **40 MHz is the panel's ceiling.** 80 MHz corrupts it, and no intermediate
  rate exists — `MST_CLK_SEL` gives XTAL (40) or APB (80) with an integer
  divider.
- **No DDR** for GP-SPI master writes.
- **No panel-side scroll** — `0x33`/`0x37` are not implemented.
- **No TE pin** wired, so 60 Hz cadence comes from our own timebase.
- **The panel keeps its framebuffer** across CPU resets *and* software reset.
  That produced three "ghost image" misdiagnoses — and is exactly why elision
  works.
- **Touch is FT3168 at 0x38**, I2C on GPIO14/15, INT active-low on GPIO21, and
  **no reset line** — same as the panel. The V2 board has a CST816 at the same
  address, so the driver checks the vendor ID rather than assuming.
- **Debris at span boundaries is real and unexplained.** More spans per frame
  means more of it, so a changed text label marks full rows to keep them
  coalesced with their neighbours. See [`docs/DESIGN.md`](docs/DESIGN.md) §11.4
  — the workaround is documented in `gfx.c`, and the cause is still open.

## Contributing

Two non-negotiable rules — pure C99 and no scalar per-element math — plus a
verification discipline earned the hard way. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Writing an application

An app is three callbacks and a name:

```c
static void my_init(void)                 { /* draw what never changes */ }
static void my_frame(uint32_t f)          { /* DESCRIBE the whole scene */ }
static void my_event(const ui_event *e)   { /* press/drag/release/tap/long */ }

const app_t APP = { "mine", my_init, my_frame, my_event };
```

`frame()` describes the **entire** scene every frame. That is not wasteful:
`gfx_present` diffs the description against what the panel actually holds, so
redescribing something unchanged transmits nothing (§5.2). Incremental drawing
is not just unnecessary here, it is a bug source — telling the layer about a
*step* rather than a *state* is how erase-then-draw leaves debris.

Events arrive before `frame()`. `ui_anchored_in()` is usually what a button
wants: a press that began on one control and lifted on another activates
neither.

The same app links into `tests/host/gfx_png`, so it can be rendered and
inspected without a board.

## Text

Fonts are rasterised from TrueType **at build time** by `tools/mkfont.py`, never
on the device — a TrueType scan converter wants `malloc`, libc and floating
point, none of which exist here, and outline filling is irreducibly scalar work
that would break the wire-bound premise in §3.0. Bits blit at **1.375
instructions per pixel** measured; outlines would not come close.

```sh
./tools/mkfont.py --list     # what gets generated
./tools/mkfont.py            # regenerate metal99/src/font_share.c
```

Doing it at build time also makes the typeface a choice rather than whatever
console font happens to be installed. Change the size, the weight or the family
by editing one table in the generator.

`Share Tech` (proportional) is tracked alongside the mono but **not generated**.
The label layer places glyph *c* at `x + c*w` — one uniform advance — and every
glyph starting on the 8px grid is what lets the blit run with no masking and no
unaligned path. Proportional text needs a per-glyph advance table and either
sub-grid placement or quantised spacing. Neither is hard; both are decisions.

## License

MIT — see [LICENSE](LICENSE).

The bundled bitmap font is derived from **Share Tech Mono**, © The Share Tech
Mono Project Authors, under the **SIL Open Font License 1.1** — see
[`tools/fonts/OFL.txt`](tools/fonts/OFL.txt). The TTF sources are tracked in
`tools/fonts/` so `font_share.c` is reproducible.
