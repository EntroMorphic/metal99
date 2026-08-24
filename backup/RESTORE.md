# Stock firmware backup — Waveshare ESP32-S3-Touch-AMOLED-1.8

Taken 2026-08-23 from the as-shipped device before any modification.

| | |
|---|---|
| File | `stock-full-16MB-20260823.bin` (16,777,216 bytes, full flash 0x000000–0xFFFFFF) |
| SHA256 | `3068edfbc21ec51b6ccc2cbfeb86fc5e9bc2911768ca4bebba2d8d57299021e0` |
| Chip | ESP32-S3 (QFN56) rev v0.2, 8MB octal PSRAM (AP_3v3), 16MB quad flash (Winbond ef:4018) |
| MAC | `30:ed:a0:ac:91:54` |
| Port | `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:ED:A0:AC:91:54-if00` |

## What's on it

Waveshare's ESP-Brookesia demo (source tree `ESP32-S3-Touch-AMOELD-2.06/1.8`).
App `phone_s3_box_3` v0.4.2, built 2025-08-05 against ESP-IDF v5.5-beta1.

- OS: FreeRTOS (ESP-IDF SMP fork, Xtensa port, dual-core)
- GUI: LVGL 9.x + `espressif/esp_lvgl_port` + ESP-Brookesia `phone` system
- Display: SH8601 AMOLED over QSPI — `waveshare/esp_lcd_sh8601`
- Touch: FT5x06-family over I2C — `espressif/esp_lcd_touch_ft5x06`
- Also: `espressif/avi_player`, QMI8658 IMU, ES8311 audio codec
- ESP-SR WakeNet9 model `wn9_nihaoxiaozhi_tts` in the `model` partition

## Partition table

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x009000 | 24K |
| otadata | data/ota | 0x00f000 | 8K |
| phy_init | data/phy | 0x011000 | 4K |
| model | data/spiffs | 0x012000 | 952K |
| factory | app/factory | 0x100000 | 4288K |
| ota_0 | app/ota_0 | 0x530000 | 6144K |
| storage | data/spiffs | 0xb30000 | 4884K |

## Restore

    sha256sum -c stock-full-16MB-20260823.bin.sha256   # verify first
    esptool --port /dev/ttyACM1 write-flash 0x0 stock-full-16MB-20260823.bin

NOTE: this restores flash only. `nvs` is included, so Wi-Fi credentials and
settings return to their state at backup time. eFuses are NOT part of this
image and were never modified.
