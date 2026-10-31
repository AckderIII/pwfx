#!/usr/bin/env python3
"""
meter.py -- out-level is what the filter produces, connected or not.

A stage whose output goes nowhere -- the last stage of a chain with no
output chosen, or one being set up before it is wired -- used to read -240
on out-level while its in-level moved: only linked output ports were
metered. Here the input is linked and the output is not, and:

  level     pw_gain at -6 dB: out-level is in-level - 6.
  channels  pw_gain in stereo with balance -50: channel 1 untouched,
            channel 2 at -6 dB. out-level is the loudest channel, so it
            equals in-level. Unconnected outputs once shared one scratch
            buffer, each channel overwriting the last; metered, that reads
            channel 2's level, 6 dB low.

usage: meter.py [BIN]
"""

import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import (Filter, check, fails, find_node, pw_dump, wait_for_node,   # noqa: E402
                  write_wav_f32, TIMEOUT)

RATE = 48000
NODE = "meter-test"


def measure(binpath, ch, args):
    """Play a 0.3 tone into the filter with its output connected to
    nothing; return the highest in-level and out-level it reports."""
    f = Filter([binpath, "-c", str(ch), "--node-name", NODE] + args)
    while True:
        l = f.line(10.0)
        if l is None or l.startswith("#"):
            break
    if wait_for_node(NODE) is None:
        f.stop()
        raise RuntimeError("node never appeared")
    f.set("meter 50")

    tmp = tempfile.mkdtemp(prefix="pwf-meter-")
    src = os.path.join(tmp, "in.wav")
    n = RATE * 3
    write_wav_f32(src, RATE, ch,
                  [v for i in range(n) for v in [0.3 * math.sin(2 * math.pi * 1000 * i / RATE)] * ch])
    play = subprocess.Popen(["pw-cat", "-p", "--target", "0", "-P", "{ node.name = pwfsrc-meter }", src],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    levels = {"in-level": [], "out-level": []}
    try:
        end = time.time() + TIMEOUT
        while time.time() < end and not find_node(pw_dump(), "pwfsrc-meter"):
            time.sleep(0.2)
        subprocess.run(["pw-link", "pwfsrc-meter", NODE], capture_output=True)
        end = time.time() + 3.5
        while time.time() < end:
            l = f.line(0.2, skip_comments=True)
            if l:
                k, _, v = l.partition(" ")
                if k in levels:
                    levels[k].append(float(v))
    finally:
        if play.poll() is None:
            play.terminate()
        play.wait(timeout=5)
        f.stop()
        shutil.rmtree(tmp, ignore_errors=True)
    return max(levels["in-level"], default=-240.0), max(levels["out-level"], default=-240.0)


def main():
    binpath = sys.argv[1] if len(sys.argv) > 1 else "./bin/pw_gain"
    print("== out-level with nothing connected to the output ==")
    try:
        lin, lout = measure(binpath, 1, ["-g", "-6"])
        check(lin > -12 and abs((lout - lin) + 6.0) <= 0.15,
              "gain -6: out-level %+.1f is in-level %+.1f less 6 dB" % (lout, lin))
    except Exception as e:
        check(False, "level", repr(e))
    try:
        lin, lout = measure(binpath, 2, ["-b", "-50"])
        check(lin > -12 and abs(lout - lin) <= 0.15,
              "balance -50, stereo: out-level %+.1f is the loud channel's, in-level %+.1f"
              % (lout, lin))
    except Exception as e:
        check(False, "channels", repr(e))
    print("meter: %d failure(s)\n" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
