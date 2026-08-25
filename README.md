# metal99

A **60 Hz graphics runtime** for the Waveshare ESP32-S3-Touch-AMOLED-1.8, in
**pure ISO C99** with **zero dependencies**.

No ESP-IDF. No FreeRTOS. No libc. No ROM calls. The firmware is loaded by the
mask ROM from flash offset `0x0` straight into SRAM, brings up its own clocks,
drives SPI2 and the SH8601 panel from raw registers, and renders through the
LX7's 128-bit vector unit.

```
 8,528-byte boot image  ·  0 dependencies  ·  -std=c99 -pedantic-errors -Wshadow -Werror
```

## Status

| | |
|---|---|
| Boot, clocks, watchdogs | verified |
| SPI2 QSPI @ 40 MHz, quad mode | verified by timing |
| SH8601 panel, cold start | verified by power cycle |
| 128-bit vector primitives | verified |
| Elision + 60 Hz pacing | **shipping** — 2.2x headroom on typical updates |
| Self-checking harness | validated against injected faults — **but see the caveat below** |
| `gfx` messaging layer | working |
| Banded GDMA | parked — data proven identical, timing suspect |

## Quickstart

```sh
./tools/flash.sh -c 20        # build, flash to 0x0, capture 20s of output
./tools/capture.py -r -s 15   # reset and watch the console
make -C tests/host test       # digest assertions, no board required
```

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
| `tools/` | research instruments (capture, register lookup, ISA probe) |
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
| `gfx.c` | retained-mode layer — dirtiness derived, not declared |
| `selftest.c` | on-device verification + fault injection |

## Performance

Measured on hardware, 368x448 RGB565 at 40 MHz QSPI. **Two transports, and which
one you mean matters:** FIFO ships, banded GDMA is parked.

### What ships — FIFO

| | |
|---|---|
| Cost per row | **0.069 ms** (0.068 flush + 0.002 render), linear |
| Typical interface update (104 rows) | **7.3 ms** — 2.2x headroom, zero late frames |
| Moving a 96-row element | **6.7 ms** — 2.4x headroom |
| Full repaint (448 rows) | **31.2 ms** — 31.9 fps, *misses* the 16.67 ms budget |
| 60 Hz boundary | **~240 rows**, 54% of the screen |
| Redundant repaint | 0 rows *changed*; 4 rows/frame still sent by the rolling resync |

### Parked — banded GDMA

| | |
|---|---|
| Cost per row | 0.037 ms — the 40 MHz wire time and nothing above it |
| Full repaint (448 rows) | 16.6 ms — would fit the 16.67 ms budget |

Banded GDMA is ~1.8x faster and is the only way a *full* repaint fits 60 Hz. It
delivers byte-identical data — the on-device self-test digests 448 rows to
`0xF5642645` through both transports — and the panel still looks visibly worse,
so it stays disabled until that is understood. See
[`docs/DESIGN.md`](docs/DESIGN.md) §6.6l.

**The useful claim is linearity, not a full-frame number.** A designer can budget
rows directly, and interfaces do not repaint whole screens — the elision layer
transmits only what changed. An earlier version of this table quoted 0.037 ms/row
and "all 448 rows at 60 Hz" as if they described what ships; those are the parked
transport's figures, and the table contradicted itself (0.037 x 104 = 3.9 ms
against a measured 7.3 ms).

### How it got here

| Stage | Frame | fps | |
|---|---|---|---|
| FIFO, 20 MHz CPU | 311 ms | 3.2 | |
| + vectorised MMIO | 82 ms | 12.2 | |
| + GDMA | 28 ms | 35.5 | |
| + PLL 160 MHz | 18.4 ms | 54.4 | |
| + banding & overlap | 16.6 ms | 60.2 | **parked** |
| FIFO + PLL, full repaint | 31.2 ms | 31.9 | **ships** |
| + elision (104-row update) | **7.3 ms** | — | **ships** |

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
