# flynn-esp32-s3-amoled

Custom graphics on a **Waveshare ESP32-S3-Touch-AMOLED-1.8**, working down to
bare metal. Project requirement: **pure ISO C99**.

## The board

ESP32-S3R8 rev v0.2 · dual Xtensa LX7 @240MHz + ULP · 8MB octal PSRAM @80MHz ·
16MB quad flash · 368x448 QSPI AMOLED · AXP2101 PMU · PCF85063 RTC ·
QMI8658 IMU · ES8311 codec. Native USB-JTAG only, no UART bridge.

> **Board revision matters.** This unit is the ORIGINAL revision: **SH8601**
> display + **FT3168** touch. Waveshare's BSP switched to CO5300/CST816 at
> **2.0.3**, and every ESP-IDF example pins `^2.0.3`. Those build cleanly and
> drive the wrong panel - a dark or garbled screen with no error. **Pin the BSP
> to `==2.0.0`.** Verify: `managed_components/` must contain
> `waveshare__esp_lcd_sh8601` and NOT `espressif__esp_lcd_co5300`.

## Layout

| Path | What |
|---|---|
| `backup/` | Verified 16MB stock flash image + restore instructions |
| `lvgl_demo_sh8601/` | Waveshare LVGL demo, BSP pinned to 2.0.0. Known-good reference |
| `bare_metal_fb/` | Own framebuffer + rasterizers. Pure C99, no LVGL, no FreeRTOS |
| `metal99/` | **Zero dependencies.** No ESP-IDF at all; ROM loads it from flash 0x0 |
| `waveshare-demo/` | Upstream clone (gitignored, see .gitignore for the clone command) |

## Toolchain

ESP-IDF v5.5.5 at `~/esp/v5.5`. Its venv is system Python 3.10 but the default
`python3` is conda 3.13, so **always**:

```sh
export PATH=/usr/bin:/bin:$PATH
. ~/esp/v5.5/export.sh
```

`metal99/` needs none of that - `./metal99/build.sh` calls gcc/ld/esptool directly.

## Measured performance (368x448 RGB565, -O2)

The SPI master **cannot DMA from PSRAM**; it bounces via internal RAM and a full
329KB frame fails. Flush in horizontal **bands** through your own internal DMA
staging buffer.

| Renderer | Render | Flush | FPS |
|---|---|---|---|
| Solid fill | 6.18 ms | 22.49 ms | 34.8 |
| Integer LUT plasma | 11.57 ms | 22.49 ms | 29.3 |
| Per-pixel `sqrtf` x2 | 135.6 ms | 22.49 ms | 6.3 |

Flush dominates: ~22.5ms of DMA leaves roughly a 7ms render budget for 30fps.
Float in the inner loop is fatal - use LUTs and fixed point. Optimal band is 64
rows. Dropping FreeRTOS cut flush from 27.85ms to 22.49ms (the semaphore
round-trip per band was real cost).

## Pure C99

ESP-IDF headers **cannot** be compiled as C99: `ESP_STATIC_ASSERT` expands to
C11 `_Static_assert`, and the Xtensa headers use bare `asm`, which
`__STRICT_ANSI__` disables. Linking only needs matching signatures, not headers,
so `bare_metal_fb/main/platform.h` hand-declares the external ABI.

ISO-safe substitutions: `__asm__` not `asm`; `typedef char x[cond?1:-1]` for
static assertions; no `M_PI` (POSIX); no `__builtin_bswap16`.
`__attribute__` *is* accepted under `-pedantic-errors`.

## Restore the stock firmware

```sh
cd backup && sha256sum -c stock-full-16MB-20260823.bin.sha256
esptool --port /dev/ttyACM1 write-flash 0x0 stock-full-16MB-20260823.bin
```
