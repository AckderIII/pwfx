#!/usr/bin/env python3
"""
impulse.py -- the delay core's impulse response, offline, against theory.

The reference comparison in run.sh proves the core and reference/delay.c
agree; this proves what they agree on is right. An impulse in gives, for a
delay of D samples, feedback g and wet share m:

    dry (1 - m) at 0,  then m * g^(k-1) at k * D for k = 1, 2, ...

and exactly zero everywhere else -- no smearing, no off-by-one, at any
block size. Run through bin/t_delay, the code the filter runs.
"""

import struct
import subprocess
import sys

RATE = 48000
fails = 0


def check(ok, what, detail=""):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what + (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        fails += 1


def response(n, *args):
    pulse = struct.pack("<%df" % n, *([1.0] + [0.0] * (n - 1)))
    out = subprocess.run(["./bin/t_delay", "-r", str(RATE), "-c", "1", *args],
                         input=pulse, capture_output=True).stdout
    return struct.unpack("<%df" % (len(out) // 4), out)


for t, fb, mix, block in ((10, 0, 100, 128), (10, 0, 100, 97), (2.5, 50, 40, 333),
                          (100, 95, 30, 1024), (0.0208333, 30, 60, 1)):
    D = int(t * RATE / 1000 + 0.5)
    g, m = fb / 100.0, mix / 100.0
    n = max(D * 6, 64) + 1
    y = response(n, "-t", str(t), "-f", str(fb), "-m", str(mix), "-b", str(block))
    want = [0.0] * n
    want[0] = 1 - m
    k = 1
    while k * D < n:
        want[k * D] += m * g ** (k - 1)
        k += 1
    err = max(abs(a - b) for a, b in zip(y, want))
    check(len(y) == n and err < 1e-6,
          "time %g ms (%d samples), feedback %g %%, mix %g %%, block %d: "
          "repeats where and as loud as they should be" % (t, D, fb, mix, block),
          "worst %.2e" % err)

y = response(64, "-t", "0", "-f", "50", "-m", "70")
check(y[0] == 1.0 and all(v == 0.0 for v in y[1:]),
      "time 0 passes the input straight through, whatever the feedback and mix")

sys.exit(fails)
