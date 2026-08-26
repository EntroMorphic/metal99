# Contributing

Two rules are non-negotiable, and both shape everything else.

## 1. Pure ISO C99

Every source file compiles under `-std=c99 -pedantic-errors -Wall -Wextra
-Wshadow -Werror`. The build enforces it; there is no opt-out.

`-Wshadow` is load-bearing, not tidiness. A shadowed `fails` counter inside
`selftest_transport()` meant every `fails++` hit an inner variable while
`return fails` handed back an outer one that nothing ever touched — the
compiler folded the function to `movi.n a2, 0 ; retw.n`, so the self-test
could not report a failure even while printing one, and `main.c` announced
`SELF-TEST PASSED` for every build ever made.

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

**Claim a vector register in `vec.h` before using one.** The owners are the
`VEC_Q_*` macros there, and the asm pastes those macros rather than naming a
register directly — so the list cannot drift out of step with the code. All
eight are currently claimed; a new owner must share one and show the two never
interleave, because they are not saved or restored.

This used to be a hand-maintained comment reading *"q0-q3 are taken; q4-q7 are
free"*, repeated here. It stayed that way after `vec_ramp16` took q4/q5 and
`vec_xor16` took q6/q7, so both documents were actively directing contributors
onto occupied registers — the exact intermittent corruption the note existed to
prevent.

The rule governs code that runs **on the device**. Host harnesses
(`tests/host/vec_host.c`) and the self-test digest are deliberately scalar —
x86 has no PIE unit, and the digest must be obviously correct rather than fast.

## Verification

**Do not claim something works because a call returned success.** This codebase
has produced "reported success, wrong outcome" repeatedly: `rc=0` from a panel
that was wedged, a digest that passed while sampling two bytes of a 736-byte
row, a self-test that computed zero for every even input.

Before claiming a change works:

1. `make -C tests/host test` — digest assertions, no board required. These
   link `metal99/src/fold.c` directly, so they exercise the firmware's own
   instrument rather than a host copy of it.
2. `./tools/flash.sh -c 20` and read the self-test result
3. If you touched a transport, confirm the **fault injection** still reports
   `DETECTED` — a verifier that always passes is worthless
4. If the change is visual, render it: `make -C tests/host png` or `gamepng`
   links the real layers and writes a PNG you can inspect pixel by pixel.
5. Then look at the panel. The ledger checks what was *sent*, not what was
   *displayed*, and that gap is not academic: banded DMA passes the ledger and
   still looks wrong.

### One change at a time, validated before anything is built on it

The panel is the least reliable instrument available — it cannot be read back
(§2.1a), so a clean screen and a stale flash are indistinguishable, and so are
correct output and a wedged panel showing the last good frame.

Two consequences, both learned expensively:

- **Never accept a single observation as proof of a root cause.** Flip the
  suspect on and off inside ONE build, on identical content, and require the
  symptom to track it. Comparing two flashes from memory has produced a
  confident, wrong conclusion here more than once.
- **A change kept "because it is correct anyway" still has to be validated on
  its own.** A pulse-width fix was reasoned sound by inspection, shipped
  unvalidated alongside other work, and then caused a regression that took four
  rounds of bisection to find — while the search kept blaming the feature it
  had shipped beside.

An A/B harness is cheap. `apps/spanlab.c` and `apps/uilab.c` are the pattern:
static content, one variable, an on-screen indicator saying which phase is
live, and a detector region that has no legitimate reason to ever change.

## Archive, never delete

Code does not leave this project by deletion. It moves to `archive/retired/`
with a header saying what it did and the measurement that retired it.

A function that was written, tested and then displaced is a **recorded result**.
"We tried panel-side scroll and it does nothing" is worth more as working code
plus an outcome than as a sentence in a design doc - deletion throws away the
evidence and invites the next person to repeat the experiment.

### A scripted edit must assert its own bounds

Any script that removes lines states, and checks, its exact first and last line
before touching the file.

This is not hypothetical. A deletion that searched backwards for a comment
boundary removed 260 lines including `sh8601_init`, `sh8601_write_frame` and
`sh8601_write_span_x` - and **it compiled**, because what remained was still
valid C. The only symptom was one now-unused static function, which was luck.
"It builds" is not the check. Three separate scripted deletions over-cut in a
single session; every one was caught by accident rather than by design.

## Instruments belong in the repo

No ephemeral code. Probes, harnesses and diagnostics go in `tools/` or
`tests/`, never `/tmp`. The serial capture script was retyped by hand about
thirty times before this rule existed, reintroducing the same bug each time.
