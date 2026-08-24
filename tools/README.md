# tools / tests

**Project rule: no ephemeral code.** This is research - every probe, harness and
diagnostic is tracked. Nothing lives in `/tmp`, which the OS evicts and which
loses the reasoning behind a result the moment the session ends.

Reconstructed after the fact: the ISA probe, serial capture, host render harness
and NeoGPU size measurement all originally ran as throwaway shell heredocs. Their
*results* were written into the docs but the *instruments* were discarded, so
none of it was reproducible. The capture script alone had been retyped by hand
about thirty times.

| Tool | Purpose | Produced |
|---|---|---|
| `capture.py` | Read the console over USB-Serial-JTAG | every hardware measurement in the docs |
| `flash.sh` | Build, flash to `0x0`, optionally capture | the build/flash loop |
| `isa_probe.sh` | Which `EE.*` vector mnemonics assemble | the ISA table in DESIGN.md 6.8 |
| `neogpu_sizes.c` | NeoGPU struct sizes + capacity cost | the memory budget in DESIGN.md 7.4 |
| `reg.sh` | Look up S3 registers/bitfields from the SoC headers | hand-grepped ~15x during bring-up |
| `tests/host/` | Run metal99 renderers on the desktop -> PPM | ~200ms iteration vs a flash cycle |

## Gotchas worth keeping

**`capture.py` flushes BEFORE reset, never after.** Toggling DTR/RTS resets the
chip; flushing afterwards discards the boot banner. That cost several confusing
captures during bring-up.

**`tools/compat/arm_neon.h` is a parse-only stub.** `hs_core.h` includes
`<arm_neon.h>` directly instead of the project's own `hs_neonCompat.h`, so the
headers will not parse off-ARM without it. It defines typedefs for `sizeof()`
and implements nothing.

**`tests/host/vec_host.c` is deliberately scalar.** The no-scalar rule governs
code that runs ON THE DEVICE. x86 has no Xtensa PIE unit; this validates
rendering *logic*, not instruction selection.

**Logs go to `logs/`, not `/tmp`.** In-repo so the OS cannot evict them
mid-session, gitignored because build noise is not a finding.
