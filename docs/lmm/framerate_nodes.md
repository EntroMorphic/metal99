# NODES — what actually matters from the raw dump

Filtering signal from noise. Not everything in RAW earns a place here.

---

## N1. The 3 fps number measures our stepping stone, not the hardware

311 ms/frame, of which **16.5 ms is wire time**. 94.7% is per-transaction setup
across 5,152 FIFO transactions. FIFO was chosen deliberately to isolate protocol
bugs from DMA bugs. It did its job. Quoting its throughput as a hardware property
is a category error.

**Status: fact, measured.**

## N2. The real ceiling at the current clock is 60 fps, not 35

    frame = 2,637,824 bits over 4 lines
    40 MHz SDR -> 16.49 ms -> 60.6 fps
    80 MHz SDR ->  8.24 ms -> 121 fps
    80 MHz DDR ->  4.12 ms -> 243 fps

The "~35 fps" I published was imported from ESP-IDF measurements taken on a
different transport (DMA + PSRAM framebuffer + bounce buffer). It was never our
number.

**Status: arithmetic, certain. Correction owed to the spec.**

## N3. This silicon supports DDR SPI

`SOC_SPI_SUPPORT_DDRCLK = 1`. Data on both clock edges — 2x with no clock
increase, no extra pins, no extra power.

**Status: silicon capability confirmed. Panel acceptance UNKNOWN.**

## N4. The 40 MHz clock is a vendor default, not a proven limit

Waveshare's BSP chose it. Vendor BSPs are conservative. We have no SH8601
datasheet. But we have something better for this specific question: **a panel
that displays its own corruption.** Colour bars make marginal signalling
instantly visible. The limit is empirically binary-searchable in ~20 minutes.

**Status: unknown, but cheaply knowable.**

## N5. Full-frame fps is the wrong metric for an interface

Interfaces do not repaint the world. The number that matters is *small updates
per second* and *latency to glass*:

    full frame @ 80 MHz SDR = 8.24 ms
    32x32 tile @ 80 MHz SDR = 51 us   -> ~19,500 updates/sec
    32x32 tile @ 80 MHz DDR = 25.6 us -> ~39,000 updates/sec

This is where the "1M fps" spirit actually lives on this hardware — not repaint
rate, but the cost of touching what changed.

**Status: reframe. The most important node here.**

## N6. The panel's framebuffer is a free compositing surface

`0x2A`/`0x2B` already set arbitrary address windows; we just always set
full-screen. The panel retains its own frame memory — the exact property that
produced ghost images and fooled us three times.

The trap and the asset are the same fact. We were overwriting a persistent
compositing surface wholesale, every frame, for no reason.

**Status: mechanism already built and verified. Currently misused.**

## N7. GDMA descriptor chains can replay the same memory

A solid fill needs one small buffer and N descriptors pointing at it. Same for
vertical gradients (one row per distinct value), tiles, repeated sprites. Near
zero RAM, zero CPU per pixel. The DMA engine becomes a crude blitter.

**Status: strong claim, unverified. Depends on GDMA descriptor semantics.**

## N8. Panel-side vertical scroll may exist

MIPI DCS `0x33` VSCRDEF / `0x37` VSCRSAR. If SH8601 implements them, scrolling
costs one command rather than a repaint. Not a multiplier — an asymptote change
for one very common interaction.

**Status: unknown. Cheap to test.**

---

## Tensions

### T1. Dirty-rect tracking costs CPU, and CPU is the scarce resource
At 20 MHz, per-pixel diffing would cost more than the transfer it saves. Any
dirty scheme must be far cheaper than the pixels it avoids — tile-level hashing
or explicit invalidation, never per-pixel comparison. **Resolves after PLL, or
by choosing a coarse scheme.**

### T2. Small updates re-introduce per-transaction overhead
Each windowed update costs a command preamble (`0x2A`, `0x2B`, `0x32`/`0x2C`
plus params). Below some tile size, overhead dominates again — the same trap as
the FIFO path, at a different scale. **There is an optimal tile size and it is
measurable.**

### T3. Every attempt to design the message layer has been overtaken by a
hardware fact
Twice now. The layer's correct shape depends on whether flush is synchronous or
asynchronous, and on whether updates are frames or regions. Both are still open.
**Designing it now would bake in assumptions we are about to disprove.**

### T4. Bus levers and CPU levers are independent, and only one is on our side
Flush is bus-bound (GDMA, clock, DDR). Render is CPU-bound (PLL, both cores,
SIMD). They multiply rather than overlap — but at 240 MHz with an 80 MHz bus,
**render becomes the bottleneck**, which inverts the current situation.

---

## Constraints that are not negotiable

- **No framebuffer in RAM.** 322 KB frame vs 192 KB DRAM region. Row/band
  streaming is structural, not a preference.
- **GPIO matrix routing is mandatory** (board pinout conflicts with IO_MUX
  defaults). The documented matrix penalty concerns *slave sampling on read*;
  we only ever write. Probably not a limit — but it is an assumption, not a fact.
- **Pure ISO C99, zero dependencies.** Every idea here must be expressible that
  way. None of them obviously aren't.
- **CPU is at 20 MHz until the PLL is enabled.** Every CPU-side idea is
  12x cheaper than it looks today.
