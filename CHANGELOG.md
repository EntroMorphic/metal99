# Changelog

Reverse chronological. Dates are the working session; this project was built in
one continuous run.

## Unreleased

### Added
- `gfx` retained-mode messaging layer. Keeps a 448-descriptor model of what each
  row should look like and **derives** dirtiness by diffing it, rather than
  trusting a caller to declare what it touched. Redundant repaints transmit
  nothing.
- On-device self-test with a **transmit ledger**: full-coverage rolling-hash
  digest of every byte handed to the hardware, compared against an
  independently computed reference — and validated against injected faults
  (bit flip, duplicated row, one-row shift), on-device and on the host.
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
- **The self-test could not report a failure.** An inner `fails` counter in
  `selftest_transport()` shadowed the outer one, so every `fails++` incremented
  a variable that went out of scope and `return fails` handed back an untouched
  zero — the compiler folded the whole function to `movi.n a2, 0 ; retw.n`.
  `main.c` therefore printed `SELF-TEST PASSED` unconditionally for the entire
  life of the project, including when the fault injection printed
  `INSTRUMENT IS BLIND`. `build.sh` now compiles with `-Wshadow`.
- **The vector-register contract pointed contributors at occupied registers.**
  `vec.h` and `CONTRIBUTING.md` both said *"q4-q7 UNUSED — take these for new
  code"* long after `vec_ramp16` took q4/q5 and `vec_xor16` took q6/q7. Owners
  are now `VEC_Q_*` macros pasted into the asm, so the list cannot drift from
  the code.
- **The content digest was blind to displacement.** `acc += w[i] ^ (pos * K)`
  is additively separable, so a sparse payload could move without changing the
  digest — a single `0x01` at offset 0 and at offset 32 both fold to
  `0x2A010AF9`. Replaced with a polynomial rolling hash (`acc = acc*M + w`),
  which also catches reordering (previously a documented blind spot) and drops
  the global position counter that had caused two earlier bugs. Found by the
  new host tests, not on hardware.
- **The digest silently discarded trailing bytes.** `vec_fold()` took a vector
  count and folded `len / 16`, dropping up to 15 bytes of any transfer whose
  length was not a multiple of 16. It now takes a byte count.
- **The digest ran on the hot path.** `spi2.h` claimed the ledger was
  *"O(1) per transfer, not per byte"*; it folded every pixel byte of every
  frame in steady state — roughly 12% of a 104-row update, for an instrument
  nothing read outside the self-test. Byte counting stays always-on; digesting
  is armed via `spi2_ledger_digest_enable()`.
- **The self-test byte check could not detect over-transmission.** It compared
  `got >= want` against the *total* byte count, under a comment promising to
  subtract the command preamble — which the code never did. The ledger now
  counts pixel payload separately, so the comparison is an equality.
- **The DMA ledger assumed a contiguous descriptor chain.** `spi2_dma_finish()`
  folded the whole transfer length from the *first* descriptor's buffer. Correct
  only by accident of how `band_chain()` carves one array; any scatter-gather
  chain would have digested memory that was never sent and reported PASS. It now
  walks the chain, and cross-checks the descriptors' lengths against `MS_DLEN`.
- **`gfx_stats.rows_changed` was declared, documented and never assigned** — it
  read 0 forever.
- **`gfx_split()` stored a precision the renderer does not have.** `x` was kept
  at 1-pixel resolution while `gfx_rowfn` fills in 8-pixel vectors, so x=100 and
  x=103 compared as different and retransmitted bytes identical to those already
  on the glass — the model-drift bug this layer exists to prevent, one level up.
  `x` is now quantised on the way in.
- **`build.sh` silently used the oldest toolchain on the machine.** `ls | head -1`
  sorts lexically and picked esp-13.2.0 out of three installed. Now newest by
  version sort, printed, and overridable with `METAL99_TOOLCHAIN`.
- **`capture.py` hardcoded one board's serial number** as its default port, so
  the README quickstart worked on exactly one machine — and `flash.sh` had
  drifted to a different default (`/dev/ttyACM1`) for the same device. Both now
  auto-detect by USB vendor ID through one shared code path.
- Unreachable one-DMA-per-row branch in `sh8601.c`'s `stream()`, along with the
  descriptor and debug accessor only it wrote. It looked live.
- Frame counter in the pacing loop was a signed `int` incremented forever;
  undefined behaviour after ~414 days at 60 Hz. Now `uint32_t`.
- `elide_flush()` left `stats.cycles` at the previous frame's value when a span
  failed, so a failing frame looked healthy in telemetry.
- Zero-length guards on all `vec_*` primitives — the loops are
  store-then-decrement, so a count of zero would have wrapped to 4 billion.
- `"memory"` clobbers on the `vec_fill16`/`vec_ramp16` seeding asm, which read
  a local through a pointer with nothing ordering the store before the read.
- Stale comments corrected: `BAR_H` described as 24 rows while defined as 96,
  `io.h` naming a `clk_set_cpu()` that does not exist, `gdma_restart()` pointing
  at a caller that deliberately does the opposite, `sh8601_stats` documented as
  per-frame when elision made it per-span, DESIGN.md §5's band description and
  its duplicate §6.8 heading.

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

- **README's performance table described a transport that does not ship.**
  `0.037 ms/row` and "All 448 rows are updatable at 60 Hz" are banded-DMA
  figures; banded DMA is parked. The table also contradicted itself —
  `0.037 x 104 = 3.9 ms` against its own measured 7.1 ms for a 104-row update.
  Measured directly on the FIFO transport that ships: a full 448-row repaint is
  **31.2 ms / 31.9 fps**, and the 60 Hz boundary is **~240 rows**, not 448.
  README and DESIGN.md §6.6h now separate the two transports; §6.6h carries a
  dated correction rather than being rewritten.
- **`gfx_stats.rows_sent` was documented as `>= rows_changed`.** The first run
  that actually populated `rows_changed` disproved it: moving a 96-row bar by
  4 px gives changed=192 (erase 96 + draw 96) and sent=100 (their union). The
  two count different things and neither bounds the other.
- `main.c` printed `SELF-TEST PASSED` on top of `selftest.c`'s identical line,
  which is part of how the constant-zero return went unnoticed. It now prints
  the return value, so the two disagreeing would be visible.

### Added — text
- **Labels are described, not rasterised.** A glyph scanline is up to five runs,
  so a twenty-character line would be a hundred runs in a row that holds eight —
  text cannot live in the run model. A label is instead a *description*
  (position, colour, font, string) diffed exactly like a run list, and
  double-buffered against what the panel holds for the same reason rows are.
  The string is **copied**, not pointed at: a caller formatting into a reused
  buffer would otherwise change content without changing the pointer.
- **The blit is transparent**, so text composes over any background at no extra
  cost — the mask that selects the foreground is the same mask that keeps the
  destination. `vec_glyph_row` measured at 1.375 instr/px.
- **Fonts are rasterised from TrueType at build time** by `tools/mkfont.py`. The
  device cannot rasterise outlines: no malloc, no libc, no floating point, and
  scan conversion is irreducibly scalar work that would break §3.0's wire-bound
  premise. Share Tech Mono, SIL OFL 1.1, TTF sources tracked so the generated
  table is reproducible.

  | measured on hardware | px/frame |
  |---|---|
  | first paint, three labels | 11,200 |
  | **setting identical text again** | **1,472 — resync only; the text is free** |
  | updating a 5-digit 16x32 counter | 3,989 |
  | a full screen, for scale | 164,864 |

  **A static label marks nothing**, so a title on screen throughout leaves the
  20 s resync-off window and its wrap trace completely unaffected — still 8 rows
  and 2 spans per frame, still exactly 192 rows at every wrap.
- 12 more host assertions covering glyph bits against the font data itself,
  transparency, elision of identical text, move, clear, grid snap and edge
  clipping. 57 across the suite.

### Added — a drawing surface
- **Rows are run lists, and spans are rectangles.** A row was previously one of
  two things: a solid colour, or two colours with one transition — which could
  not express a rectangle anywhere but against a screen edge. `gfx_rect()` now
  composes arbitrary rectangles, `elide` carries an x-extent per dirty row, and
  `sh8601` sets the address window to it. The `0x2A`/`0x2B` window always took
  `x0`/`x1`; the driver had simply never used them.
- **Dirtiness is derived at present time, against what the panel actually
  received.** The first version diffed at set time against the model in flight,
  so erase-then-draw marked the overlapping rows `FG→BG→FG` — net unchanged,
  transmitted anyway. A second 448-row model (`g_sent`) makes intermediate
  states free.

  Measured on hardware, one step of continuous motion:

  | | px/frame |
  |---|---|
  | full-width bar, before | 37,750 |
  | 88x88 box, sub-width only | 9,236 |
  | 88x88 box, + present-time diff | **1,968** |

  **19x** end to end, and the run model is what made the box expressible at all.
- Run overflow past `GFX_MAX_RUNS` merges the narrowest adjacent pair and is
  counted in `gfx_stats.run_overflows` — lossy, bounded, never silent.
- 24 host assertions on the run model, linking the real `gfx.c` and stubbing
  only the layers beneath it.

- **The long-run verification now drives `gfx`, not raw `elide`.** The 20 s
  resync-off window with every wrap traced is the strongest check in the
  project, and it called `elide_mark()` and `scene()` directly — testing elide's
  marking and nothing above it. `g_sent`, the model of what the panel is
  believed to hold, was the one layer it never touched, and a drift there
  produces exactly the symptom the window exists to catch.

  | pacing loop | rows | px | ms | headroom |
  |---|---|---|---|---|
  | stationary bar, resync on | 4 | 1,472 | 0.3 | 48.9x |
  | moving bar, resync OFF | 8 | 2,944 | 0.6 | 26.2x |
  | *previously, raw elide* | *100* | *36,800* | *7.0* | *2.3x* |

  A moving element costs its leading and trailing edges, not its area. Every
  wrap still marks exactly 192 rows in 2 spans through the new path.
- **The sub-width transport had no ledger coverage.** Every self-test case
  called the full-width wrapper, so the narrow-window path was verified only by
  looking at the panel. Three cases now — a centred element, the first column
  cell, the last column cell — all exact on hardware.

### Added (remediation)
- `tests/host/digest_test.c` — 14 assertions on the transmit-ledger digest,
  runnable without a board, linking `metal99/src/fold.c` so the firmware's own
  instrument is what gets tested. Covers fault detection, grouping independence
  across FIFO/band/row slicing, byte-tail coverage, displacement and reordering.
- `metal99/src/fold.c` — digest split out of `vec.c` so the host can compile it.
- One-shot timed full repaint at boot, on the shipping transport, so the
  full-frame cost is measured rather than extrapolated from the parked one.
- Static assertion that the probe ramp cannot reach int16 saturation, plus a
  runtime check in the host tests. `vec_ramp16` accumulates with the saturating
  `ee.vadds.s16`, so the old `0x0111` step pinned lanes 103-367 to `0x7FFF` —
  the pattern documented as "positionally unique in BOTH axes" was flat across
  72% of every row. Same trap `fold.c` records as digest version 3.
- Render/flush cycle split accumulated per frame in `elide_stats` and reported.
  `sh8601`'s per-span figures were computed on the hot path every span and read
  by nobody: `sh8601_last_frame()` was dropped by `--gc-sections`.

### Changed
- ESP-IDF stages moved to `archive/` — superseded, not deleted, and still the
  reference for the panel's init sequence.
- `main.c` split; self-test extracted to `selftest.c`.

### Known issues
- **Banded GDMA is disabled.** The self-test shows it delivers byte-identical
  data to the FIFO path, yet the display is visibly worse — so the fault is in
  delivery timing, not content. Parked; FIFO ships with 2.3x headroom. The
  ledger cannot see this by construction: it verifies *what* was handed to the
  peripheral, never *when* it reaches the wire.
- **Banded DMA's periodic corruption is still unexplained.** It now provably
  ships identical bytes — 448 rows digest to `0xF5642645` through both
  transports on device — which narrows the fault to delivery timing but does
  not locate it.
- **The rolling resync is now the dominant cost of a small update** — 4
  full-width rows per frame is 1,472 px, against 1,968 px for a moving 88x88
  element. The safety net outweighs the work. `ELIDE_RESYNC_FRAMES` trades that
  against how fast model drift is corrected; left alone deliberately.
- **Full-frame 60 Hz is not achieved by what ships.** FIFO measures 31.2 ms for
  448 rows. Interface-sized updates hit 60 Hz with 2.2x headroom; a literal full
  repaint needs the parked transport.
- The first GDMA arm after init is swallowed. Characterised and worked around
  (arm twice, trigger once), not explained.
