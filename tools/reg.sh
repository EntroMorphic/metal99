#!/usr/bin/env bash
# Look up ESP32-S3 registers and bitfields from the ESP-IDF SoC headers.
#
# Written because this was hand-grepped about fifteen times during bring-up,
# with inconsistent patterns each time - and GDMA is about to need far more.
# The headers ARE the machine-readable TRM; this just stops re-deriving the
# incantation.
#
#   ./tools/reg.sh GDMA                 addresses matching GDMA
#   ./tools/reg.sh -f FWRITE_QUAD       a bitfield: which register, shift, mask
#   ./tools/reg.sh -b SPI2              a peripheral base address
#   ./tools/reg.sh -r SPI_USER          one register's full field list
set -uo pipefail
IDF="${IDF_PATH:-$HOME/esp/v5.5}"
SOC="$IDF/components/soc/esp32s3"
REG="$SOC/register/soc"
[ -d "$REG" ] || { echo "SoC headers not found under $SOC" >&2; exit 1; }

mode=addr
while getopts "fbr" o; do case $o in f) mode=field;; b) mode=base;; r) mode=reg;; esac; done
shift $((OPTIND-1))
q="${1:-}"; [ -n "$q" ] || { sed -n '3,14p' "$0" | sed 's/^# \?//'; exit 2; }

case $mode in
  base)
    grep -hE "^#define DR_REG_.*${q}.*_BASE" "$REG"/reg_base.h "$SOC"/include/soc/reg_base.h 2>/dev/null | sort -u
    ;;
  field)
    # which register block does the field live in, plus shift and mask
    for f in "$REG"/*.h; do
      awk -v q="$q" -v file="$f" '
        /^#define [A-Z0-9_]+_REG(\([a-z]\))? +/ { reg=$2; sub(/\(.*/,"",reg) }
        $0 ~ ("^#define " q "_S +")  { print "  " q "  in " reg "  shift=" $3 "  (" file ")" }
        $0 ~ ("^#define " q "_V +")  { print "  " q "  mask=" $3 }
      ' "$f"
    done | sort -u
    ;;
  reg)
    for f in "$REG"/*.h; do
      awk -v q="$q" '
        $0 ~ ("^#define " q "_REG") { inreg=1; print "  " $0; next }
        /^#define [A-Z0-9_]+_REG(\([a-z]\))? +/ && inreg { inreg=0 }
        inreg && /bitpos/ { print "   " $0 }
      ' "$f"
    done | head -60
    ;;
  *)
    grep -hE "^#define .*${q}.*(_REG|_BASE)" "$REG"/*.h "$SOC"/include/soc/*.h 2>/dev/null | sort -u | head -40
    ;;
esac

exit 0
