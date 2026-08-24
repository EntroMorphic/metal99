#!/usr/bin/env bash
# Build metal99 and flash it to offset 0x0. Logs land in logs/, never /tmp.
#
#   ./tools/flash.sh            build + flash
#   ./tools/flash.sh -b         build only
#   ./tools/flash.sh -c 20      build, flash, then capture 20s with reset
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PORT:-/dev/ttyACM1}"
LOG="$ROOT/logs/build-$(date +%H%M%S).log"
BUILD_ONLY=0; CAPTURE=0

while getopts "bc:" o; do
  case $o in
    b) BUILD_ONLY=1 ;;
    c) CAPTURE=$OPTARG ;;
    *) exit 2 ;;
  esac
done

mkdir -p "$ROOT/logs"
if ! "$ROOT/metal99/build.sh" > "$LOG" 2>&1; then
  echo "BUILD FAILED (see $LOG):"
  grep -E "error|Error" "$LOG" | head -10
  exit 1
fi
tail -2 "$LOG"
[ "$BUILD_ONLY" = 1 ] && exit 0

esptool --port "$PORT" write-flash 0x0 "$ROOT/metal99/build/fw.bin" 2>&1 \
  | grep -aE "Hash of data verified|Wrote" || { echo "FLASH FAILED"; exit 1; }

if [ "$CAPTURE" != 0 ]; then
  "$ROOT/tools/capture.py" -r -s "$CAPTURE"
fi
