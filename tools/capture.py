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
import argparse, re, sys, time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: pip install pyserial")

DEFAULT_PORT = "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:ED:A0:AC:91:54-if00"


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
    ap.add_argument("-p", "--port", default=DEFAULT_PORT)
    ap.add_argument("-s", "--seconds", type=float, default=12.0)
    ap.add_argument("-r", "--reset", action="store_true", help="reset the board first")
    ap.add_argument("-g", "--grep", default=None, help="regex line filter")
    ap.add_argument("--raw", action="store_true", help="unfiltered, keep blank lines")
    ap.add_argument("-t", "--timestamp", action="store_true",
                    help="prefix each line with arrival time - use to measure\non-device intervals against a host clock, e.g. deriving CPU frequency")
    a = ap.parse_args()
    out = capture(a.port, a.seconds, a.reset, a.grep, a.raw, a.timestamp)
    print(out if out else "(no output captured)")


if __name__ == "__main__":
    main()
