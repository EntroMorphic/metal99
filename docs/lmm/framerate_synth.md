# SYNTHESIZE — how we get out of 3 fps

Actionable. Decisions with rationale, ordered plan, success criteria, and
explicit resolution of the tensions raised in NODES.

---

## Headline

**3.2 fps is not a hardware property.** It is 94.7% per-transaction overhead in
a FIFO path we chose deliberately to isolate protocol bugs. The physical ceiling
at the clock we are *already running* is **60.6 fps**. The "~35 fps" figure in
the spec was imported from ESP-IDF measurements on a different transport and is
wrong; it is corrected below.

And full-frame fps is the wrong target anyway. The interesting number is
**~10,000-39,000 small updates per second**, at **26-102 us latency to glass**,
using an address-window mechanism we built and verified two milestones ago and
have been using only to say "the whole screen."

---

## Decisions

### D1. Reframe the panel from sink to stateful peer
The panel holds its own framebuffer — the property that produced ghost images
three times. Stop treating it as write-only. **The fastest pixel is the one we
never send.**
*Rationale:* every downstream architecture decision follows from this. Sink =>
optimise throughput (bounded at 2.6 Mbit/frame). Peer => optimise elision
(bounded only by how much we can prove is already correct).

### D2. Resolve cheap unknowns BEFORE designing anything around them
Bus clock ceiling, DDR support, panel-side scroll: each is under an hour, and
the panel displays its own corruption, so failures are visible instantly.
*Rationale:* `COLMOD` cost three milestones because we assumed instead of
checking. Twice now the message-layer design has been overtaken by a hardware
fact (T3). Cheap facts first.

### D3. GDMA before everything else
17x, high confidence, pure arithmetic.
*Rationale:* it is the single largest multiplier, and it changes flush from
synchronous to asynchronous — which determines the message layer's frame
semantics. Designing the protocol first would bake in assumptions we are about
to invalidate.

### D4. The message layer is an elision layer, not a transport layer
Transport exists, works, and is 185 lines. The layer's job is **to know what did
not change.** Its performance metric is how much it throws away.
*Rationale:* a layer pushing 1M full frames/sec at a panel accepting 60 is not
fast, it is 16,000x wasteful. On the Pi, 1M msg/sec meant "no substrate cost";
here the equivalent is eliding millions of ops into a few small writes.

### D5. Optimise latency-to-glass, not refresh rate
A 32x32 update in 102 us is a different category of object than "60 fps".
*Rationale:* interfaces do not repaint the world. Perceived responsiveness is
latency, not refresh.

### D6. Any elision scheme must carry a resync path
A dirty-region system is a **model of remote state we cannot read back.** Model
drift produces exactly the failure mode that fooled us three times: looks right,
isn't.
*Rationale:* non-negotiable. Every trap so far came from unobservable remote
state where stale looked like success. Requires: a full-repaint escape hatch, a
generation counter, and a periodic forced resync.

---

## Plan

### Phase 0 — Cheap unknowns (~1 day, resolves 3 open questions)

| Step | Method | Success criterion |
|---|---|---|
| 0a Bus clock ceiling | Binary-search `SPI_CLOCK` 40 -> 80 MHz, colour bars as the error display | Highest clock with clean bars, minus 20% margin |
| 0b DDR | Enable DDR, draw bars | Clean bars = supported; corruption = not, revert |
| 0c Panel scroll | Send `0x33` VSCRDEF / `0x37` VSCRSAR, draw bars, scroll | Bars move without a repaint = supported |

Each answered with visual evidence, recorded in DESIGN.md as fact or refuted.

### Phase 1 — GDMA
Descriptor chains replacing 5,152 FIFO transactions.
**Success: full frame <= 20 ms at 40 MHz (>= 50 fps), bus utilisation > 80%.**
Stretch: verify descriptors can replay one buffer (N7) — solid fills at near-zero
RAM and zero CPU per pixel.

### Phase 2 — PLL, 20 -> 240 MHz
**Success: CPU measured at 240 MHz by the same heartbeat method; gradient render
falls from 288 ms toward ~24 ms.**
*Mandatory:* `CPU_HZ` in `io.h` must be updated in the same commit, or every
delay silently shortens 12x — including the SH8601 120 ms sleep-out minimum.

### Phase 3 — Re-measure, then design the message layer
With flush and render both known and comparable, the protocol's frame semantics
(blocking vs fenced, frames vs regions) are determined by evidence.

### Phase 4 — Dirty-region elision
Tile-based invalidation, explicit not diffed. Determine optimal tile size
empirically (T2 crossover).
**Success: 32x32 update latency < 200 us; >= 1,000 region updates/sec on
representative UI content; forced resync verified to repair induced drift.**

---

## Tension resolutions

**T1 — dirty-rect CPU cost vs a 20 MHz CPU.**
Resolved by ordering: PLL (Phase 2) precedes elision (Phase 4). Additionally,
the scheme is **explicit tile invalidation, never per-pixel diffing** — the
renderer declares what it touched; we never compare framebuffers we do not have.

**T2 — small updates re-introduce per-transaction overhead.**
Real, and the same trap as FIFO at a different scale. Each windowed update costs
a command preamble (`0x2A`, `0x2B`, `0x32`/`0x2C` + params). There is a crossover
tile size below which overhead dominates. **Measure it in Phase 4; do not guess.**
Below the crossover, merge adjacent dirty tiles into one window.

**T3 — message-layer design keeps being overtaken by hardware facts.**
Resolved by D2 + D3: all cheap unknowns answered in Phase 0, GDMA lands in
Phase 1, design happens in Phase 3 against measurements. Twice burned, twice
deferred — deliberately.

**T4 — bus levers and CPU levers are independent, and invert.**
Today flush dominates (311 ms vs ~288 ms render for the gradient). After GDMA +
PLL: flush ~16.5 ms, render ~11.7 ms — **comparable, and render may dominate.**
The second core becomes worthwhile only at that point, not before. Re-rank after
Phase 2.

---

## Expected trajectory

| Stage | Full frame | fps | Small update (32x32) |
|---|---|---|---|
| Today (FIFO, 20 MHz) | 311 ms | 3.2 | ~4 ms |
| + GDMA | ~18 ms | ~55 | ~200 us |
| + PLL 240 MHz | ~17 ms | ~59 | ~150 us |
| + 80 MHz bus *(if panel allows)* | ~9 ms | ~110 | ~75 us |
| + DDR *(if panel allows)* | ~5 ms | ~200 | ~40 us |
| + elision, typical UI | n/a | n/a | **>10,000 updates/sec** |

The last row is the point. Everything above it is repaint rate; the last row is
what an interface actually does.

---

## Spec corrections owed

1. §4.3 and the Milestone-2 summary state a "~35 fps" ceiling. **Wrong** — that
   was ESP-IDF's DMA+PSRAM transport, not ours. Ours is **60.6 fps at 40 MHz**.
2. §4 performance table should be labelled as ESP-IDF-transport measurements, not
   metal99 targets. A number without its transport is not data.
