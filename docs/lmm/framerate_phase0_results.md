# Phase 0 results — 2026-08-24

Three cheap experiments, run to avoid designing around guesses.
**All three returned negative or blocked.** That is a result, not a failure:
the path forward is now unambiguous.

---

## 0a — Bus clock ceiling: **BLOCKED (not refuted)**

Cannot be run yet. Our SPI source is **XTAL at 40 MHz**. Exceeding 40 MHz
requires APB, which is PLL-derived, and the PLL is off (CPU measured at
20 MHz = XTAL/2).

**The synthesis had this backwards.** It listed 0a as a cheap experiment
preceding the expensive work; in fact it *depends* on Phase 2. Sequencing
assumptions deserve the same scrutiny as technical ones.

Deferred until the PLL is enabled, and folded into that milestone.

## 0b — DDR (double data rate): **REFUTED**

Resolved without touching hardware.

`SOC_SPI_SUPPORT_DDRCLK = 1` appears **only in Kconfig capability files**.
There is no DDR implementation anywhere in the SPI master HAL. The only
DDR-adjacent registers are `SPI_D_DQS_MODE` (in `SPI_DOUT_MODE_REG`) and
`SPI_DQS_IDLE_EDGE` (in `SPI_USER_REG`) - a **DQS strobe** scheme for
octal/PSRAM-style interfaces, which needs a strobe line this panel does not
have. The SH8601 is a standard SDR QSPI device.

No 2x from DDR. The bandwidth table's DDR rows are unreachable on this board.

## 0c — Panel-side vertical scroll: **REFUTED (with positive control)**

Drew colour bars once, then animated only `0x37` VSCRSAR with `0x33` VSCRDEF
set to `TFA=0, VSA=448, BFA=0` (sums to panel height, as MIPI DCS requires).
Every command returned `rc=0`.

**The bars did not move.** The SH8601 does not implement DCS vertical scroll,
at least not as sent.

**RED-TEAM RETRACTION.** This conclusion was drawn from *absence of evidence*.
"Nothing moved" is also exactly what a dead command path looks like - and this
project has three documented silent-failure traps (`FWRITE_QUAD` in the wrong
register, missing `COLMOD`, ghost framebuffers x3). Concluding "unimplemented"
without a positive control repeats the precise reasoning pattern that has
burned us every previous time.

Retest in progress: two 6-second phases against the SAME drawn image -
[A] animate `0x37` scroll, [B] pulse `0x51` brightness as a control. Only if
**B pulses and A does not move** is "scroll unimplemented" sound. If neither
happens, the original finding was worthless.

**RESULT (2026-08-24): control PASSED, scroll REFUTED.**

Observed: phase B **blinked** (brightness pulse reached the panel), phase A left
the bars **stationary**. The command path was demonstrably live at the moment
scroll failed to move anything, so the negative is real rather than an artifact
of a dead bus.

The SH8601 does not implement MIPI DCS vertical scroll. Scrolling must be built
from dirty-region updates.

**Method note.** The first run reached the same conclusion from "nothing moved"
alone - and would have reached it even if nothing was getting through. The
conclusion happened to be right; the reasoning was not, and the difference only
became visible because a control was added. *A correct conclusion from invalid
reasoning is not a result, it is a coincidence you have not caught yet.*

---

## What this changes

**The realistic ceiling is 60.6 fps full-frame**, at 40 MHz SDR QSPI, until the
PLL allows a higher bus clock. The 121 / 243 fps rows in the bandwidth table
require 80 MHz (gated on PLL, panel acceptance still unknown) and DDR
(unavailable).

**Elision matters MORE, not less.** Panel-side scroll would have made list
scrolling free. It is not available, so scrolling must be built from
dirty-region updates. The one hardware shortcut that could have substituted for
the software approach does not exist.

**Measured clock sources** (red-team, corrected method):

| source | /1 | /2 | /3 |
|---|---|---|---|
| XTAL | 38.2 MHz | 19.2 MHz | 13.7 MHz |
| APB | 20.2 MHz | 10.1 MHz | 6.6 MHz |

APB tracks the CPU at XTAL/2 with the PLL off, so XTAL is genuinely the fastest
source available today. 0a stays blocked - now for a measured reason.

**The remaining levers are exactly three, all high-confidence:**

| Lever | Effect | Status |
|---|---|---|
| GDMA | 82 ms -> ~18 ms (bus utilisation 20% -> >80%) | ready to build |
| PLL 20 -> 240 MHz | 12x CPU-side; also gates the 80 MHz bus test | ready to build |
| Dirty-region elision | 10-100x on real UI content | after both |

Nothing exotic is left. Phase 0 did its job by removing three maybes.

## Method note

Phase 0 was designed so that a "no" costs an hour instead of a milestone.
Two of the three answers came from reading headers rather than flashing, and
the third took one flash. Compare with `COLMOD`, which cost three milestones
because it was assumed rather than checked.
