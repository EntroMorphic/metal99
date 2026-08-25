# metal99 — Design Specification

**Zero-dependency, pure ISO C99 graphics runtime for the Waveshare
ESP32-S3-Touch-AMOLED-1.8.**

Status: Milestone 1 complete and verified on hardware. Milestones 2+ specified
here, not yet built.
Last updated: 2026-08-24.

---

## 1. Purpose

Drive the board's 368x448 QSPI AMOLED from bare metal, with a message-passing
graphics layer on top, under two absolute constraints:

1. **Pure ISO C99.** Every source file compiles under
   `-std=c99 -pedantic-errors -Wall -Wextra -Werror`.
2. **Zero dependencies.** No ESP-IDF, no FreeRTOS, no libc, no ROM function
   calls. Our code talks to silicon directly.

The compiler, linker, and `esptool` (image packer) are *build tools*, not
runtime dependencies. `libgcc` is linked for compiler intrinsics such as
`__udivsi3`; this is the compiler's runtime, not a third-party library.

### 1.1 Non-goals

Explicitly out of scope, with reasons:

| Excluded | Reason |
|---|---|
| **PSRAM** | Octal PSRAM timing training is the single hardest part of a from-scratch S3 bring-up. Band rendering into internal SRAM is *already* the faster architecture (measured §4), so skipping PSRAM removes the largest risk at no cost. |
| **Interrupts** | Poll the SPI2 done bit. No vector table, no exception handlers, no `xt_ints`. |
| **240 MHz clock** | Deferred, but see the warning in §4.3 — the ROM default is **20 MHz**, not 240, so this is required before any performance target is reachable. |
| **Flash XIP / MMU / cache** | Image is RAM-resident. No cache configuration means no cache-coherency class of bug. |
| **BitNet / ML** | Out of scope for this project. |
| **Wi-Fi / BT** | Would require Espressif's closed-source blobs, violating constraint 2. |

---

## 2. Verified hardware facts

Everything in this section was read off the device or its stock firmware during
bring-up. Nothing here is quoted from a datasheet unless marked.

### 2.1 Silicon

| Property | Value | How verified |
|---|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2 | esptool chip probe |
| Cores | 2x Xtensa LX7 @ up to 240 MHz + ULP | eFuse `DIS_APP_CPU = False` |
| PSRAM | 8 MB octal, AP_3v3, 80 MHz | eFuse `PSRAM_CAP`, boot log |
| Flash | 16 MB quad, Winbond `ef:4018` | esptool flash probe |
| MAC | `30:ed:a0:ac:91:54` | esptool |
| Console | **USB-Serial-JTAG only — no UART bridge** | `lsusb` VID:PID `303a:1001` |

### 2.2 Display

| Property | Value |
|---|---|
| Controller | **SH8601** (NOT CO5300 — see §2.3) |
| Resolution | 368 x 448 |
| Interface | QSPI on SPI2, 40 MHz, SPI mode 0 |
| Pixel format | RGB565, **big-endian on the wire** (byte-swapped) |
| Reset pin | **None** (`BSP_LCD_RST = GPIO_NUM_NC`) |

Pin map:

| Signal | GPIO |
|---|---|
| CS | 12 |
| CLK (PCLK) | 11 |
| D0 | 4 |
| D1 | 5 |
| D2 | 6 |
| D3 | 7 |

> **Routing consequence.** The IO_MUX defaults for these pins are
> `GPIO11 = FSPID` and `GPIO12 = FSPICLK` — the board wires them the *other way
> round* (11 = CLK, 12 = CS). Direct IO_MUX routing is therefore **impossible**;
> all six signals must go through the **GPIO matrix**. See §6.3.

### 2.3 Board revision trap

This unit is the **original** revision: SH8601 display + FT3168 touch.
Waveshare's BSP switched to CO5300/CST816 at version **2.0.3**, and every
ESP-IDF example pins `^2.0.3`. Those builds succeed and drive the *wrong panel*
— producing a dark or garbled screen with **no error message**.

Relevant here because the SH8601 init sequence in §6.5 is transcribed from BSP
**2.0.0**. If that sequence is ever re-derived from upstream, confirm the
version first.

---

## 3. Architecture

### 3.0 The 60 Hz thesis

**The panel is wire-bound, so the only lever that matters is how few bytes go
over the wire.**

Everything else is fixed or already spent. The bus tops out at 40 MHz (6.6h),
which puts full-frame wire time at 16.49 ms against a 16.67 ms budget - 99% of
the period gone before a single command byte. The CPU is at 160 MHz, the top of
its useful range for this workload. Rendering costs **0.002 ms/row**, 1.4% of a
frame; making it free would buy nothing.

What is *not* fixed is how much of the frame gets sent. The SH8601 keeps its own
framebuffer across CPU resets and software reset - the property that caused
three ghost-image misdiagnoses (6.6d) before it became the design's foundation.
An untouched pixel costs nothing. So:

> The runtime's job is not to draw quickly. It is to know exactly what did not
> change, and to send nothing else.

Measured, that is the difference between 31.2 ms per frame (repaint everything,
32 fps) and 7.3 ms (send only what changed, 60 Hz locked with 2.2x headroom).
**4.5x, and no other layer in the stack closes that gap.**

The corollary sets the design budget: cost is linear at 0.069 ms/row, so **up to
~240 rows - 54% of the screen - can change every frame at a locked 60 Hz.**
Interfaces sit far inside that. Only workloads touching most of the screen every
frame exceed it, and those are what banded DMA (6.6j, parked in 6.6l) exists for.

### 3.0a Layers

```
+-------------------------------------------------------------+
|  scene / application   (pure C99, portable, host-testable)  |
|    a rowfn: given y, fill one row                           |
+-------------------------------------------------------------+
|  gfx.c      retained model of all 448 rows                  |
|             DERIVES dirtiness by diffing; never declared    |
+-------------------------------------------------------------+
|  elide.c    dirty rows -> coalesced spans; rolling resync   |
+-------------------------------------------------------------+
|  sh8601.c   address window, span writes, QSPI framing       |
+-------------------------------------------------------------+
|  spi2.c  gdma.c   transport + transmit ledger               |
+-------------------------------------------------------------+
|  metal99 platform   start.c wdt.c clk.c io.c vec.c fold.c   |
+-------------------------------------------------------------+
|  ESP32-S3 silicon                                            |
+-------------------------------------------------------------+
```

Layers above `sh8601.c` contain **no register access**. Layers below contain
**no rendering logic**. That boundary is what lets a rowfn compile and run
unchanged on a Linux host (`tests/host/`), and it is why the digest lives in its
own `fold.c` - the host builds the firmware's instrument rather than a copy.

`vec.c` is cross-cutting by necessity: the no-scalar rule (6.9) applies at every
layer that touches bulk data.

> **CORRECTION (2026-08-24).** This section previously diagrammed a NeoGPU port
> - `hs_backend_sh8601.c`, an `HSBackendOps` vtable, an opcode stream, and a
> `render_c99.c` application layer. None of those exist. That was the Milestone-2
> *plan* (§7), and what actually shipped is `gfx.c`: 101 lines keeping a 448-row
> descriptor model, because there is no framebuffer to replay a command list
> into — and there is none because the panel is wire-bound, not because one
> would not fit (§5.1). The diagram was never updated
> when the plan changed, so this document described an architecture the code had
> not had for some time.

### 3.1 Boot model

The firmware image is written to **flash offset 0x0** — the bootloader slot.
The mask ROM loads our segments straight into SRAM and jumps to `_start`. There
is no second-stage bootloader and no partition table.

Consequences:
- The entire firmware must fit in internal SRAM.
- The ROM jumps in **with a valid stack**, so `_start` can be plain C. No
  assembly prologue is needed.
- `.bss` is **not** zeroed for us. `_start` does it.
- Hardware watchdogs arrive **armed**. See §6.1.

Memory layout (`link.ld`):

| Region | Origin | Length |
|---|---|---|
| `iram` (code) | `0x40378000` | `0x20000` (128 K) |
| `dram` (data) | `0x3FCA8000` | `0x30000` (192 K) |

SRAM1 is dual-mapped: IRAM `0x40378000` == DRAM `0x3FC88000`, offset `0x6F0000`.
Both regions stay below `0x3FCE9700`, which the ROM uses for its own stack.

---

## 4. Performance budget

> **PROVENANCE WARNING.** The table below was measured on the **ESP-IDF
> transport** (DMA + PSRAM framebuffer + bounce buffer). metal99 uses none of
> those. These are not metal99 targets and must not be quoted as our ceiling -
> doing exactly that produced the "~35 fps" error corrected in §4.4.
> **A number without its transport is not data.**

Measured on this hardware with the ESP-IDF-based `bare_metal_fb` build
(`-O2`, 368x448 RGB565, band flush, no FreeRTOS). These numbers set the targets
metal99 must meet or beat.

| Renderer | Render | Flush | Total | FPS |
|---|---|---|---|---|
| Solid fill | 6.18 ms | 22.49 ms | 28.67 ms | 34.8 |
| Integer LUT plasma | 11.57 ms | 22.49 ms | 34.07 ms | 29.3 |
| Per-pixel `sqrtf` x2 | 135.57 ms | 22.49 ms | 158.06 ms | 6.3 |

Band-height sweep (solid fill):

| Band rows | Flush | FPS |
|---|---|---|
| 8 | 27.38 ms | 29.7 |
| 16 | 24.13 ms | 32.9 |
| 32 | 22.49 ms | 34.8 |
| **64** | **21.81 ms** | **35.7** |

### 4.1 What the numbers mean

- **Flush dominates.** ~21.8 ms of DMA per full frame = 15.1 MB/s effective.
  Hard ceiling ~**45 fps** at band 64, before any rendering.
- **Render budget is ~7 ms** to hold 30 fps.
- **Float in the inner loop is fatal.** Two `sqrtf` per pixel costs 11x. Use
  LUTs and fixed point.
- **Larger bands are better** and the curve had not flattened at 64. metal99
  should sweep past 64 — the ESP-IDF build was capped by its staging buffer, not
  by the hardware.

### 4.3 The CPU runs at 20 MHz until we change it

**Measured 2026-08-24: 20.0 MHz** (240,000,000 cycles took 12.027 s, twice).
That is the ROM default — XTAL/2, with the PLL off.

Every number in the table above was taken at **240 MHz** under ESP-IDF. metal99
currently runs the CPU **12x slower**. Consequences:

- Render times scale directly: the 11.57 ms plasma becomes ~139 ms at 20 MHz.
- Flush does **not** scale — it is bound by the 40 MHz SPI bus, not the CPU.
- Any `ccount`-based delay depends on this. `CPU_HZ_ASSUMED` in `spi2.c` is set
  to the measured 20 MHz and **must be updated** if the PLL is ever enabled, or
  every delay silently shortens by the same factor.

Enabling the PLL therefore moves from "nice later" to **a prerequisite for
Milestone 4**. It is not needed for Milestone 2, which is about correctness.

### 4.4 CORRECTION: the real ceiling is 60.6 fps, not 35

Earlier text claimed a "~35 fps ceiling after GDMA and PLL". **That was wrong.**
It was imported from the ESP-IDF measurements above, which used a different
transport. The physical bound for metal99 is arithmetic:

    frame = 368 x 448 x 16 bits = 2,637,824 bits over 4 QSPI lines

    40 MHz SDR : 160 Mb/s -> 16.49 ms -> **60.6 fps**
    80 MHz SDR : 320 Mb/s ->  8.24 ms -> 121 fps
    80 MHz DDR : 640 Mb/s ->  4.12 ms -> 243 fps

Today's 3.2 fps is **94.7% per-transaction overhead**, not physics - the FIFO
path was chosen to isolate protocol bugs from DMA bugs and it did that job.

~~Note `SOC_SPI_SUPPORT_DDRCLK = 1`~~ - **REFUTED, see
`docs/lmm/framerate_phase0_results.md`.** That flag appears only in Kconfig
capability files; there is no DDR implementation in the SPI master HAL, and the
DDR-adjacent registers belong to a DQS strobe scheme needing a strobe line this
panel lacks. **No DDR on this path.** Likewise panel-side vertical scroll
(`0x33`/`0x37`) is **not implemented** by this panel - tested, bars did not
move. And 80 MHz requires the PLL, so the clock ceiling test is gated on
Phase 2 rather than preceding it.

**Realistic ceiling: 60.6 fps full-frame at 40 MHz SDR.**

**Full-frame fps is also the wrong metric.** See `docs/lmm/framerate_synth.md`.
A 32x32 region update costs ~102 us at 40 MHz, i.e. ~10,000 updates/sec, using
the `0x2A`/`0x2B` address window we already have and have been using only to say
"the whole screen".

### 4.2 Known-bad path

The ESP-IDF SPI master **cannot DMA out of PSRAM**; it silently bounces through
an internal-RAM buffer and a full 329 KB frame fails to allocate. metal99 sidesteps
this entirely by never putting pixels in PSRAM.

---

## 5. Memory budget

Total internal SRAM: **512 KB**. Our linker regions claim 320 KB of it.

| Consumer | Size | Notes |
|---|---|---|
| Code (`.text`) | ~8–16 K est. | Milestone 1 was 879 B |
| Band staging buffer | 46 K | 2 buffers x 32 rows x 368 x 2 B (double-buffered) |
| Messaging layer (embedded profile, §7.4) | 34 K | vs 1920 K at desktop defaults |
| Stack | 8 K | |
| **Subtotal** | **~104 K** | Comfortable inside 320 K |

Headroom allows a larger band (128 rows = 92 K) or a full 368x448 framebuffer
in SRAM (322 K) if a persistent surface is ever needed — though that would
crowd out everything else and is not planned.

### 5.1 A framebuffer would fit. That is not why we do not have one.

Measured 2026-08-24, because "322 KB will not fit in 192 KB of DRAM" had been
repeated in README, `gfx.h` and §3 as the justification for the retained-model
design, and it is **false**.

The 192 KB is a line in `link.ld`, not silicon. The ESP32-S3 has 480 KB of
DRAM-addressable SRAM (SRAM1 0x3FC8_8000-0x3FCF_0000 plus SRAM2 to
0x3FD0_0000). The script claims 192 KB and uses 52. IRAM claims 128 KB for
8 KB of code.

| | |
|---|---|
| DRAM-addressable total | 480 KB |
| Declared in `link.ld` | 192 KB (52 used) |
| Unclaimed above it, below the ROM stack floor `0x3FCE9700` | 69 KB |
| Recoverable by right-sizing IRAM to 16 KB | ~112 KB |
| **Usable** | **373 KB** |
| Framebuffer 368x448x2, plus non-band `.bss` | 328 KB |

**It fits, with 45 KB spare.** (Arithmetic against the ROM stack floor taken
from `link.ld`'s own comment; not a tested link.)

The real reason is §3.0: the panel is **wire-bound**. A framebuffer does not
reduce bytes on the bus, so it would cost 322 KB and buy nothing. What reduces
bytes is knowing what did not change - a 1,792-byte model, 184x smaller than the
thing it replaces.

#### If a framebuffer is ever wanted anyway

It would be for something the row-descriptor model cannot express: per-pixel
diffing, alpha blending, read-modify-write compositing. Costs, measured:

| representation | size | expand cost |
|---|---|---|
| raw RGB565 | 322 KB | none |
| 8bpp indexed | 161 KB | 8 instr/px scalar, ~22 us/row (+32% on a 69 us row) |
| 4bpp indexed | 80 KB | 10.5 instr/px vectorised - worse than scalar |
| **2bpp indexed** | **41 KB** | **3 instr/px vectorised, ~6.9 us/row (+10%)** |

Palette expansion IS vectorisable, contrary to an earlier claim here: PIE has no
gather, but `ee.vcmp.eq.s16` + `ee.andq` + `ee.orq` is a complete select, and
walking the index down with `ee.vsubs.s16` needs only one compare constant.
Cost is `(5N + 4)` instructions per 8 pixels, linear in palette size N, so it
beats the 8 instr/px scalar path **below ~12 entries** and loses above.

Two ISA facts cap it, both probed with `tools/isa_probe.sh` on gcc 14.2.0:
**no permute/gather** (`ee.vperm`, `ee.vtbl`, `ee.vshuf`, `ee.vgather`,
`ee.vsel`, `ee.vlut` all fail to assemble - a PSHUFB-equivalent would make a
16-entry lookup one instruction) and **no 16-bit vector shifts** (only
`ee.vsl.32`/`ee.vsr.32`), which rules out a log2(N) bitplane blend tree at pixel
width. What does assemble - `ee.vrelu.s16`, `ee.vprelu.s16`, `ee.vmulas.*` into
40-bit accumulators - shows why: this is the AI extension, built for inference
MACs and activations. Table lookup was never a design target.

### 5.2 The run model, and diffing against what was sent

Measured 2026-08-24.

**A row used to be a band.** `gfx_row` was `{kind, a, b, x}`: one colour, or two
with a single transition. That expressed a full-width band and one vertical
edge, and it could not express a rectangle anywhere but against a screen edge -
a row crossing a centred box is `bg|fg|bg`, two transitions. You could not draw
a box in the middle of the screen.

A row is now a run list (8 runs, 48 bytes, padded to whole vectors so `vec_copy`
moves one in three instructions). Rectangles compose, overlap and elide.

**Spans became rectangles.** `elide` carries an x-extent per dirty row and
coalesces only rows whose extents are identical - merging different extents
would force their union on every row, and two rows dirty at opposite edges would
union to full width, worse than two spans. `sh8601_write_span_x` sets the
address window to the extent. The `0x2A`/`0x2B` window had always taken x0/x1;
the driver passed `0, WIDTH-1` on every call from first contact until now.

**Then the diff moved to present time.** The first cut diffed at SET time
against the model in flight, which marked every intermediate state. Erasing a
box and drawing it 4 px lower takes the overlapping rows `FG -> BG -> FG`: net
unchanged, both transitions marked, transmitted for nothing. So `gfx` keeps two
models - `g_model` (described) and `g_sent` (what the panel actually received) -
and diffs them in `gfx_present()`, walking only rows touched since the last one.

| one step of continuous motion | px/frame | |
|---|---|---|
| full-width bar, band model | 37,750 | the demo element, full width by nature |
| 88x88 box, sub-width spans | 9,236 | 4x |
| 88x88 box, + present-time diff | **1,968** | **19x end to end** |

`g_sent` advances **only on a successful flush**. Advancing it regardless would
tell the next diff those rows are already on the panel and the update would be
lost for good - the precise "model drifts from reality" failure this layer
exists to prevent.

#### Verified over a long run, with the net down

The 20 s resync-off window (6.6k) is the strongest verification here, and it
used to drive `elide_mark()` and `scene()` directly - testing elide's marking
and nothing above it. `g_sent`, the model of what the panel is believed to hold,
was the one layer it did not touch, and a drift there produces exactly the
symptom this window exists to catch. The loop now describes the scene through
`gfx` instead, so the whole stack is under test and there is no hand-marking
left in the demo to get wrong.

| pacing loop, measured | rows | px | ms | headroom |
|---|---|---|---|---|
| stationary bar, resync on | 4 | 1,472 | 0.3 | 48.9x |
| moving bar, resync OFF | 8 | 2,944 | 0.6 | 26.2x |
| *(previously, raw elide)* | *100* | *36,800* | *7.0* | *2.3x* |

**12.5x fewer pixels, and headroom from 2.3x to 26.2x.** A moving element now
costs its leading and trailing edges rather than its area: a 96-row bar stepping
4 px changes 8 rows. A stationary element costs nothing at all - the 4 rows in
the first line are the resync alone.

Every wrap still marks exactly 192 rows in 2 spans, through the new path: at the
wrap old and new do not overlap, so both bands are genuine net changes.

### 5.3 Text is described, not rasterised

Measured 2026-08-24.

**Why text is not runs.** A glyph scanline like `0b01100110` is five runs. A
twenty-character line would be a hundred runs in a row that holds eight, so
rasterising text into the run model is not available. The other obvious move -
a "this row is custom, call back" escape hatch - gives up the diffing the whole
layer exists for.

So a label is a DESCRIPTION: position, colour, font, and the string. It diffs
exactly like a run list, and it is double-buffered against what the panel holds
for the same reason rows are (5.2).

**The string is copied, not pointed at.** A caller formatting into a reused
buffer would otherwise change the content without changing the pointer, and the
diff would miss it entirely. Owning the bytes makes the comparison exact rather
than a hash that can collide.

**The blit is transparent.** A clear glyph bit keeps whatever the runs drew,
which costs nothing extra: the mask that selects the foreground is the same mask
that keeps the destination. Text therefore composes over a changing background
without either layer knowing about the other - the run diff marks the row and
`gfx_rowfn` re-renders it runs-then-text.

| measured on hardware | px/frame |
|---|---|
| first paint, three labels | 11,200 |
| **setting identical text again** | **1,472 - resync only; the text is free** |
| updating a 5-digit 16x32 counter | 3,989 (2,560 label + ~1,374 resync) |
| a full screen, for scale | 164,864 |

**A static label marks nothing.** It cannot differ from what the panel holds, so
the 20 s resync-off window and its wrap trace are completely unaffected by a
title sitting on screen throughout - still 8 rows and 2 spans per frame, still
exactly 192 rows at every wrap. The label still RENDERS over the bar each time
the bar passes beneath it.

#### Fonts come from TrueType, at build time

`tools/mkfont.py` rasterises a TTF once on the host into 1bpp bitmaps. The
device cannot do this: a TrueType scan converter wants malloc, libc and floating
point, and outline filling is irreducibly scalar per-element work - 3.0 says
compute has to stay negligible or the wire-bound thesis stops holding. Blitting
bits is 1.375 instructions per pixel (6.9a); rasterising outlines is not in the
same class.

Build time also means the typeface is a CHOICE, with a licence chosen rather
than inherited from whatever console font is installed. Share Tech Mono, SIL OFL
1.1, TTF sources tracked in `tools/fonts/` so the generated table is
reproducible.

Glyph width is a multiple of 8 by construction, so a glyph placed on the 8px
grid occupies whole 128-bit vectors and the blit needs no masking and no
unaligned path. Anti-aliased text would need per-channel arithmetic on packed
RGB565, which 6.9b records as the one place this ISA runs out.

#### The safety net is now the dominant cost

The rolling resync refreshes 4 full-width rows every frame: 1,472 px, against
1,968 px total for a moving 88x88 element. **75% of a small update is now the
resync, not the update.**

That is what elision being this cheap looks like, and it is not obviously wrong
- the resync is what makes a model of unreadable remote state self-correcting,
and 6.6k records what happened the one time drift went unnoticed.
`ELIDE_RESYNC_FRAMES` trades its cost against how fast drift is repaired (120
frames = 4 rows/frame = whole screen in 1.9 s). Left alone deliberately: the
marking is now proven correct with the net off (13 wraps, 6.6k), but "proven for
this workload" is not "proven for every future one".

---

## 6. Milestone 2 — SH8601 bring-up from registers

**Goal:** color bars on the panel, driven entirely by our own register writes.

**Success criterion:** five horizontal bands — red, green, blue, white, black —
top to bottom. Chosen deliberately over a solid fill because one image
simultaneously verifies (a) byte order, (b) geometry / address-window commands,
and (c) that the panel is out of sleep.

### 6.1 Watchdogs (done in Milestone 1)

The ROM hands over with watchdogs armed; without this the board reset-loops with
`rst:0x7 (TG0WDT_SYS_RST)`.

| Watchdog | Config reg | Protect reg | Key |
|---|---|---|---|
| TIMG0 MWDT | `0x6001F048` | `0x6001F064` | `0x50D83AA1` |
| TIMG1 MWDT | `0x60020048` | `0x60020064` | `0x50D83AA1` |
| RTC WDT | `0x60008098` | `0x600080B0` | `0x50D83AA1` |
| RTC super-WDT | `0x600080B4` | `0x600080B8` | `0x8F1D312A` |

The super-watchdog **cannot be disabled** — set `SWD_AUTO_FEED_EN` (bit 31).

### 6.2 Peripheral clock and reset

SPI2 boots clock-gated. Order matters: enable clock, pulse reset, then configure.

| Register | Address | Bit |
|---|---|---|
| `SYSTEM_PERIP_CLK_EN0` | `0x600C0018` | `SPI2_CLK_EN` = bit 6 |
| `SYSTEM_PERIP_RST_EN0` | `0x600C0020` | `SPI2_RST` = bit 6 |

Sequence: set `CLK_EN`; set then clear `RST`.

### 6.3 Pin routing via GPIO matrix

Because the board's pinout conflicts with IO_MUX defaults (§2.2), every signal
routes through the GPIO matrix.

Per pin:
1. Set the pin's IO_MUX register (`0x60009000` + pin offset) to function
   `GPIO` (so the matrix owns it), drive strength, and no pull.
2. Enable output: `GPIO_ENABLE_W1TS` (`0x60004024`).
3. Point the matrix at the peripheral signal:
   `GPIO_FUNC{n}_OUT_SEL_CFG` = `0x60004554 + 4*n`, write the signal index.

Signal indices (from `gpio_sig_map.h`):

| Signal | Index | GPIO |
|---|---|---|
| `FSPICLK_OUT` | 101 | 11 |
| `FSPIQ_OUT` (D1) | 102 | 5 |
| `FSPID_OUT` (D0) | 103 | 4 |
| `FSPIHD_OUT` (D3) | 104 | 7 |
| `FSPIWP_OUT` (D2) | 105 | 6 |
| `FSPICS0_OUT` | 110 | 12 |

> **Unverified:** the D0–D3 to FSPID/Q/WP/HD mapping above follows the standard
> quad-SPI ordering (D0=D/MOSI, D1=Q/MISO, D2=WP, D3=HD). Confirm against TRM
> before first bring-up; a swap here produces scrambled pixels, not a blank
> screen, which makes it easy to diagnose from the color bars.

### 6.4 SPI2 configuration

Base `0x60024000`.

| Register | Offset | Purpose |
|---|---|---|
| `SPI_CMD` | `0x00` | `USR` bit starts a transaction; poll it for done |
| `SPI_CTRL` | `0x08` | quad/dual mode for data phase |
| `SPI_CLOCK` | `0x0C` | clock divider |
| `SPI_USER` | `0x10` | which phases are present (cmd/addr/dummy/MOSI) |
| `SPI_USER1` | `0x14` | addr and dummy bit-lengths |
| `SPI_USER2` | `0x18` | command value and command bit-length |
| `SPI_MS_DLEN` | `0x1C` | transaction data length in bits |
| `SPI_MISC` | `0x20` | CS setup/hold |
| `SPI_DMA_CONF` | `0x30` | DMA enable (Milestone 3) |
| `SPI_W0..W15` | `0x98..` | 64-byte CPU FIFO |

Required settings: SPI mode 0, MSB first, 40 MHz, CS asserted for the whole
transaction, command phase 32 bits on **one** line, data phase on **four** lines
for pixels and **one** line for parameters.

**RESOLVED 2026-08-24 — implemented and verified on hardware (`src/spi2.c`).**

| Field | Register | Bit |
|---|---|---|
| `USR` (start) | `SPI_CMD` | 24 |
| `UPDATE` (latch config) | `SPI_CMD` | 23 |
| `USR_MOSI` | `SPI_USER` | 27 |
| **`FWRITE_QUAD`** | **`SPI_USER`** | **13** |
| `D_POL` / `Q_POL` | `SPI_CTRL` | 19 / 18 |
| `CS0/1/2_DIS` | `SPI_MISC` | 0 / 1 / 2 |
| `CS_KEEP_ACTIVE` | `SPI_MISC` | 30 |
| `MS_DATA_BITLEN` | `SPI_MS_DLEN` | 0..17 |
| `CLK_EQU_SYSCLK` | `SPI_CLOCK` | 31 |
| `CLK_EN`/`MST_CLK_ACTIVE`/`MST_CLK_SEL` | `SPI_CLK_GATE` (`0xE8`) | 0 / 1 / 2 |

> **Trap: `FWRITE_QUAD` lives in `SPI_USER`, not `SPI_CTRL`.** Only *`FREAD`*`_QUAD`
> is in CTRL. Putting it in CTRL fails **silently** — the transfer still
> completes, just on one line. Caught only because quad and single transfers
> timed identically.

> **Trap: clock source must be XTAL.** `MST_CLK_SEL` 0 = XTAL, 1 = APB. The ROM
> leaves the **PLL off**, so APB is starved and selecting it yielded a 2.1 MHz
> bus. XTAL is a steady 40 MHz — exactly the panel's rate — so
> `MST_CLK_SEL = 0` plus `CLK_EQU_SYSCLK` (bypass divider) gives 40 MHz with no
> PLL work at all.

Measured after the fix, 300-sample averages, 64 B on 4 lines (128 spi clocks)
vs 1 line (512), so per-call overhead cancels exactly:

```
64B quad=1117 single=1309 delta=192 -> 40000 kHz   PASS
```

Predicted delta 192 cycles; measured 192. A deliberate /2 divider measured
19,948 kHz. Bus rate confirmed at **40.0 MHz with quad mode active**.

### 6.5 SH8601 QSPI framing

Command word is 32 bits, sent on a single line:

```
[ opcode , 0x00 , cmd , 0x00 ]        (big-endian byte order)
   opcode = 0x02   parameter write, data phase on 1 line
   opcode = 0x32   pixel  write, data phase on 4 lines (cmd = 0x2C)
```

Equivalently: `word = (opcode << 24) | (cmd << 8)`.

### 6.6 Init sequence

Transcribed from BSP 2.0.0. Format: `{ cmd, params, param_count, delay_ms }`.

| # | Cmd | Params | Delay | Meaning |
|---|---|---|---|---|
| 1 | `0x11` | — | **120 ms** | Sleep out |
| 2 | `0x44` | `01 D1` | 0 | Tear scanline |
| 3 | `0x35` | `00` | 0 | Tearing effect on |
| 4 | `0x53` | `20` | 10 ms | Write CTRL display |
| 5 | `0x2A` | `00 00 01 6F` | 0 | Column addr 0..367 |
| 6 | `0x2B` | `00 00 01 BF` | 0 | Row addr 0..447 |
| 7 | `0x51` | `00` | 10 ms | Brightness = 0 |
| 8 | `0x29` | — | 10 ms | Display on |
| 9 | `0x51` | `FF` | 0 | Brightness = full |

Note `0x2A` ends at `0x16F` = 367 and `0x2B` at `0x1BF` = 447 — confirming
368x448 with zero offset. There is **no reset pin**, so software reset via
`0x11` is the only reset path. The 120 ms delay after it is mandatory.

Brightness is set to 0 before display-on and raised afterwards, which suppresses
a flash of garbage framebuffer at power-up. Preserve that ordering.

### 6.6a Red-team review of `spi2.c` (2026-08-24)

Adversarial review after the driver passed. Nine findings; all remediated and
re-verified on hardware, with a ten-assertion regression test in `main.c`.

| # | Finding | Severity | Fix |
|---|---|---|---|
| F1 | `spi2_xfer` returned **silently** on `len == 0` or `len > 64` | **High** | Returns `SPI2_E_LEN`/`E_NULL`; added `spi2_write()` that chunks internally |
| F2 | ROM-inherited `SPI_SLAVE` / `SPI_DMA_CONF` never cleared | Medium | Cleared in init, as IDF's master init does |
| F3 | `spi2_init` never pulsed `SPI_UPDATE`, so its config was unlatched | Medium | `spi2_sync()` at end of init |
| F4 | `spi2_set_clock_reg()` debug hook shipped in the public API | Medium | Removed |
| F5 | `1u << gpio` is **undefined for gpio >= 32**; this chip has pins to 48 | Medium (latent) | Two-bank handling via `GPIO_ENABLE1_W1TS` |
| F6 | Always wrote all 16 `W` registers regardless of length | Low (perf) | Write `ceil(len/4)`; **4-byte transfer 889 -> 206 cycles, 4.3x** |
| F7 | `keep_cs` could strand CS low with no diagnostic | Low | Contract documented; `spi2_write()` cannot get it wrong |
| F8 | SPI mode 0 was correct only *by omission* of two bits | Cosmetic | Explicit comment so it is not "tidied" away |
| F9 | `delay_ms` lived in the SPI header and hid a CPU-clock dependency | Low | Moved to `io.h` beside `CPU_HZ` |

**Not a bug:** `D_POL`/`Q_POL` are set to 1, which matches the hardware
reset default (`1'b1`) - confirmed against the register description.

**The review also found a bug in its own test.** The first run reported
`FAIL SPI_DMA_CONF cleared`, reading `0x3` after writing 0. Bits 0 and 1 are
**read-only status** (`DMA_OUTFIFO_EMPTY`, `DMA_INFIFO_FULL`), reset-default 1,
meaning "idle". The driver was correct; the assertion was wrong. It now masks
the RO bits. Worth recording because a register that does not read back what
you wrote is not automatically a defect.

Regression output:

```
PASS  SPI_SLAVE cleared              PASS  len=64  -> OK
PASS  SPI_DMA_CONF writable cleared  PASS  write 200B -> OK
PASS  len=0   -> E_LEN               PASS  write 1B   -> OK
PASS  len=65  -> E_LEN               PASS  write 0B   -> E
PASS  NULL    -> E_NULL
64Bq=1124 64Bs=1316 4B=206 -> 40000 kHz  PASS 40MHz
```

### 6.6b First contact - VERIFIED 2026-08-24

**Result: the panel responds to commands composed from raw registers.**

Method mattered more than the code. The panel keeps power and state across CPU
resets and has no reset pin, so after flashing metal99 it was still displaying
the previous firmware's last frame - already initialised, awake, display on.
That is a diagnostic asset, not a nuisance:

1. **Do not run the init sequence.** A known-good display is the best reference
   available; `0x11` sleep-out would have destroyed it before anything was
   proven.
2. **Send no pixels.** If colour bars had failed, a framing bug, a CS-hold bug
   and a data-path bug would all look identical (black screen, no error).
3. **Use a reversible knob.** `0x51` (brightness) needs zero pixel data, is
   guaranteed supported, and alternating `0x00`/`0xFF` on a 1.5 s cycle turns a
   static image into an unmistakable signal.

Observed: the frozen image went fully dark, then came back, repeatedly.

Proven by that one test, without sending a single pixel:

| Proven | |
|---|---|
| Command framing `{0x02, 0x00, cmd, 0x00}` on one line | correct |
| CS held across command -> parameter | works |
| All six pins routed through the GPIO matrix | correct |
| 40 MHz, SPI mode 0 | accepted by the panel |
| Parameter write path, both directions | works |

Still unproven, and deliberately so: the **pixel path** (opcode `0x32`, quad
mode, `0x2C`), **CS held across ~5,152 FIFO chunks**, and **our own init
sequence** - we are currently riding on the previous firmware's initialisation.

**Revised milestone order.** Test one new thing at a time, keeping the
pre-initialised panel as the reference for as long as possible:

| Step | Tests | Status |
|---|---|---|
| 2b-i | command path via brightness | **DONE** |
| 2b-ii | pixel path, reusing the existing init and address window | next |
| 2b-iii | our own init sequence (destructive - do it last) | after |

This inverts the original plan, which ran init first. Running our own init last
means that if the panel ever goes dark we know exactly which change did it.

### 6.6c Pixel path - VERIFIED 2026-08-24

**Colour bars and a gradient render correctly on the panel.** Milestone 2 is
complete: the display is driven end to end from our own registers, in pure ISO
C99, with no ESP-IDF, no FreeRTOS and no ROM calls.

Proven by this step:

| Proven | |
|---|---|
| Pixel framing: opcode `0x32`, command `0x2C` | correct |
| Four-line (QIO) data phase | correct |
| **CS held across 5,152 FIFO chunks for 311 ms** | works |
| Address window `0x2A` / `0x2B` | correct |
| RGB565 **big-endian** byte order | correct |
| Redraw (two different frames alternating) | works |

The CS-hold concern is resolved empirically: holding CS asserted across
thousands of small transactions is fine, so `0x3C` write-memory-continue is
not needed.

**No framebuffer is required.** Frames stream row by row through a single
736-byte row buffer, so a 329,728-byte frame never exists in RAM. This was
forced by the 192 K DRAM region and happens to be the architecture we wanted
anyway.

#### ISA probe results

Probed by assembling each mnemonic. Available and useful:

| Category | Instructions |
|---|---|
| 16-bit lane arithmetic | `ee.vadds.s16` `ee.vsubs.s16` `ee.vmul.s16` `ee.vmul.u16` |
| Logical | `ee.andq` `ee.orq` `ee.xorq` |
| Broadcast / lane insert | `ee.vldbc.16` `ee.vldbc.32` `ee.movi.32.q` |
| Byte reorder | `ee.vzip.8` `ee.vunzip.8` `ee.vzip.16` |
| Min/max, 32-bit shift | `ee.vmin.s16` `ee.vmax.s16` `ee.vsl.32` `ee.vsr.32` |

**Not available:** `ee.vsl.16` / `ee.vsr.16` (no 16-bit lane shift), `ee.shfqi`,
`ee.slci.2q`, `ee.srci.2q`.

The missing 16-bit shift matters less than it looks. In RGB565 **wire format**
(byte-swapped) the memory word is `(lo << 8) | hi` where `hi = RRRRRGGG` and
`lo = GGGBBBBB`. So **red sits at bits 3-7 and blue at bits 8-12 of the memory
word** - both contiguous. Ramping red or blue is plain vector addition with no
byte swap required. Only green straddles the byte boundary.

### Measured, and what it costs

| Frame | Time |
|---|---|
| Colour bars (trivial fill) | **311 ms** |
| Gradient (per-pixel arithmetic) | **599 ms** |

Two independent facts fall out:

1. **The FIFO path is overhead-bound, not bus-bound.** 329,728 B in 311 ms is
   1.06 MB/s. Each 64-byte chunk costs ~1124 cycles (56.2 us at 20 MHz) but
   only 128 SPI clocks (3.2 us) of that is time on the wire - about **6 % bus
   utilisation**. The other 94 % is per-transaction setup. This is precisely
   what GDMA exists to remove, and it bounds the win at roughly 17x.
2. **The CPU is starved.** Identical transfers, yet the gradient costs 288 ms
   more purely in per-pixel arithmetic. At 240 MHz that would be ~24 ms. This
   is the §4.3 warning showing up in a real measurement.

Together they say Milestone 3 (GDMA) and PLL enablement are the two things
standing between here and a usable frame rate, and they are independent.

### 6.6d Our own init sequence - 2026-08-24

`sh8601_init()` brings the panel up and drawing works repeatedly. Two real
findings, both of which cost time and are worth keeping.

#### The BSP's nine commands are only the VENDOR portion

The array in the BSP is `vendor_config.init_cmds`. The `esp_lcd_sh8601` driver
wraps it with three more steps that are **not** in that array:

| Step | Command | Value |
|---|---|---|
| `panel_reset()` | `0x01` SWRESET + **80 ms** | (no reset pin on this board) |
| `panel_init()` | `0x36` MADCTL | `0x00` (RGB element order) |
| `panel_init()` | **`0x3A` COLMOD** | **`0x55` (16bpp RGB565)** |

`COLMOD` is the pixel-format register. Without it the panel does not know the
stream is RGB565, accepts every byte, and renders nothing. Transcribing only
the vendor array produced a permanently black screen.

This went unnoticed for three milestones because the previous firmware had
already set those registers and they survive a CPU reset.

#### Sleep-in is a one-way door with this init

`0x10` (sleep in) puts the panel somewhere our abbreviated sequence cannot
recover from - `0x11` sleep-out plus the vendor list is not enough to bring it
back. Almost certainly sleep-in drops the panel's internal supply, and waking
needs vendor power-on steps the BSP list does not contain, because the BSP only
ever initialises from power-on, never from sleep.

**Sleep is not a project goal.** `sh8601_sleep()` existed only to manufacture a
cold start to recover from. It is retained but marked unsupported; nothing in
the graphics path calls it.

#### The ghost-framebuffer trap

The panel keeps its framebuffer across CPU resets, **and** `0x01` software reset
explicitly does not clear it. So a stale image is indistinguishable from a
freshly drawn one, and "the screen shows bars" was twice mistaken for success
when nothing was actually reaching the panel.

> **Rule for this project: a display showing something is not evidence that
> your code put it there. Only change is evidence.** Every test after first
> contact is built on motion - alternating two very different frames - so a
> static screen is unambiguously a failure.

The only true cold start is a **physical power cycle**; a CPU reset is not one.

### 6.6e MILESTONE 2 COMPLETE - cold start verified 2026-08-24

Physical USB unplug/replug, which drops power to the panel and clears its RAM.
On replug the bars/magenta animation returned. A ghost is impossible here, so
everything displayed was produced by our own code starting from nothing.

**metal99 now drives the display end to end with zero dependencies:**

| Layer | Status |
|---|---|
| Boot from flash `0x0` via mask ROM into SRAM | verified |
| Watchdog disable, `.bss` zeroing, USB-JTAG console | verified |
| SPI2 QSPI at 40 MHz, quad mode | verified by timing |
| SH8601 init from a cold panel | verified by power cycle |
| Address window, RGB565 big-endian pixels | verified visually |
| Row-streamed frames, no framebuffer in RAM | verified |

2,240-byte image **at this milestone**. Pure ISO C99 under `-pedantic-errors
-Wall -Wextra -Werror`. No ESP-IDF, no FreeRTOS, no libc, no ROM calls.

> Historical figure, kept as the record of what Milestone 2 shipped. The
> current image is 8,528 bytes — elision, `gfx`, GDMA and the self-test all landed
> after this. `build.sh` prints the size on every build; README carried the
> 2,240 figure forward unchanged for months, which is why this one is dated.

**What remains is performance, and both levers are measured and independent:**

| Lever | Current | Expected |
|---|---|---|
| GDMA instead of 64-byte FIFO | 6 % bus utilisation, 311 ms/frame | ~17x - the FIFO path is overhead-bound, not bus-bound |
| PLL instead of ROM default | CPU 20 MHz | 12x on per-pixel work; flush is unaffected, being bus-bound |

### 6.6f Red-team of Phase 0 (2026-08-24)

Four findings from attacking the Phase 0 conclusions.

**R1 - 0a's reasoning rested on a corrupted measurement.** Corrected inline in
§6.4 above. The conclusion (0a is blocked on the PLL) survives; the reasoning
did not. Noted at the time that 4.2 MHz matched no plausible clock, and moved
on anyway - that instinct should have been followed.

**R2 - unbounded spin loops could hang the device. REAL BUG, FIXED.**
`spi2_sync()` and the `CMD_USR` wait in `spi2_xfer()` spun forever. A bad clock
config stops the SPI clock, `SPI_UPDATE` never clears, and the board hangs with
**no diagnostic at all** - which is exactly what happened during this red-team.
Both now bound at `SPI2_SPIN_LIMIT` and return `SPI2_E_HANG`.

A timeout had been added to the equivalent flush path in the ESP-IDF build and
never carried into metal99. **Fixes do not migrate across rewrites by
themselves.**

The trigger was an arithmetic error of mine: `0x2102` was written intending
`CLKCNT_H = 1`, but decodes to `H = 4, N = 2`, which is invalid because H must
be <= N. Correct encoding is `0x2042`. Layout is `N << 12 | H << 6 | L`.

**R3 - 0c was absence-of-evidence reasoning. RESOLVED.** "Nothing moved" was
read as "scroll is unimplemented", when a dead command path produces an
identical observation. Retested with a positive control: phase B **blinked**
(brightness reached the panel) while phase A left the bars **stationary**.
Control passed, so the negative is real. **0c REFUTED, properly.**

*A correct conclusion from invalid reasoning is not a result - it is a
coincidence you have not caught yet.*

**R5 - the probe corrupted the device under test. FIXED.** The clock sweep drove
1,200 transfers of filler with **CS asserted**, so the panel received all of it
as commands and blacked out. Bus probing now parks CS high
(`GPIO_FUNC_OUT_SEL = 256`, the `SIG_GPIO_OUT_IDX` "driven by GPIO_OUT" signal)
and reattaches it afterwards. Measure the bus, not the device on it.

The R1 caveat had already noted that the safety guards perturbed the
*instrument* by 4%. That was written too narrowly: the probe was perturbing the
*system*.

**R4 - an unevidenced claim.** "The SH8601 is a standard SDR QSPI device" was
stated as fact; we have no datasheet. It is an **inference**. The 0b refutation
does not depend on it - the IDF-side evidence (no DDR path for GP-SPI master
writes) stands alone - but the sentence overstated what we know.

**Instrument caveat.** Adding the spin guards moved the measured delta from 192
to 201 cycles, about 4%. The safety fix perturbs the measuring instrument. True
bus rate is still 40 MHz.

### 6.6g Streaming telemetry - measure the workload, never probe it

**No synthetic probe traffic.** `sh8601_write_frame()` is instrumented and
streams per-frame statistics over the console. Everything reported is derived
from work that actually happened.

This is not only cleaner, it makes R5 structurally impossible: there is no test
traffic to accidentally aim at the panel. The probe hooks
(`spi2_set_src_and_div`, `spi2_cs_detach/attach`) have been deleted.

```
 grad | render 6.72ms  flush 82.20ms  total 89.83ms | 3584 kB/s | 20% | 11.1 fps
 bars | render 6.27ms  flush 82.20ms  total 89.38ms | 3602 kB/s | 20% | 11.1 fps
```

| Component | Time | Share |
|---|---|---|
| Render (vectorised) | 6.3-6.7 ms | **7%** |
| Flush | **82.20 ms** | **92%** |
| Command setup | ~0.9 ms | 1% |

Three things this shows that a single frame-time number could not:

1. **Vectorisation effectively removed rendering from the budget.** Per-pixel
   generation is now 7% of the frame. Bars and gradient differ by 0.45 ms.
2. **Flush is 82.20 ms for BOTH frames, identical to the hundredth.** Transfer
   cost is completely independent of content - exactly what you expect when the
   bottleneck is per-transaction overhead rather than data.
3. **Wire utilisation is 20%.** Of 82.20 ms of flush, only 16.49 ms is time on
   the wire. **80% of flush is per-transaction setup**, across 5,152 FIFO
   transactions.

#### The safety guards cost ~8% of flush

Flush measured ~76 ms before the R2 spin guards and 82.20 ms after: **+8%**. The
guard adds a compare-and-increment inside the `CMD_USR` poll loop, which runs
5,152 times per frame with several poll iterations each.

That is a real price for not hanging, and it is worth paying - but it should be
recorded rather than absorbed silently. **GDMA deletes the polling loop
entirely**, so the cost disappears with the same change that removes the
overhead it is measuring.

*(Attribution is inference from the arithmetic and timing, not an isolated
experiment - flagged as such rather than asserted.)*

### 6.6h 80 MHz bus - REFUTED. 40 MHz is the panel's usable maximum.

Experiment 0a, finally run once the PLL made APB 80 MHz available.

**The ESP32 emits at 80 MHz perfectly.** Flush halved exactly as predicted:
17.15 -> 8.92 ms, 101.7 fps, 92% wire utilisation.

**The panel does not accept it.** Test alternated long 40 MHz phases with short
80 MHz phases, each preceded by a full re-init at that clock. Measured phase
timing was 3.0 s / 0.6 s, cycling every 3.6 s. Observed behaviour was ~15 s of
bars then ~30 s of black - **not tracking the phases at all**. 80 MHz corrupts
panel state, and the following 40 MHz re-inits need several cycles to recover
it. `init rc=0` throughout, which as ever proves only that our transactions
completed.

**And there is no intermediate rate to fall back to.** `MST_CLK_SEL` picks XTAL
(40 MHz) or APB (80 MHz), and the divider is integer:

| source | /1 | /2 | /3 | /4 |
|---|---|---|---|---|
| XTAL | **40.0** | 20.0 | 13.3 | 10.0 |
| APB | 80.0 (fails) | 40.0 | 26.7 | 20.0 |

60 MHz would need 80/1.33 and 53 MHz 80/1.5 - neither is representable. There
is nothing between 40 and 80.

#### CORRECTION (2026-08-24, after elision landed)

The section below says "full-frame 60 fps is not achievable" and that was
**stated too broadly**. It is true only of a *literal 100% repaint every single
frame*. Measured against real updates, cost per row is dead linear at
**0.041 ms**:

| rows | update | % of 60Hz budget | |
|---|---|---|---|
| 32 | 1.3 ms | 8% | fits |
| 128 | 5.2 ms | 31% | fits |
| 256 | 10.5 ms | 63% | fits |
| 384 | 15.7 ms | 94% | fits |
| **408** | **16.67 ms** | **100%** | **the boundary** |
| 448 | 18.3 ms | 110% | misses |

**408 of 448 rows - 91% of the screen - can be updated at a steady 60 Hz on the
40 MHz bus.** Only a literal full repaint misses, and only by 10%.

And that last 10% is now **closed** (see 6.6j): banding plus render/DMA overlap
took a full frame from 18.30 ms to **16.60 ms**, so **every one of the 448 rows
can be updated at 60 Hz**.

> **CORRECTION (2026-08-24, after banded DMA was parked).** The paragraph above
> is true *of banded DMA*, and banded DMA is disabled (6.6l). It was written
> before that decision and reads as an unqualified claim, which is how README's
> "All 448 rows are updatable at 60 Hz" came to describe a transport that does
> not ship.
>
> Measured directly on the **FIFO** transport that does ship, one timed
> 448-row repaint at 160 MHz CPU / 40 MHz QSPI:
>
> | | |
> |---|---|
> | total | **31.2 ms** — **31.9 fps** |
> | flush | 30.3 ms (0.068 ms/row) |
> | render | 0.8 ms (0.002 ms/row) |
> | 60 Hz boundary | **~240 rows**, not 408 and not 448 |
>
> Still linear, still 2.2x headroom on a 104-row interface update, and still
> zero late frames in steady state. But a literal full repaint at 60 Hz needs
> the parked transport. The honest statement is: **60 Hz is achieved for
> interface-sized updates; full-frame 60 Hz is not achieved by what ships.**

So the honest statement is: **60 Hz is achieved.** The original claim confused
"full-frame refresh at 60 fps" with "60 fps", and only the former was ever in
doubt.

#### Original (over-broad) claim: full-frame 60 fps is not achievable

| | |
|---|---|
| Wire time at 40 MHz | **16.49 ms** |
| 60 fps budget | 16.67 ms |
| Left for ALL overhead and render | **0.18 ms** |
| Currently used by overhead + render | 1.44 ms |

Even with perfect banding (overhead ~0.05 ms) and perfect render/DMA overlap
(render off the critical path), the best case is ~16.54 ms = **60.4 fps, a 0.8%
margin**. That is not a steady 60 - any jitter breaks it.

**60 fps of an INTERFACE remains entirely achievable**, because interfaces do
not repaint every pixel every frame. At 19.2 MB/s effective, a 16.67 ms budget
carries ~320 KB - 97% of a full frame, but **5x** a screen that updates 20% of
its area. Elision is no longer the elegant option; it is the only route to a
steady 60 Hz.

### 6.6i Red-team of the GDMA/PLL work (2026-08-24)

Seven findings, all remediated. One of them was hiding a genuine hardware bug.

| # | Finding | Sev | Fix |
|---|---|---|---|
| B1 | **`gdma_wait()` was dead code** - the DMA path never synchronised | **High** | called, with stale-EOF cleared first |
| B1a | **DMA start / SPI_UPDATE ordering** - engine stayed PARKED on the first transfer | **High** | `gdma_start()` now precedes `spi2_sync()` |
| B2 | Descriptor address truncated to a 20-bit field, unvalidated | Med | `gdma_desc_addr_ok()` checks internal SRAM |
| B3 | AFIFOs never reset per transfer; latched outfifo-empty error never cleared | Med | mirrors IDF's `spi_hal_hw_prepare_tx()` |
| B4 | `clk_set_cpu_pll()` switched **blind** - no readback | Med | verifies `SOC_CLK_SEL`; added `clk_set_cpu_xtal()` |
| B6 | Vector register allocation was accidental | Low | documented as a contract in `vec.h` |
| B7 | DMA length never checked against the 12-bit size field | Med | returns `SPI2_E_LEN`; **a landmine directly in banding's path** |

#### B1 was concealing B1a, and that is the real lesson

`gdma_wait()` existed but was never called. Calling it immediately exposed a
timeout on the **very first** DMA transfer, every boot, deterministically.

Diagnosis came from dumping state rather than guessing - four hypotheses
(stop-before-start, module clock gate, address-latch ordering, outfifo-empty
clear) all failed before the data pointed at the answer:

```
raw=0x00000000  link=0x008A8550  conf0=0x00000038  desc.dw0=0xC02E02E0
                     ^ OUTLINK_PARK=1              ^ owner=1, never fetched
```

Root cause: **`gdma_start()` must precede `SPI_UPDATE`, not follow it.** IDF's
order is `prepare_tx -> dma_start -> apply_config -> user_start`; ours applied
the config first, and the first transfer's DMA request was missed.

What makes this worth recording is the *shape* of the failure. With no
synchronisation, SPI completed anyway and shipped **stale AFIFO contents** -
one garbage frame per boot, no error, nothing visible. Dead synchronisation
code was hiding a real hardware bug behind a plausible-looking display.

#### Cost of correctness

55.3 -> 54.4 fps, about 1.6%, for the added `gdma_wait()`. Verified over a
45-second run: 112 reported frames, **zero failures**, flush constant at
17.46 ms to the hundredth.

### 6.6j Banding + render/DMA overlap - the gap is closed

Two changes, each aimed at a measured cost.

**Banding** collapses 448 single-row transfers into **14 banded descriptor
chains**. Per-transfer overhead was 0.97 ms above the 16.49 ms of real wire
time. The descriptor size field is 12 bits, so one descriptor carries at most
4095 bytes - 5 rows - and a band is therefore a CHAIN. We use 4 rows per
descriptor (2944 B) so a 32-row band divides evenly into 8.

**Overlap** hides rendering. DMA is asynchronous and the CPU was idle for the
entire transfer, so band N+1 is now rendered *while band N is on the wire*.
`spi2_dma_start()` / `spi2_dma_finish()` split the old blocking call. Frame
time becomes max(render, flush) rather than their sum.

| | before | after |
|---|---|---|
| Full frame (448 rows) | 18.30 ms - **110%, misses** | **16.60 ms - 99%, FITS** |
| Cost per row | 0.041 ms | **0.037 ms** |
| 32-row elided update | 1.30 ms | **1.20 ms** |
| Transfers per frame | 448 | **14** |
| Full-frame fps | 54.6 | **60.2** |

1.70 ms recovered of the 1.75 ms that overhead and render together cost - very
close to the predicted ceiling.

**Every row is now updatable at 60 Hz.** Combined with elision, a typical
interface update (32 rows) costs 1.20 ms, or 7.2% of the frame period - about
14x headroom.

Verified over 20 seconds: ~11,000 frames, **zero failures**, resync firing on
schedule.

The FIFO transport is retained alongside, unbanded and synchronous, because
measuring both on identical work has repeatedly caught wrong assumptions.

### 6.6k Red-team of banding, overlap and elision

Six findings. Two were only findable by removing a safety net.

**R-A - the overlap claim was unverifiable from telemetry.** `flush` is measured
as time spent *waiting*, which rendering has already shortened, so the numbers
could not distinguish overlap from no-overlap. Added `sh8601_set_overlap()` as a
control: **ON 16.6 ms / OFF 17.4 ms**. Overlap is real, and *load-bearing* -
banding alone misses 60 Hz at 104% of budget.

**R-B - dirty-tracking leak, found only with resync OFF.** The demo marked
`g_bar_prev`, which held the position from TWO frames ago. It worked by
accident: a 4px step and 24px bar overlap so heavily the union covered the gap.
**At the wrap the positions stop being adjacent and the cover fails**, leaving
red behind permanently. Resync scrubbed the evidence every 120 frames, so
~11,000 frames had already run "verified".

> The safety net worked so well it hid the thing it was protecting against.
> `elide_set_resync(0)` exists so this is testable.

**R-C - full-frame 60 Hz was a knife-edge.** 16.6 ms against 16.67 ms is 70 us
of slack. Measured: **19 late frames against 14 resyncs** - every full-repaint
resync missed its deadline. Fixed by making resync **rolling**: refresh
`448/120 = 4` rows per frame instead of everything every 120 frames. Same drift
protection, no 16.6 ms spike. Late frames in steady state went to **zero**.

**Cold-start DMA failure returned in the banded path.** Characterised, not
explained: the first `gdma_start()` after init is ignored - engine PARKED,
descriptor never fetched - while a second start takes **provided the channel is
not reset in between**. Resetting undoes whatever the first arm primes, which is
why an earlier reset-then-retry did not work. Recovery is a re-arm without
reset. Priming with a dummy descriptor at init did **not** work.

**Guards were 53x-106x oversized.** The longest legitimate wait is one 32-row
band, ~1.18 ms. Limits of 1,000,000 / 2,000,000 made a failed first transfer
cost **293 ms** before its retry ran. Cut to 200,000 (11x margin): boot frame
**292.8 -> 45.3 ms**, late frames **19 -> 2**, both at boot.

**Per-stage error codes.** A single `SPI2_E_HANG` said a transfer failed but not
*where*, and three rounds were spent fixing the wrong stage. Now `E_SYNC`,
`E_USR`, `E_DMA`.

### 6.6l Status: banded DMA is DISABLED. FIFO ships.

**Requirement met with the FIFO transport.** A 104-row interface update costs
7.1 ms against a 16.67 ms budget - 2.3x headroom, zero late frames in steady
state, verified by eye as smooth continuous motion with nothing left behind.

**Banded DMA is 1.8x faster and is not needed for 60 Hz.** It corrupts
periodically - a full-screen flash of the foreground colour - and is disabled
until that is understood.

#### What is actually known

| | |
|---|---|
| FIFO + elision + marking + 60Hz pacing | **verified working** |
| Panel liveness check at boot | works, and removes the wedged-vs-wrong-code ambiguity |
| Banded DMA steady-state throughput | 3.8 ms / 104 rows, 16.6 ms full frame |
| Banded DMA correctness | **periodic corruption, cause unknown** |
| Boot cold-start DMA swallow | characterised, aborts cleanly, costs one frame |

#### How the investigation went wrong

Worth recording, because the process failure was larger than the bug.

1. **Multiple variables at once.** DMA was tested while transport switching,
   elision, rolling resync and pacing were all changing. No observation could
   isolate a cause.
2. **A human used as the instrument.** Every hypothesis needed someone to look
   at a screen and describe it, so each round cost minutes and the descriptions
   could not distinguish "wedged panel showing stale content" from "wrong
   pixels being drawn". Several conclusions were drawn from a frozen image.
3. **Hypotheses swapped instead of narrowed.** Stop-before-start, module clock
   gate, address latch ordering, outfifo-empty clear, settle delay, retry,
   abort - each replaced the last rather than eliminating a class.
4. **An error path that corrupted.** The retry re-sent a band into a CS-held
   stream, shifting everything after it. Added because the root cause was not
   understood, without asking whether retrying was safe there.

**What should have happened:** build a self-checking harness first - draw a
known pattern, read back what the panel shows, compare on-device - and only
then change one variable at a time. The panel cannot be read back, so the
equivalent is a host-side reference render compared against a captured frame
description, or an on-device checksum of what was actually transmitted.

Until that exists, banded DMA stays off. It is an optimisation on a path that
already has 2.3x headroom.

### 6.7 Transfer path: CPU FIFO first

Milestone 2 uses the **64-byte CPU FIFO** (`SPI_W0..W15`), not DMA.

Rationale: if SPI2 configuration and GDMA descriptors are written together and
the screen stays black, the fault could be in either and they fail identically.
The FIFO path is slow — 368x448x2 = 329,728 B in 64-byte chunks is ~5,152
transactions, likely seconds per frame — but if bars appear, **the protocol is
proven** and DMA becomes purely a speed problem with a known-good reference.

Loop: write up to 64 B into `SPI_W0..W15`; set `SPI_MS_DLEN` to the bit count;
set `SPI_CMD.USR`; poll `SPI_CMD.USR` until clear; repeat.

### 6.8 Deliverables

| File | Contents |
|---|---|
| `src/spi2.c` / `.h` | clock/reset, pin routing, SPI2 config, FIFO transfer |
| `src/sh8601.c` / `.h` | init sequence, address window, pixel write |
| `src/main.c` | color-bar test |

---

## 6.9 PROJECT RULE: no scalar per-element math

**Non-negotiable.** All bulk data work goes through the LX7's 128-bit vector
unit. No scalar loops over pixels or bytes.

Two blockers had to be cleared before this was even possible:

1. **GCC-for-Xtensa does not auto-vectorise to `EE.*`.** They must be written as
   inline `__asm__`. That is ISO-legal - the reserved `__asm__` spelling, not the
   bare `asm` keyword `-pedantic-errors` rejects - and consistent with the
   `rsr`/`waiti` we already use. ESP-DSP and ESP-NN are dependencies we exclude.
2. **The vector unit is a gated coprocessor** (`XCHAL_HAVE_CP = 1`). Nothing
   enables it bare metal, so the first `EE.*` traps as Coprocessor Disabled.
   `start.c` now sets `CPENABLE = 0xFF` before anything else. With no context
   switching, enabling all coprocessors permanently is safe and free.

### Primitives (`vec.h` / `vec.c`)

| Function | Implementation |
|---|---|
| `vec_fill16` | `EE.VLDBC.16` broadcast + `EE.VST.128.IP` loop |
| `vec_copy` | `EE.VLD.128.IP` / `EE.VST.128.IP` |
| `vec_zero` | `EE.ZERO.Q` + `EE.VST.128.IP` |

**Alignment contract:** 16-byte aligned pointers, lengths a multiple of 16 B.
Use `VEC_ALIGN`. Conveniently **368 px = 736 B = exactly 46 vectors**, so a full
row divides evenly and needs no scalar tail handling.

### ISA probe results

Probed by assembling each mnemonic. Available and useful:

| Category | Instructions |
|---|---|
| 16-bit lane arithmetic | `ee.vadds.s16` `ee.vsubs.s16` `ee.vmul.s16` `ee.vmul.u16` |
| Logical | `ee.andq` `ee.orq` `ee.xorq` |
| Broadcast / lane insert | `ee.vldbc.16` `ee.vldbc.32` `ee.movi.32.q` |
| Byte reorder | `ee.vzip.8` `ee.vunzip.8` `ee.vzip.16` |
| Min/max, 32-bit shift | `ee.vmin.s16` `ee.vmax.s16` `ee.vsl.32` `ee.vsr.32` |

**Not available:** `ee.vsl.16` / `ee.vsr.16` (no 16-bit lane shift), `ee.shfqi`,
`ee.slci.2q`, `ee.srci.2q`.

The missing 16-bit shift matters less than it looks. In RGB565 **wire format**
(byte-swapped) the memory word is `(lo << 8) | hi` where `hi = RRRRRGGG` and
`lo = GGGBBBBB`. So **red sits at bits 3-7 and blue at bits 8-12 of the memory
word** - both contiguous. Ramping red or blue is plain vector addition with no
byte swap required. Only green straddles the byte boundary.

### Measured

Row fill of 368 px: **364 cycles = 0.99 cycles/pixel** (scalar was ~3-4).
Verified on hardware: fill, zero and copy all produce correct data, and the
panel renders from vectorised fills.

### 6.9a Why the rule exists HERE, and what it costs to break

Measured 2026-08-24 with `tools/isa_probe.sh` and instruction counts from the
real toolchain, because "scalar is slow" is a generic claim and this project
should carry a specific one.

**1bpp glyph expansion**, the shape all text rendering reduces to - broadcast a
byte of glyph bits, isolate one bit per lane, select fg/bg:

| | instr/pixel | 368-px row | of the 69 us row cost |
|---|---|---|---|
| scalar | **9.0** | 20.7 us | 30% |
| vector | **1.375** | 3.16 us | 4.6% |

**6.5x**, in 11 instructions that fit exactly the 8 available q registers:

```
ee.vldbc.8     q0, bits    broadcast the glyph byte to all lanes
ee.andq        q0, q0, q1  isolate one bit per lane {0x80,0x40,...,0x01}
ee.vcmp.eq.s16 q3, q0, q2  mask = lanes whose bit was clear
ee.notq        q4, q3
ee.andq        q7, q6, q3  bg where clear
ee.andq        q0, q5, q4  fg where set
ee.orq         q0, q0, q7
ee.vst.128.ip  q0, dst, 16
```

(Instruction counts, not measured cycles: several of these are multi-cycle and
the scalar loop uses Xtensa's zero-overhead `loop`. The ratio is indicative.)

**THE REASON IS NOT "SCALAR IS SLOW". IT IS THAT THE THESIS DEPENDS ON COMPUTE
STAYING NEGLIGIBLE.**

Rendering is currently 0.002 ms/row - **1.4% of a frame**. That figure is what
makes 3.0 true: the panel is wire-bound, so the only lever is sending fewer
bytes. Scalar text would take rendering to 30% of a frame, and at that point
there is no longer one bottleneck to reason about linearly - there are two, and
"the fastest pixel is the one never sent" stops being the dominant lever.

So the rule is: **keep compute negligible so the system stays wire-bound.**
That is also what says when it may be relaxed - the digest (fold.c) and the host
harness are exempt precisely because they are not on the render path and do not
move the compute-to-wire ratio.

### 6.9b Where the ISA actually runs out

Probed on gcc 14.2.0. What is missing matters as much as what is present.

| want | available | consequence |
|---|---|---|
| permute / gather (`vperm`, `vtbl`, `vshuf`, `vgather`, `vsel`, `vlut`) | **none** | no single-instruction table lookup; palette expansion costs `(5N+4)` instr per 8 px and only beats scalar below ~12 entries (5.1) |
| 16-bit vector shift (`vsr.16`, `vsl.16`, `vsra.16`) | **none** - only `.32` | RGB565 channel extraction has no clean path |
| funnel shift across two vectors | `ee.src.q`, `ee.slci.2q`, `ee.srci.2q` | unaligned glyph placement IS covered |
| compare + logical | `ee.vcmp.eq/lt/gt`, `andq`, `orq`, `notq` | a complete SELECT primitive - the workhorse for glyphs and palettes |

The missing 16-bit shift is the one real wall. Anything needing per-channel
arithmetic on packed RGB565 - alpha blending, anti-aliased text, gradients,
fades, colour interpolation - has to drop to 32-bit lanes (4 px per vector
instead of 8) or fake a left shift with `ee.vmul.s16` by 2^k. Right shift has no
clean path at 16-bit width. **Crisp 1bpp text needs none of this; anti-aliased
text needs all of it.**

None of this is surprising once you notice what DOES assemble: `ee.vrelu.s16`,
`ee.vprelu.s16`, and `ee.vmulas.*` into 40-bit accumulators. This is the AI
extension - built for inference MACs and activations. Table lookup and channel
swizzling were never design targets.

### Where scalar remains, and why

| Site | Status |
|---|---|
| Row fills | vectorised (`vec_fill16`) |
| `.bss` zeroing | **vectorised** - linker aligns/pads `.bss` to whole 128-bit vectors so `vec_zero` covers it with no scalar tail |
| Vertical gradient | vectorised - one colour per ROW (448/frame), then a broadcast fill |
| `spi2_xfer` FIFO load | **vectorised** - see 6.9c |
| `sh8601_rgb565` | single value, not bulk |
| `con_dec` | console formatting |
| Loop counters, addresses, branches | inherent control flow |

### 6.9c Vectorised MMIO - both blockers dissolved

I had written off vectorising the FIFO load. Both reasons turned out to be
wrong, and testing them cost under an hour.

**Blocker 1 - alignment. Real, and sidestepped.**
`SPI_W0` is at `0x60024098`: `% 4 = 0`, `% 8 = 0`, but **`% 16 = 8`**. So
`EE.VST.128.IP`, which requires 16-byte alignment, genuinely cannot target it.
But `EE.VST.L.64.IP` / `EE.VST.H.64.IP` need only **8-byte** alignment, which
`W0` satisfies exactly. One 128-bit register goes out as two 64-bit stores.

**Blocker 2 - can vector stores reach peripheral space? YES.**
Assumed no; never checked. Tested directly by storing a known pattern to
`SPI_W0..W3` and reading it back scalar:

```
W0 = 0x11223344   W1 = 0x55667788
W2 = 0x99AABBCC   W3 = 0xDDEEFF00
PASS  128b via 2x64b store reached MMIO
```

**Result: 3.8x on a full frame, before GDMA.**

| Frame | Scalar packing | Vectorised | Speedup |
|---|---|---|---|
| Colour bars | 311 ms | **82 ms** | **3.8x** |
| Gradient | 599 ms | **83 ms** | **7.2x** |
| fps | 3.2 | **12.2** | |

The gradient is now the same cost as flat bars - per-pixel work has effectively
vanished. Bus utilisation rose from 5.3% to 20%.

**Alignment contract.** `spi2_xfer` now REJECTS misaligned input with
`SPI2_E_ALIGN` rather than silently falling back to a scalar path - a silent
fallback is exactly the failure mode this project keeps getting bitten by.
Command words and parameter blocks are `VEC_ALIGN` and padded to 16 bytes; the
FIFO load rounds up to whole vectors and `MS_DLEN` bounds what is actually
transmitted, so the padding is never sent.

**GDMA still matters** - 20% bus utilisation means 80% is still overhead - but
it is now an optimisation rather than a rescue.

## 7. Graphics messaging layer

> **SUPERSEDED (2026-08-24) — kept as the reasoning, not the design.**
>
> This section specifies a NeoGPU port: an opcode stream, channels, an
> `HSBackendOps` vtable and an `hs_backend_sh8601.c` implementing it. **None of
> that shipped.** What shipped is `gfx.c`, 101 lines keeping a 448-row
> descriptor model and diffing it.
>
> The reason is in §7.4 and worth stating plainly: a command-list backend
> replays into a framebuffer, and there is no framebuffer — because the panel is
> wire-bound and storing pixels does not send fewer of them (§5.1), not because
> one would not fit. Rows stream straight to the panel. So the layer that
> was specified as "opcodes in, pixels out" became "what should each row look
> like, and which of those changed". That inversion is the whole 60 Hz
> architecture (§3.0); the vtable would have been machinery around it.
>
> Read this for *why* the messaging layer exists and what it was measured
> against. Read §3.0 and `metal99/src/gfx.c` for what it is.

Port of the NeoGPU graphics core (`github.com/anjaustin/neogpu`), ML and GLES
excluded.

### 7.1 Scope

The graphics-only subset is **4,856 lines**, not the repo's 47K:

| File | Lines | Disposition |
|---|---|---|
| `hs_core.c` | 1455 | Port |
| `hs_nodes.c` | 847 | Port |
| `hs_gpu.c` | 382 | Port |
| `hs_async.c` | 281 | **Drop** (POSIX threads) |
| `hs_math_neon.h` | 514 | **Replace** (scalar C99) |
| `hs_msg.h`, `hs_core.h`, `hs_buffer.h`, `hs_nodes.h`, `hs_gpu.h`, `hs_render.h`, `hs_backend.h` | ~1377 | Port |

No ML or GLES symbols appear in any core `.c` file — the subsystems are cleanly
separable.

### 7.2 NEON is a non-issue

`hs_core.h` includes `<arm_neon.h>`, but the message layer **uses no vector
types**. The include is vestigial. Deleting one line removes NEON from the
messaging layer entirely. `hs_math_neon.h` is only needed by the 3D math path,
which the display backend does not use.

### 7.3 Concurrency is the real work

`hs_core.h` pulls in `<pthread.h>` and `<stdatomic.h>`, and the submit queue is
built for 8 concurrent producers:

```c
typedef struct {
    atomic_uint seq;
    Message msg;
    u32 payload_len;
    u8  payload[HS_PAYLOAD_SIZE];
} __attribute__((aligned(64))) HSSubmitSlot;
```

**`<stdatomic.h>` is C11 and directly violates constraint 1.** Unlike the NEON
include it is load-bearing.

Replacement model — single core, no preemption, no interrupts (§1.1):

| POSIX construct | C99 bare-metal replacement |
|---|---|
| `atomic_uint` | plain `volatile unsigned` |
| `pthread_mutex_lock/unlock` | interrupt-disable critical section (`rsil`) |
| `pthread_cond_*` | delete — nothing blocks |
| lock-free MPSC ring | single-producer ring; no CAS needed |

Because Milestone 2 polls rather than using interrupts, the critical sections
can initially be **empty macros**. They become real `rsil`/`wsr ps` pairs only
if an ISR is ever introduced. That keeps the port honest without paying for
synchronisation nothing needs yet.

> This is a rewrite of the concurrency model, not a search-and-replace. It is
> the single largest piece of work in §7 and should be scoped as such.

### 7.4 Embedded capacity profile

Defaults are desktop-sized. Measured struct sizes: `Message` 20 B,
`Payload` 128 B, `HSSubmitSlot` 128 B.

| Constant | Desktop | Embedded | Cost at embedded |
|---|---|---|---|
| `HS_MAX_MSG_LOG` | 65536 | **512** | 10.0 K |
| `HS_MAX_PAYLOADS` | 4096 | **128** | 16.0 K |
| `HS_SUBMIT_SIZE` | 1024 | **64** | 8.0 K |
| **Total** | **1920 K** | | **34 K** |

1920 K is 3.75x the entire SRAM; 34 K fits comfortably (§5). These are
compile-time constants and scale linearly, so this is configuration, not
redesign. The capture/replay feature is the first thing constrained by the
smaller log.

### 7.5 Backend interface

```c
typedef struct {
    bool (*init)(void* ctx, HSGpu* gpu);
    void (*shutdown)(void* ctx, HSGpu* gpu);
    void (*begin_frame)(void* ctx, const HSFrameContext* frame);
    void (*execute)(void* ctx, const HSFrameContext* frame);
    void (*end_frame)(void* ctx, const HSFrameContext* frame);
} HSBackendOps;
```

`hs_backend_sh8601.c` implements this vtable:

| Op | Behaviour |
|---|---|
| `init` | SPI2 bring-up + SH8601 init (§6), allocate band buffer |
| `begin_frame` | reset band cursor / dirty tracking |
| `execute` | consume the render command list, rasterize into bands |
| `end_frame` | flush remaining bands, set address window, push pixels |
| `shutdown` | display off |

Note `bool` requires `<stdbool.h>`, which is C99 — fine.

### 7.6 Other host assumptions to replace

| Used by core | Replacement |
|---|---|
| `fprintf` | `con_puts` / `con_dec` (`io.c`) |
| `clock_gettime` | `rsr ccount` cycle counter |
| `read` / `write` | delete (IPC path, not needed) |
| `memcpy` / `memset` | own implementations — no libc |

---

## 8. Verification plan

Each milestone has a criterion that fails loudly rather than silently.

| # | Deliverable | Success criterion | Status |
|---|---|---|---|
| 1 | Boot, console, watchdogs | Stable heartbeat over USB-Serial-JTAG, no reset loop | **DONE** — 960 B image |
| 2a | SPI2 from registers | 40 MHz bus, quad mode confirmed by timing | **DONE** — 40000 kHz, PASS |
| 2b-i | Command path (brightness) | Frozen image pulses under command | **DONE** |
| 2b-ii | Pixel path | Colour bars + gradient render correctly | **DONE** |
| 2b-iii | Our own init sequence | init + redraw verified; sleep/wake unsupported | **DONE** |
| 2b-iv | Power-cycle cold start | Bars/magenta alternate after physical replug | **DONE** |
| 3 | GDMA transfer | Same bars, full frame under 25 ms | specified |
| 4 | Renderer integration | `render_c99.c` plasma at >= 25 fps | specified |
| 5 | Messaging layer | Opcode stream drives the panel; `OP_PRESENT` produces a visible frame | specified |

**Visual verification is mandatory.** A silent black screen is the expected
failure mode for every register-level bug in Milestone 2 — wrong pin, wrong bit,
wrong init byte all look identical from the console. Console output alone does
not constitute success.

---

## 9. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| ~~SPI2 register bit layouts~~ | ~~High~~ | **RESOLVED** — implemented and verified, §6.4 |
| CPU at 20 MHz starves rendering (§4.3) | **High** | PLL enablement required before Milestone 4 |
| D0–D3 to FSPID/Q/WP/HD mapping wrong (§6.3) | Medium | Produces scrambled — not blank — output; diagnosable from bars |
| QSPI failures are silent | Medium | FIFO before DMA; bars before animation |
| Concurrency rewrite (§7.3) larger than estimated | Medium | Empty critical sections initially; no ISR exists yet |
| ROM stack too small for our call depth | Low | Switch to own stack via `_stack_top` if it bites |
| Image outgrows 128 K IRAM | Low | 879 B used of 128 K |

---

## 10. Open questions

1. **Is NeoGPU the target, or a source of ideas?** Milestone 2 is unchanged
   either way — the register work is identical whether it produces a standalone
   demo or an `HSBackendOps` implementation. The decision can be deferred until
   after the panel lights up.
2. **Does the message log matter on-device?** It is the largest single consumer
   at desktop defaults and the feature most constrained by the embedded profile.
3. **Second core.** Currently unused. Rendering on core 1 while core 0 flushes
   is the obvious path to the ~45 fps ceiling, but requires starting the APP CPU
   by hand.

---

## Appendix A — Register reference

Confirmed against ESP-IDF v5.5.5 `soc/esp32s3` headers.

| Peripheral | Base |
|---|---|
| UART0 | `0x60000000` |
| GPIO | `0x60004000` |
| RTC_CNTL | `0x60008000` |
| IO_MUX | `0x60009000` |
| TIMG0 | `0x6001F000` |
| TIMG1 | `0x60020000` |
| SPI2 | `0x60024000` |
| USB-Serial-JTAG | `0x60038000` |
| GDMA | `0x6003F000` |
| SYSTEM | `0x600C0000` |

| Register | Address |
|---|---|
| `GPIO_OUT_W1TS` | `0x60004008` |
| `GPIO_ENABLE_W1TS` | `0x60004024` |
| `GPIO_FUNC0_OUT_SEL_CFG` | `0x60004554` (+4 per GPIO) |
| `SYSTEM_PERIP_CLK_EN0` | `0x600C0018` |
| `SYSTEM_PERIP_RST_EN0` | `0x600C0020` |
| `USJ_EP1` (console FIFO) | `0x60038000` |
| `USJ_EP1_CONF` | `0x60038004` |

---

## Appendix B — Build and flash

```sh
./metal99/build.sh                                    # gcc + ld + elf2image
esptool --port /dev/ttyACM1 write-flash 0x0 metal99/build/fw.bin
```

No ESP-IDF environment required. To restore the stock firmware:

```sh
cd backup && sha256sum -c stock-full-16MB-20260823.bin.sha256
esptool --port /dev/ttyACM1 write-flash 0x0 stock-full-16MB-20260823.bin
```
