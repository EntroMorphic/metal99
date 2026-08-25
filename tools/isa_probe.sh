#!/usr/bin/env bash
# Probe which Xtensa LX7 vector (EE.*) mnemonics the assembler accepts.
#
# GCC-for-Xtensa does not auto-vectorise to these and Espressif's ISA docs are
# not in the toolchain, so assembling a candidate IS the documentation. This
# produced the availability table in docs/DESIGN.md 6.8.
#
#   ./tools/isa_probe.sh                    probe the default list
#   ./tools/isa_probe.sh "ee.vadds.s16 q0, q1, q2"   probe one instruction
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Newest toolchain, not lexically-first: which mnemonics assemble depends on the
# assembler version, so probing with an older gas silently under-reports the ISA.
# Same defect build.sh had.
TC="${METAL99_TOOLCHAIN:-$(ls -d "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | sort -V | tail -1)}"
CC="$TC/xtensa-esp32s3-elf-gcc"
INC=$("$CC" -print-file-name=include)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

probe() {
  printf '#include <stdint.h>\nvoid f(void){__asm__ __volatile__("%s");}\n' "$1" > "$TMP/t.c"
  if "$CC" -std=c99 -Os -mlongcalls -ffreestanding -nostdinc -isystem "$INC" \
        -c "$TMP/t.c" -o "$TMP/t.o" 2>/dev/null; then
    printf "  OK    %s\n" "$1"
  else
    printf "  --    %s\n" "$1"
  fi
}

if [ $# -gt 0 ]; then probe "$1"; exit 0; fi

echo "load / store / broadcast"
for i in "ee.vld.128.ip q0, a2, 16" "ee.vst.128.ip q0, a2, 16" \
         "ee.vst.l.64.ip q0, a2, 8" "ee.vst.h.64.ip q0, a2, 8" \
         "ee.vldbc.16 q0, a2" "ee.vldbc.32 q0, a2" "ee.zero.q q0" \
         "ee.movi.32.q q0, a2, 0" "ee.movi.32.a q0, a2, 0"; do probe "$i"; done
echo "integer lane arithmetic"
for i in "ee.vadds.s16 q0, q1, q2" "ee.vsubs.s16 q0, q1, q2" \
         "ee.vmul.s16 q0, q1, q2" "ee.vmul.u16 q0, q1, q2" \
         "ee.vmin.s16 q0, q1, q2" "ee.vmax.s16 q0, q1, q2"; do probe "$i"; done
echo "logical / shift / reorder"
for i in "ee.andq q0, q1, q2" "ee.orq q0, q1, q2" "ee.xorq q0, q1, q2" \
         "ee.vsl.32 q0, q1" "ee.vsr.32 q0, q1" \
         "ee.vsl.16 q0, q1" "ee.vsr.16 q0, q1" \
         "ee.vzip.8 q0, q1" "ee.vunzip.8 q0, q1" "ee.vzip.16 q0, q1"; do probe "$i"; done
