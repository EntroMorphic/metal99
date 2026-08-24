# REFLECT — structure, assumptions, leverage

Where the raw material stops being a list and starts being a shape.

---

## The unstated assumption underneath everything: *the frame is the unit of work*

Look at what we built. `sh8601_write_frame()`. Full-screen address window, every
time. Row streaming top to bottom. fps as the metric. The spec's §7 inherits
NeoGPU's `begin_frame` / `execute` / `end_frame` / `present`.

Every one of those encodes: **the display is a sink you pour a frame into.**

That is how GPUs work. It is how the Pi works. It is not how this panel works.

This panel has its own memory. It holds an image indefinitely with the CPU
halted, through a software reset, and across a firmware reflash. We know this
better than we know almost anything else about it, because it produced ghost
images that fooled us **three separate times.**

So the panel is not a sink. **It is a stateful peer with its own framebuffer,
and we have been treating it as write-only.** Nearly every performance decision
downstream inherits from which of those two models you hold.

If it's a sink: you optimise throughput. Push frames faster. GDMA, higher clock,
DDR, more cores. All real, all bounded by 2.6 Mbit per frame.

If it's a stateful peer: you optimise **elision.** The fastest pixel is the one
you never send. The bound is not bandwidth, it's how much you can prove is
already correct on the far side.

## The three traps had one root, and it is the same root as the opportunity

| Trap | What happened |
|---|---|
| `FWRITE_QUAD` in the wrong register | Transfer completed, on one line, silently |
| Missing `COLMOD` | Panel accepted every byte, rendered nothing |
| Ghost framebuffer, x3 | Stale image indistinguishable from a fresh one |

All three: **remote state we could not observe, where stale looked like success.**

The counter-discipline we adopted — *only change is evidence* — is not just a
testing rule. It is a statement about the architecture. There is a stateful thing
on the other side of a write-only bus, and we cannot read it back.

That cuts both ways, and the second edge is the interesting one. A dirty-region
system is **a model of remote state we cannot verify.** If the model drifts from
reality, we get exactly the failure we've hit three times: something looks right
and isn't. So any elision scheme must carry the same discipline — it needs a way
to resynchronise, and a way to prove what it chose not to send.

This is not a footnote. It is the main design constraint on the whole layer.

## Why I anchored on 35 fps, and why that matters more than the number

I imported it from ESP-IDF measurements. They were real measurements — on a
different transport, with a PSRAM framebuffer and a bounce buffer we do not use
and cannot use.

That is the ghost-image failure again, wearing a different coat: **a stale
artifact from a prior configuration, mistaken for current truth, because it had
the authority of having been measured.**

I have now made this mistake at three different levels: register, panel, and
architecture. The lesson isn't "check your numbers." It's that in a system where
state persists invisibly across configuration changes, *provenance* matters as
much as measurement. A number without its transport is not data.

## The leverage, honestly ranked

Sorted by (multiplier x confidence) / effort:

| # | Lever | Effect | Confidence | Effort |
|---|---|---|---|---|
| 1 | **GDMA** | 3.2 -> ~55 fps (**17x**) | High — arithmetic, 94.7% is overhead | Medium |
| 2 | **Dirty regions** | 10-100x on real UI content | High — mechanism already built & verified | Medium |
| 3 | **PLL 20 -> 240 MHz** | 12x on all CPU-side work | High — standard, well documented | Medium |
| 4 | **Bus 40 -> 80 MHz** | 2x flush | Medium — vendor default, not a proven limit | Low |
| 5 | **DDR** | 2x flush | Low — silicon yes, panel unknown | Low |
| 6 | **Panel-side scroll** | asymptote change for lists | Low — may not be implemented | Low |
| 7 | **Second core** | up to 2x, *only after PLL* | High | High |

Two observations that change the order:

**Levers 4, 5 and 6 are cheap experiments, not projects.** Raise the clock until
the bars corrupt. Try DDR, look at the screen. Send `0x33`/`0x37`, watch what
moves. Each is under an hour, and the panel reports its own failures visibly.
**Cheap unknowns should be resolved before expensive work is designed around
guesses.** We learned that from `COLMOD`.

**Lever 7 inverts after lever 3.** Today flush (311 ms) dwarfs render (11.6 ms
plasma at 20 MHz... which was 139 ms — no: measured 288 ms of the gradient's
599 ms was render). After GDMA + PLL: flush ~16.5 ms, render ~11.7 ms. They
become comparable, and *then* the second core matters. Not before.

## What the message layer is actually for

Not transport. We have transport — it works, it's verified, it's 185 lines.

Its job is **to know what did not change, so we never send it.**

That reframes throughput completely. On the Pi, 1M msg/sec meant "the substrate
imposes no cost." Here the equivalent is: **accept millions of drawing
operations and elide almost all of them into a few small panel writes.** The
layer's speed is measured by how much it can *throw away*, not how much it can
push.

A message layer that pushes a million full frames per second at a panel that
accepts 60 is not fast. It is 16,000x wasteful.

## The metric we should actually optimise

Not fps. **Latency from "something changed" to "photons changed."**

    full frame @ 40 MHz SDR = 16.5 ms
    32x32 tile @ 40 MHz SDR =  102 us
    32x32 tile @ 80 MHz DDR =   26 us

A UI that responds in 100 microseconds is not "60 fps." It is a different
category of object. And it is reachable with mechanisms we have already built
and verified — the address window is `0x2A`/`0x2B`, which we have been calling
correctly since the pixel path came up.

We have been using a scalpel as a paint roller.
