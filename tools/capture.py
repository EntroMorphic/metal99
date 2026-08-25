#!/usr/bin/env python3
"""Capture the metal99 console over USB-Serial-JTAG.

Written because this was retyped inline roughly thirty times during bring-up.
Research tooling belongs in the repo, not in shell history.

  ./tools/capture.py                       # 12s, no reset
  ./tools/capture.py -r -s 20              # reset first, 20s
  ./tools/capture.py -r -g 'frame|PASS'    # only matching lines
  ./tools/capture.py --raw                 # everything, unfiltered

NOTE on reset: toggling DTR/RTS on USB-Serial-JTAG resets the chip. Flush the
input buffer BEFORE the reset, never after, or the boot banner is discarded -
a mistake that cost several confusing captures.
"""
import argparse, glob, re, sys, time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing: pip install pyserial")

ESPRESSIF_VID = 0x303A          # Espressif Systems, USB-Serial-JTAG


def find_port():
    """Locate the board rather than hardcoding one.

    This used to default to a by-id path with one specific board's MAC baked
    into it. That worked on exactly one machine with exactly one board plugged
    in - README's quickstart failed for anyone else - and tools/flash.sh had
    drifted to a different default (/dev/ttyACM1) for the same device. One
    rule now, used by both.
    """
    hits = [p.device for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
    if not hits:
        # by-id survives re-enumeration renaming ttyACMn; fall back to it, then
        # to the raw device nodes, for setups where VID is not exposed.
        hits = sorted(glob.glob("/dev/serial/by-id/*USB_JTAG*")) \
            or sorted(glob.glob("/dev/ttyACM*"))
    if not hits:
        sys.exit("no Espressif USB-Serial-JTAG device found.\n"
                 "  is the board plugged in?  lsusb | grep 303a\n"
                 "  override with -p /dev/ttyACMn")
    if len(hits) > 1:
        sys.exit("several candidate ports; pick one with -p:\n  " +
                 "\n  ".join(hits))
    return hits[0]


def capture(port, seconds, do_reset, pattern, raw, stamp=False):
    try:
        p = serial.Serial(port, 115200, timeout=0.3)
    except Exception as e:
        sys.exit("cannot open %s: %s\n(is the board enumerated? lsusb | grep 303a)" % (port, e))

    p.reset_input_buffer()              # BEFORE reset, not after
    if do_reset:
        p.setDTR(False); p.setRTS(False); time.sleep(0.10)
        p.setRTS(True);  time.sleep(0.15)
        p.setRTS(False)

    buf, t0, stamped = b"", time.time(), []
    while time.time() - t0 < seconds:
        d = p.read(4096)
        if d:
            buf += d
            if stamp:
                # timestamp each COMPLETE line as it arrives; the partial tail
                # stays in the buffer until its newline turns up
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    stamped.append((time.time() - t0,
                                    line.decode("utf-8", "replace").rstrip()))
    p.close()

    if stamp:
        rx = re.compile(pattern) if pattern else None
        return "\n".join("%8.3fs  %s" % (t, l) for t, l in stamped
                          if l.strip() and (rx.search(l) if rx else True))

    text = buf.decode("utf-8", "replace")
    if raw:
        return text
    rx = re.compile(pattern) if pattern else None
    keep = [l.rstrip() for l in text.splitlines()
            if l.strip() and (rx.search(l) if rx else True)]
    return "\n".join(keep)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None,
                    help="serial port; auto-detected when omitted")
    ap.add_argument("-s", "--seconds", type=float, default=12.0)
    ap.add_argument("-r", "--reset", action="store_true", help="reset the board first")
    ap.add_argument("-g", "--grep", default=None, help="regex line filter")
    ap.add_argument("--raw", action="store_true", help="unfiltered, keep blank lines")
    ap.add_argument("-t", "--timestamp", action="store_true",
                    help="prefix each line with arrival time - use to measure\non-device intervals against a host clock, e.g. deriving CPU frequency")
    ap.add_argument("--print-port", action="store_true",
                    help="resolve the port, print it and exit "
                         "(so flash.sh and this script cannot disagree)")
    a = ap.parse_args()
    port = a.port or find_port()
    if a.print_port:
        print(port)
        return
    out = capture(port, a.seconds, a.reset, a.grep, a.raw, a.timestamp)
    print(out if out else "(no output captured)")


if __name__ == "__main__":
    main()
