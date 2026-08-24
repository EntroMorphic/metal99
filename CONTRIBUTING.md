# Contributing

Two rules are non-negotiable, and both shape everything else.

## 1. Pure ISO C99

Every source file compiles under `-std=c99 -pedantic-errors -Wall -Wextra
-Werror`. The build enforces it; there is no opt-out.

Practical consequences:

- `__asm__`, never bare `asm` — `__STRICT_ANSI__` disables the latter
- `typedef char x[cond ? 1 : -1]` for static assertions; `_Static_assert` is C11
- No `M_PI` (POSIX), no `__builtin_bswap16`
- `__attribute__` **is** accepted under `-pedantic-errors`

ESP-IDF headers cannot be included anywhere: they use `_Static_assert` and bare
`asm`. Linking needs matching signatures, not headers.

## 2. No scalar per-element math

Bulk data work goes through the LX7 128-bit vector unit (`vec.h`). GCC does not
auto-vectorise to `EE.*`, so these are inline `__asm__`.

**Claim a vector register in `vec.h` before using one.** q0-q3 are taken; q4-q7
are free. A collision shows up as intermittent visual corruption.

The rule governs code that runs **on the device**. Host harnesses
(`tests/host/vec_host.c`) and the self-test digest are deliberately scalar —
x86 has no PIE unit, and the digest must be obviously correct rather than fast.

## Verification

**Do not claim something works because a call returned success.** This codebase
has produced "reported success, wrong outcome" repeatedly: `rc=0` from a panel
that was wedged, a digest that passed while sampling two bytes of a 736-byte
row, a self-test that computed zero for every even input.

Before claiming a change works:

1. `./tools/flash.sh -c 20` and read the self-test result
2. If you touched a transport, confirm the **fault injection** still reports
   `DETECTED` — a verifier that always passes is worthless
3. If the change is visual, look at the panel; the ledger checks what was
   *sent*, not what was *displayed*

## Instruments belong in the repo

No ephemeral code. Probes, harnesses and diagnostics go in `tools/` or
`tests/`, never `/tmp`. The serial capture script was retyped by hand about
thirty times before this rule existed, reintroducing the same bug each time.
