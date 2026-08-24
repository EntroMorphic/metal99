#!/usr/bin/env bash
# Zero-dependency build: compiler + linker + image packer. No ESP-IDF build system.
set -e
M="$(cd "$(dirname "$0")" && pwd)"
TC=$(ls -d /home/ztflynn/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1)
CC="$TC/xtensa-esp32s3-elf-gcc"
OBJCOPY="$TC/xtensa-esp32s3-elf-objcopy"
SIZE="$TC/xtensa-esp32s3-elf-size"

GCCINC=$($CC -print-file-name=include)
CFLAGS="-std=c99 -pedantic-errors -Wall -Wextra -Werror -Os -mlongcalls
        -ffreestanding -mtext-section-literals -ffunction-sections -fdata-sections -fno-builtin
        -nostdinc -isystem $GCCINC -I$M/src"
LDFLAGS="-nostdlib -Wl,--gc-sections -Wl,-T,$M/link.ld -Wl,-Map,$M/build/fw.map"

mkdir -p "$M/build"
$CC $CFLAGS $LDFLAGS "$M"/src/*.c -o "$M/build/fw.elf" -lgcc
$SIZE "$M/build/fw.elf"
esptool --chip esp32s3 elf2image --flash-mode dio --flash-freq 80m \
        --flash-size 16MB --output "$M/build/fw.bin" "$M/build/fw.elf" >/dev/null
ls -l "$M/build/fw.bin" | awk '{print "image:", $5, "bytes"}'
