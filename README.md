# metal99

A **60 Hz graphics runtime** for the Waveshare ESP32-S3-Touch-AMOLED-1.8, in
**pure ISO C99** with **zero dependencies**.

No ESP-IDF. No FreeRTOS. No libc. No ROM calls. The firmware is loaded by the
mask ROM from flash offset `0x0` straight into SRAM, brings up its own clocks,
drives SPI2 and the SH8601 panel from raw registers, and renders through the
LX7's 128-bit vector unit.

```
 2,240-byte boot image  ·  0 dependencies  ·  -std=c99 -pedantic-errors -Werror
```

## Status

| | |
|---|---|
| Boot, clocks, watchdogs | verified |
| SPI2 QSPI @ 40 MHz, quad mode | verified by timing |
| SH8601 panel, cold start | verified by power cycle |
| 128-bit vector primitives | verified |
| Elision + 60 Hz pacing | **shipping** — 2.3x headroom |
| Self-checking harness | validated against injected faults |
| `gfx` messaging layer | working |
| Banded GDMA | parked — data proven identical, timing suspect |

## Quickstart

```sh
./tools/flash.sh -c 20        # build, flash to 0x0, capture 20s of output
./tools/capture.py -r -s 15   # reset and watch the console
```

No toolchain environment to source. `build.sh` invokes gcc, ld and esptool
directly; esptool only packs the image and is not linked in.

To restore the stock Waveshare firmware, see [`backup/RESTORE.md`](backup/RESTORE.md).

## Layout

| Path | |
|---|---|
| `metal99/src/` | the firmware — 2,400 lines |
| `metal99/build.sh` | gcc + ld + esptool, no build system |
| `tools/` | research instruments (capture, register lookup, ISA probe) |
| `tests/host/` | run renderers on a desktop, ~200 ms iteration |
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
| `spi2.c` | SPI2 QSPI from registers, transmit ledger |
| `gdma.c` | GDMA descriptor chains (parked) |
| `sh8601.c` | panel init, address window, row/span writes |
| `elide.c` | dirty-row tracking, span coalescing, rolling resync |
| `gfx.c` | retained-mode layer — dirtiness derived, not declared |
| `selftest.c` | on-device verification + fault injection |

## Performance

Measured on hardware, 368x448 RGB565 at 40 MHz QSPI.

| | |
|---|---|
| Cost per row | **0.037 ms**, linear from 1 to 448 rows |
| Full repaint (448 rows) | 16.6 ms — **fits the 16.67 ms budget** |
| Typical interface update (104 rows) | **7.1 ms** — 2.3x headroom |
| Redundant repaint | **0 rows transmitted** |

Linearity is the useful part: a designer can budget rows directly. **All 448
rows are updatable at 60 Hz**, so even a full repaint fits — but interfaces
rarely need one, and the layer elides what did not change.

### How it got here

| Stage | Frame | fps |
|---|---|---|
| FIFO, 20 MHz CPU | 311 ms | 3.2 |
| + vectorised MMIO | 82 ms | 12.2 |
| + GDMA | 28 ms | 35.5 |
| + PLL 160 MHz | 18.4 ms | 54.4 |
| + banding & overlap | 16.6 ms | 60.2 |
| + elision (typical update) | **7.1 ms** | — |

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

## Contributing

Two non-negotiable rules — pure C99 and no scalar per-element math — plus a
verification discipline earned the hard way. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT — see [LICENSE](LICENSE).
