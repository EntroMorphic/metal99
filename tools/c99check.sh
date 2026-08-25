#!/usr/bin/env bash
# Second-opinion C99 conformance check.
#
# build.sh already compiles with -std=c99 -pedantic-errors -Werror, but that is
# ONE front end's opinion, and gcc-for-xtensa is the same family that defines
# the extensions we lean on. Clang is an independent implementation with its own
# reading of the standard; a construct both accept under -pedantic-errors is far
# more likely to be portable C99 than one only gcc accepts.
#
# Also enumerates the extension surface, so the claim in vec.h ("exactly two
# constructs") is verified rather than asserted. If someone adds a statement
# expression or a typeof, this prints it and fails.
set -u
cd "$(dirname "$0")/.."
M=metal99
CC=${CC:-clang}
command -v "$CC" >/dev/null || { echo "c99check: no $CC, skipping"; exit 0; }

fails=0
echo "c99check: $($CC --version | head -1)"
for f in $M/src/*.c $M/apps/*.c; do
    if ! out=$("$CC" -std=c99 -pedantic-errors -Wall -Wextra -Wshadow -Werror \
                     -ffreestanding -I$M/src -I$M/apps -fsyntax-only "$f" 2>&1); then
        echo "  FAIL $f"; echo "$out" | sed 's/^/       /'; fails=$((fails+1))
    fi
done

# Extension surface. __asm__ and __attribute__ are the two we accept and have
# documented; anything else is new and must be justified, not absorbed.
unexpected=$(grep -rnE '\b(typeof|__typeof__|__builtin_[a-z_]+)\b|\(\{' \
             $M/src $M/apps --include=*.c --include=*.h 2>/dev/null \
             | grep -vE '^\s*[^:]+:[0-9]+:\s*\*' || true)
if [ -n "$unexpected" ]; then
    echo "  FAIL undocumented GNU extension:"; echo "$unexpected" | sed 's/^/       /'
    fails=$((fails+1))
fi

# Count CODE occurrences only. Counting comment text too made this number climb
# every time the extensions were discussed in prose, which is a metric that
# measures documentation rather than exposure.
code() { grep -rn "$1" $M/src $M/apps --include=*.c --include=*.h 2>/dev/null \
         | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' | wc -l; }
a=$(code '__asm__'); b=$(code '__attribute__')
echo "  extension surface: __asm__ x$a, __attribute__ x$b, nothing else"

[ "$fails" -eq 0 ] && echo "c99check: PASS (all sources, strict ISO C99, second front end)"
exit "$fails"
