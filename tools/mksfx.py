#!/usr/bin/env python3
"""Bake sound effects into the firmware as raw PCM.

Decoding happens HERE, at build time, for the same reason the fonts and the
trig table are generated here: an MP3 decoder is tens of kilobytes of code plus
real CPU per frame, against a runtime with no libc, no malloc and no floating
point. The device should receive samples, not a codec.

16-BIT, after listening. It started at 8-bit to save SRAM - 33 KB against 67 -
and the result played but did not sound like the source. Measuring the sources
said why: 94% of the energy in both effects is below 4 kHz, so the 7.8 kHz
bandwidth of 15625 Hz sampling loses almost nothing, and the audible damage was
all quantisation. 8-bit is ~48 dB SNR with no dither, which is grit on every
decay tail. 67 KB against ~121 KB free is affordable; sounding wrong is not.

31250 Hz because that is what the hardware produces exactly - XTAL/5 for MCLK,
then /8 and /32 - and because 15625 was audibly not enough. See i2s.h: the
effects were rough while a tone through the same path was clean, and the cause
was bandwidth rather than anything in the pipeline.

  ./tools/mksfx.py --list        what would be generated
  ./tools/mksfx.py               regenerate metal99/src/sfx_data.c
"""
import argparse, os, struct, subprocess, sys, textwrap

RATE = 31250
SRC = os.path.expanduser("~/Projects/002-business/inlikeflynn-io/assets/audio/sfx")
LOCAL = os.path.join(os.path.dirname(__file__), "sfx_src")
OUT = os.path.join(os.path.dirname(__file__), "..", "metal99", "src", "sfx_data.c")

# name in C, source file, trim to seconds (None = whole file)
EFFECTS = [
    ("SFX_FIRE",  "blaster.mp3",      None),
    ("SFX_KILL",  "direct-hit-1.mp3", None),
    # A KNOWN-GOOD SIGNAL THROUGH THE SAME PIPELINE. Every diagnostic tone so
    # far was generated at runtime, which tests the mixer and DMA but skips
    # decode, resample, filter, normalise and the C array entirely. If this
    # comes out clean and the effects do not, the pipeline is exonerated and
    # the fault is the content or the speaker. If it comes out rough, the
    # pipeline is the fault and no amount of source work would ever have fixed
    # it. Local file, so it is reproducible from this repo alone.
    ("SFX_PROBE", "probe-sine.wav",   None),
]


# The speaker on this board is a ~10 mm driver. Below roughly this frequency it
# does not reproduce, it EXCURSES - the cone hits its limit and the result is a
# rattle rather than a note.
HIGHPASS_HZ = 450


def decode(path, limit):
    """MP3 -> mono, RATE Hz, signed 16-bit, high-passed, via ffmpeg.

    THE HIGH-PASS IS THE WHOLE POINT and it took an embarrassing amount of
    measuring to find. These effects are sub-bass: 49% of the blaster and 97%
    of the direct hit sit between 20 and 150 Hz, because they were mixed for
    headphones and desktop speakers where that is the impact. Played through a
    10 mm driver it is not impact, it is a cone slapping its endstops - which
    sounds exactly like raking rocks, and is not a defect in any of the code it
    passes through on the way.

    Every diagnostic tone this project used sat at 488 Hz, comfortably above
    the problem band, which is why the path kept certifying clean while the
    effects did not.

    Two poles, so the rolloff is steep enough to actually remove the content
    rather than merely tilt it.
    """
    cmd = ["ffmpeg", "-v", "error", "-i", path,
           "-ac", "1",
           "-af", f"highpass=f={HIGHPASS_HZ}:poles=2,highpass=f={HIGHPASS_HZ}:poles=2",
           "-ar", str(RATE), "-f", "s16le", "-"]
    if limit:
        cmd[4:4] = ["-t", str(limit)]
    out = subprocess.run(cmd, capture_output=True, check=True).stdout
    return out


def to_s16(raw):
    """Little-endian signed 16-bit bytes -> list of ints."""
    return list(struct.unpack(f"<{len(raw)//2}h", raw[:len(raw)//2*2]))


def trim(pcm, name):
    """Strip leading and trailing near-silence.

    THIS IS NOT AN OPTIMISATION, it is the difference between a sound effect
    and a bug. blaster.mp3 opens with 0.42 s of silence - 42% of the clip -
    because it was exported for a web page where a few hundred milliseconds of
    lead-in is invisible. On a trigger-driven effect it means every shot is
    silent for nearly half a second, and firing repeatedly fills all four
    voices with clips that have not started yet. They get stolen before they
    make a sound, and whatever survives fires late and overlapping.

    Threshold is 1% of the clip's own peak, so a quiet effect is not judged
    against a loud one.
    """
    if not pcm:
        return pcm
    thr = max(abs(v) for v in pcm) // 100
    lead = next((i for i, v in enumerate(pcm) if abs(v) > thr), 0)
    tail = len(pcm) - next((i for i, v in enumerate(reversed(pcm)) if abs(v) > thr), 0)
    if lead or tail != len(pcm):
        print(f"  {name:10s} trimmed {lead/RATE:.3f}s lead, "
              f"{(len(pcm)-tail)/RATE:.3f}s tail")
    return pcm[lead:tail]


def normalise(clips):
    """Scale every clip by ONE common factor.

    Per-clip normalisation was wrong and audibly so: it makes the quiet effect
    as loud as the loud one, discarding the relative levels whoever mixed the
    originals chose. A blaster is not supposed to be as big as a direct hit.
    One factor, taken from the loudest clip, preserves that relationship while
    still using the available range.
    """
    peak = max((max(abs(v) for v in c) for c in clips if c), default=1) or 1
    # After the high-pass most of the original energy is gone, so this is what
    # brings the remainder back to a usable level. Normalising BEFORE filtering
    # would have scaled to a peak that is about to be thrown away.
    g = 30000  # leave headroom; four voices can sum
    return [[max(-32768, min(32767, (v * g) // peak)) for v in c] for c in clips]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list:
        for name, f, limit in EFFECTS:
            p = os.path.join(SRC, f)
            if not os.path.exists(p): p = os.path.join(LOCAL, f)
            print(f"  {name:10s} {f:22s} {'present' if os.path.exists(p) else 'MISSING'}")
        return 0

    raw = []
    for name, fname, limit in EFFECTS:
        path = os.path.join(SRC, fname)
        if not os.path.exists(path):
            path = os.path.join(LOCAL, fname)
        if not os.path.exists(path):
            sys.exit(f"missing source: {path}")
        raw.append(trim(to_s16(decode(path, limit)), name))
    clips = normalise(raw)

    body, table, total = [], [], 0
    for (name, fname, limit), pcm in zip(EFFECTS, clips):
        total += len(pcm) * 2
        rows = textwrap.wrap(", ".join(f"{v:6d}" for v in pcm), 76)
        body.append(f"/* {fname} - {len(pcm)} samples, {len(pcm)/RATE:.2f} s */\n"
                    f"static const int16_t {name}_PCM[{len(pcm)}] = {{\n" +
                    "\n".join("    " + r for r in rows) + "\n};\n")
        table.append(f"    {{ {name}_PCM, {len(pcm)}u }}")
        print(f"  {name:10s} {len(pcm):6d} samples  {len(pcm)*2/1024:5.1f} KB  "
              f"{len(pcm)/RATE:.2f} s  peak {max(abs(v) for v in pcm)}")

    with open(OUT, "w") as fh:
        fh.write("/* GENERATED by tools/mksfx.py - do not edit.\n"
                 " *\n"
                 f" * {RATE} Hz, mono, signed 16-bit, scaled by one common factor.\n"
                 " * Sources live outside this repo; see the generator for paths.\n"
                 " */\n"
                 '#include "sfx.h"\n\n')
        fh.write("\n".join(body))
        fh.write("\nconst sfx_clip SFX[] = {\n" + ",\n".join(table) + "\n};\n")
        fh.write(f"const uint32_t SFX_COUNT = {len(EFFECTS)}u;\n")

    print(f"  total {total/1024:.1f} KB -> {os.path.relpath(OUT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
