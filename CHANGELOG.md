# Changelog

Reverse chronological. Dates are the working session; this project was built in
one continuous run.

## Unreleased

### Added
- `gfx` retained-mode messaging layer. Keeps a 448-descriptor model of what each
  row should look like and **derives** dirtiness by diffing it, rather than
  trusting a caller to declare what it touched. Redundant repaints transmit
  nothing.
- On-device self-test with a **transmit ledger**: full-coverage, position-
  weighted digest of every byte handed to the hardware, compared against an
  independently computed reference — and validated against injected faults
  (bit flip, duplicated row, one-row shift).
- Panel liveness check at boot, so a dark screen is no longer ambiguous between
  a wedged panel and code correctly drawing black.
- Elision layer: dirty-row tracking, span coalescing, **rolling** resync.
- 60 Hz frame pacing with an explicit late-frame counter.
- `gdma.c`: descriptor chains, banding, render/DMA overlap (currently parked).
- `clk.c`: PLL switch to 160 MHz, verified by host-timed markers.
- 128-bit vector primitives (`vec.c`) and the project's no-scalar rule.
- Research tooling: `capture.py`, `flash.sh`, `reg.sh`, `isa_probe.sh`,
  host render harness.

### Fixed
- **CS left asserted on nine error paths.** `CS_KEEP_ACTIVE` holds the line until
  a transaction runs without it, so an early return left the panel selected and
  the next command word was swallowed as pixel data. A plausible root cause for
  the repeated panel wedges.
- `FWRITE_QUAD` was written to `SPI_CTRL` instead of `SPI_USER`. Failed
  **silently** — transfers completed, on one line.
- Missing `COLMOD` (`0x3A`) in panel init. The BSP's nine commands are only the
  *vendor* portion; the driver also sends `0x01` reset, `0x36` and `0x3A`.
- Unbounded spin loops could hang the device with no diagnostic.
- Console `usj_putc` spun up to 125 ms **per character** when the host was not
  reading, stalling the render loop. Misdiagnosed three times as a graphics
  fault.
- Unchecked PLL switch would have left every delay 8x short, including the
  panel's 120 ms sleep-out minimum.
- Unbounded frame-pacing drift that silently disabled pacing after ~13 s of
  accumulated lateness.

### Changed
- ESP-IDF stages moved to `archive/` — superseded, not deleted, and still the
  reference for the panel's init sequence.
- `main.c` split; self-test extracted to `selftest.c`.

### Known issues
- **Banded GDMA is disabled.** The self-test shows it delivers byte-identical
  data to the FIFO path, yet the display is visibly worse — so the fault is in
  delivery timing, not content. Parked; FIFO ships with 2.3x headroom.
- The first GDMA arm after init is swallowed. Characterised and worked around
  (arm twice, trigger once), not explained.
