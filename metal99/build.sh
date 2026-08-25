#!/usr/bin/env bash
# Zero-dependency build: compiler + linker + image packer. No ESP-IDF build system.
set -e
M="$(cd "$(dirname "$0")" && pwd)"

# Toolchain selection is DETERMINISTIC and REPORTED.
#
# This used to be `ls -d ... | head -1`, which sorts lexically and therefore
# picked esp-13.2.0 out of three installed toolchains - silently building with
# the oldest compiler on the machine while the project claimed a reproducible
# zero-dependency build. Newest by version sort, overridable, and printed so
# the log records which compiler produced the image.
TC="${METAL99_TOOLCHAIN:-$(ls -d "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin 2>/dev/null | sort -V | tail -1)}"
[ -n "$TC" ] && [ -x "$TC/xtensa-esp32s3-elf-gcc" ] || {
  echo "no xtensa-esp-elf toolchain found under ~/.espressif/tools/" >&2
  echo "set METAL99_TOOLCHAIN=/path/to/xtensa-esp-elf/bin to override" >&2
  exit 1
}
CC="$TC/xtensa-esp32s3-elf-gcc"
OBJCOPY="$TC/xtensa-esp32s3-elf-objcopy"
SIZE="$TC/xtensa-esp32s3-elf-size"
echo "toolchain: $($CC -dumpversion)  ($TC)"

GCCINC=$($CC -print-file-name=include)
# -Wshadow is load-bearing, not tidiness. A shadowed `fails` in selftest.c made
# selftest_transport() return 0 unconditionally - a self-test that could not
# report a failure, which is the exact defect the self-test exists to prevent.
CFLAGS="-std=c99 -pedantic-errors -Wall -Wextra -Wshadow -Werror -Os -mlongcalls
        -ffreestanding -mtext-section-literals -ffunction-sections -fdata-sections -fno-builtin
        -nostdinc -isystem $GCCINC -I$M/src"
LDFLAGS="-nostdlib -Wl,--gc-sections -Wl,-T,$M/link.ld -Wl,-Map,$M/build/fw.map"

# ONE app is linked, chosen here. Every app defines `const app_t APP`, so they
# cannot all be compiled at once - and that is the point of the boundary: an
# application is a file you swap, not a branch inside main.c.
APP="${APP:-gridvoid}"
[ -f "$M/apps/$APP.c" ] || { echo "no such app: $M/apps/$APP.c" >&2
                             echo "available: $(ls "$M"/apps/*.c | xargs -n1 basename | sed 's/\.c$//' | tr '\n' ' ')" >&2
                             exit 1; }
echo "app: $APP"

mkdir -p "$M/build"
$CC $CFLAGS -I"$M/apps" $LDFLAGS "$M"/src/*.c "$M/apps/$APP.c" -o "$M/build/fw.elf" -lgcc
$SIZE "$M/build/fw.elf"
esptool --chip esp32s3 elf2image --flash-mode dio --flash-freq 80m \
        --flash-size 16MB --output "$M/build/fw.bin" "$M/build/fw.elf" >/dev/null
ls -l "$M/build/fw.bin" | awk '{print "image:", $5, "bytes"}'
