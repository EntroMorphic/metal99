# RAW — why are we at 3 fps and what would it take to be embarrassed by that

Unfiltered. Not for anyone else.

---

3.2 fps. I wrote "~35 fps ceiling after GDMA and PLL" in the summary and I think
I was being lazy. I anchored on the ESP-IDF numbers (29.3 fps plasma) because
they were measured, and measured numbers feel safe. But those were measured on a
*different transport* — IDF's DMA path with a PSRAM framebuffer and a bounce
buffer. I imported that ceiling without checking whether it's ours.

Where does 3.2 fps actually come from? 311 ms per frame. Of that, only 16.5 ms is
time on the wire. **94.7% of the frame time is per-transaction setup for 5,152
FIFO transactions.** We are not slow because of physics. We are slow because we
haven't written GDMA yet. The 3 fps number is an artifact of a deliberately
chosen stepping stone. I keep quoting it like it's a property of the hardware.

Let me do the actual bandwidth arithmetic instead of vibing:

    frame = 368 x 448 x 16 bits = 2,637,824 bits
    QSPI  = 4 lines

    40 MHz SDR : 160 Mb/s -> 16.49 ms -> 60.6 fps
    80 MHz SDR : 320 Mb/s ->  8.24 ms -> 121 fps
    80 MHz DDR : 640 Mb/s ->  4.12 ms -> 243 fps

So even at the clock we're already running, the *physical* ceiling is 60 fps, not
35. My "35" was pessimism dressed as measurement.

---

DDR. `SOC_SPI_SUPPORT_DDRCLK = 1`. I did not know that. Data on both clock edges.
That's a free 2x that costs no clock increase and no extra pins. Does the SH8601
accept DDR on its QSPI input? No idea. Probably not — most of these AMOLED
controllers are SDR-only on the command interface. But I don't *know*, and the
difference between "probably not" and "checked" is the whole game here. Even if
the panel is SDR-only, worth knowing definitively.

80 MHz — the BSP picked 40. Is that a panel limit or Waveshare being careful?
Vendor BSPs are conservative by default. The SH8601 datasheet would say. We don't
have it. But we can *binary-search it empirically*: raise the clock until the
panel produces garbage, back off 20%. We have a perfect test image — colour bars.
Corruption would be instantly visible. That's a 20-minute experiment.

I like this because it inverts the usual problem: normally you can't tell if a
bus is marginal. Here the panel is a giant error display.

---

Now the thing that's been nagging me.

**Why am I measuring full-frame fps at all?**

Tripp got 1M fps on a Pi4. That was the *message layer*, not display refresh. No
display refreshes a million times a second. The number meant "the substrate
imposes no meaningful cost." The display is a separate, physically bounded thing.

So what's the right metric for *this* device? Full-frame refresh is the wrong one
for an interface. Interfaces don't repaint everything. A clock face changes a few
hundred pixels a second. A button press lights one rectangle. A scrolling list
moves a region.

    full frame  @ 80MHz SDR = 8.24 ms
    32x32 tile  @ 80MHz SDR = 2048 B = 16,384 bits = 51 us
    32x32 tile  @ 80MHz DDR = 25.6 us

51 microseconds. That's ~19,500 small updates per second. THAT is where the
million-fps spirit actually lives on this hardware. Not repainting the world 240
times a second — touching only what changed, thousands of times a second, with
sub-millisecond latency to glass.

We already have the mechanism! `0x2A`/`0x2B` set an arbitrary address window. We
use it to set full-screen every frame. It could set a 32x32 box instead. The
panel has its own framebuffer — that's the whole reason ghost images kept fooling
us. **The panel's memory is not our enemy, it's a free compositing surface we've
been overwriting wholesale.**

That reframes the ghost-image trap as an asset. It bit us three times. It's
actually the feature.

---

Other things rattling around:

**GDMA descriptors can point at the same memory repeatedly.** A solid-colour fill
doesn't need a big buffer — it needs one small buffer and N descriptors all
pointing at it. Same for vertical gradients (one row per distinct value), tiled
backgrounds, repeated sprites. Near-zero RAM, zero CPU per pixel. The DMA engine
becomes a crude blitter. This might be the single most useful trick available and
it costs nothing but descriptor setup.

**Two cores.** Core 1 renders frame N+1 while core 0 runs DMA for frame N. But if
DMA is truly async, core 0 isn't doing anything during flush anyway — so the real
win is just "render during flush," which one core can do. Second core is for when
render exceeds flush. At 240 MHz, plasma is ~11.7 ms render vs 8.24 ms flush at
80 MHz, so render becomes the bottleneck and the second core matters again. Only
after the PLL though.

**Panel-side scroll.** MIPI DCS has `0x33` VSCRDEF and `0x37` VSCRSAR — vertical
scroll defined entirely in the panel. If SH8601 implements it, scrolling a list
costs *one command* instead of a full repaint. That's not a 2x, that's an
asymptote change. Unknown whether supported. Cheap to test.

**Partial mode** `0x30`/`0x12` — panel displays a subset, powers down the rest.
On AMOLED that's a battery play more than a speed play, but worth knowing.

**TE / tearing effect.** `0x35` is already enabled in our init. There's a TE pin
we're not reading. Syncing to it doesn't add throughput but removes tearing,
which matters if we're doing partial updates at high rates — you'd see shear
otherwise.

---

Doubts, honestly:

- The panel might cap at 40 MHz and refuse DDR entirely. Then the bus levers give
  us nothing and it's GDMA + dirty rects only. That's still 3 -> 60 fps, so fine.
- Dirty-rect tracking has CPU cost. At 20 MHz that cost could exceed the transfer
  it saves for small changes. Needs the PLL first, or a very cheap dirty scheme
  (tile hashing, not per-pixel diff).
- Many small address-window updates each carry command overhead (~6 commands of
  4 bytes + params). At some tile size, overhead dominates again. There's an
  optimal tile size and I don't know it. Measurable though.
- I keep wanting to design the message layer. Every time I do, I discover a
  hardware fact that would have changed the design. Resist.

---

The thing I actually believe after writing this:

3 fps isn't a finding, it's a placeholder. The honest bound at the current clock
is 60 fps, and the interesting number isn't fps at all — it's ~20,000 small
updates/sec, which is a completely different kind of machine to build an
interface on. The message layer's job is not to be fast. **Its job is to know
what didn't change, so we don't send it.**
