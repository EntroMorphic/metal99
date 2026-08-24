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

```
+-------------------------------------------------------------+
|  application  (pure C99, portable, host-testable)           |
|    render_c99.c : rasterizers, LUTs, palette                |
+-------------------------------------------------------------+
|  graphics messaging layer  (NeoGPU port, §7)                |
|    opcode stream -> channels -> frame begin/end/present     |
|    HSBackendOps vtable                                       |
+-------------------------------------------------------------+
|  hs_backend_sh8601.c   <-- Milestone 2 delivers this        |
+-------------------------------------------------------------+
|  metal99 platform  (§6)                                      |
|    start.c  wdt.c  io.c  spi2.c  gdma.c                     |
+-------------------------------------------------------------+
|  ESP32-S3 silicon                                            |
+-------------------------------------------------------------+
```

Layers above `hs_backend_sh8601.c` contain **no register access**. Layers below
contain **no rendering logic**. That boundary is what keeps `render_c99.c`
compiling and running unchanged on a Linux host.

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

Note `SOC_SPI_SUPPORT_DDRCLK = 1`: this silicon does double-data-rate SPI.
Whether the SH8601 accepts it is unknown and cheap to test.

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
| Band staging buffer | 46 K | 64 rows x 368 x 2 B |
| Messaging layer (embedded profile, §7.4) | 34 K | vs 1920 K at desktop defaults |
| Stack | 8 K | |
| **Subtotal** | **~104 K** | Comfortable inside 320 K |

Headroom allows a larger band (128 rows = 92 K) or a full 368x448 framebuffer
in SRAM (322 K) if a persistent surface is ever needed — though that would
crowd out everything else and is not planned.

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

#### Measured, and what it costs

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

2,240-byte image. Pure ISO C99 under `-pedantic-errors -Wall -Wextra -Werror`.
No ESP-IDF, no FreeRTOS, no libc, no ROM calls.

**What remains is performance, and both levers are measured and independent:**

| Lever | Current | Expected |
|---|---|---|
| GDMA instead of 64-byte FIFO | 6 % bus utilisation, 311 ms/frame | ~17x - the FIFO path is overhead-bound, not bus-bound |
| PLL instead of ROM default | CPU 20 MHz | 12x on per-pixel work; flush is unaffected, being bus-bound |

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

## 6.8 PROJECT RULE: no scalar per-element math

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

### Measured

Row fill of 368 px: **364 cycles = 0.99 cycles/pixel** (scalar was ~3-4).
Verified on hardware: fill, zero and copy all produce correct data, and the
panel renders from vectorised fills.

### Where scalar remains, and why

| Site | Status |
|---|---|
| Row fills | vectorised |
| `.bss` zeroing | scalar word loop - to convert |
| **`spi2_xfer` W-packing** | **scalar per-byte, 5,152x/frame** |
| `sh8601_rgb565` | single value, not bulk |
| `con_dec` | console formatting |
| Loop counters, addresses, branches | inherent control flow |

The W-packing loop is the hottest scalar code we have, but its destination is
**MMIO peripheral registers** (`SPI_W0..W15`), and 128-bit stores to peripheral
address space cannot be assumed to work. **GDMA deletes the loop entirely**,
which is the correct fix rather than vectorising MMIO.

## 7. Graphics messaging layer

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
