#!/usr/bin/env python3
"""
gain.py -- pw_gain in the graph, measured per channel against what its
settings say: the level of each channel, its polarity, and that switching
any of it while audio flows does not click.

pw_gain has no reference implementation (see the top of src/pw_gain.c), so
this is the ground truth for it, and every expectation is arithmetic:

  level     20 log10(rms out / rms in) per channel -- the gain, minus
            20 log10(1 - balance) on the side the balance turns down
  polarity  the sign of mean(x^3). The test signal, 0.2 sin t + 0.1 cos 2t,
            is skewed: its third moment is -0.0015, not 0, and flipping the
            polarity flips its sign. Unlike correlating against the input,
            that needs no idea of how long the graph took, and a pure sine
            could not do it at all -- inverted, it is the same sine half a
            period late.
  no click  a change mid-stream may not make any step between samples
            larger than the signal's own steepest step allows; an unramped
            polarity flip jumps by up to twice the peak.

usage: gain.py [BIN]
"""

import math
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import Filter, check, fails, run_audio, wait_for_node, db   # noqa: E402

RATE, CH, F = 48000, 2, 220.0


def skewed(n, rate):
    w = 2 * math.pi * F / rate
    return [0.2 * math.sin(w * i) + 0.1 * math.cos(2 * w * i) for i in range(n)]


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def steady(x):
    # skip the first and last second: the links are made and broken there
    return x[RATE:-RATE]


def run(binpath, args, during=None, seconds=4.0):
    name = "pw_gain-test"
    f = Filter([binpath, "-c", str(CH), "--node-name", name] + args)
    while True:
        l = f.line(10.0)
        if l is None or l.startswith("#"):
            break
    if wait_for_node(name) is None:
        f.stop()
        raise RuntimeError("node never appeared")
    t = None
    if during:
        t = threading.Thread(target=during, args=(f,), daemon=True)
        t.start()
    try:
        _, raw, _ = run_audio(name, RATE, CH, [], seconds=seconds, extra_src=skewed)
    finally:
        if t:
            t.join(timeout=10)
        f.stop()
    return [raw[c::CH] for c in range(CH)]


def main():
    binpath = sys.argv[1] if len(sys.argv) > 1 else "./bin/pw_gain"
    print("== pw_gain: level, polarity and ramps per channel ==")

    src = skewed(RATE * 2, RATE)
    src_rms, src_m3 = rms(src), sum(v ** 3 for v in src) / len(src)

    # (args, what, wanted dB per channel or None for silence, polarity per channel)
    cases = [
        ([], "defaults", [0.0, 0.0], [1, 1]),
        (["-g", "-12"], "gain -12", [-12.0, -12.0], [1, 1]),
        (["-g", "6"], "gain +6", [6.0, 6.0], [1, 1]),
        (["-i", "left"], "invert left", [0.0, 0.0], [-1, 1]),
        (["-i", "right"], "invert right", [0.0, 0.0], [1, -1]),
        (["-i", "all"], "invert all", [0.0, 0.0], [-1, -1]),
        (["-b", "50"], "balance +50", [20 * math.log10(0.5), 0.0], [1, 1]),
        (["-b", "-100"], "balance -100", [0.0, None], [1, 0]),
        (["-m"], "mute", [None, None], [0, 0]),
    ]
    for args, what, want_db, want_pol in cases:
        try:
            chans = run(binpath, args)
        except Exception as e:
            check(False, what, repr(e))
            continue
        for c in range(CH):
            x = steady(chans[c])
            got = db(rms(x) / src_rms) if rms(x) > 0 else -240.0
            m3 = sum(v ** 3 for v in x) / len(x) if x else 0.0
            pol = 0 if abs(m3) < abs(src_m3) * 1e-4 else (1 if (m3 > 0) == (src_m3 > 0) else -1)
            if want_db[c] is None:
                check(got < -100, "%s, channel %d: silent" % (what, c + 1), "%.1f dB" % got)
            else:
                check(abs(got - want_db[c]) <= 0.05 and pol == want_pol[c],
                      "%s, channel %d: %+.2f dB, polarity %+d" % (what, c + 1, got, pol),
                      "want %+.2f dB, polarity %+d" % (want_db[c], want_pol[c]))

    # Switch everything there is to switch while audio flows. The steepest
    # step the signal takes on its own is the bound; a ramp adds at most the
    # peak times 2/480 per sample to it.
    def switch(f):
        time.sleep(1.4)
        for cmd in ("invert all", "mute on", "mute off", "invert left",
                    "gain -20", "gain 0", "balance 100", "balance -40"):
            f.send(cmd)
            time.sleep(0.15)

    try:
        chans = run(binpath, [], during=switch, seconds=5.0)
        src_step = max(abs(src[i + 1] - src[i]) for i in range(len(src) - 1))
        for c in range(CH):
            x = chans[c][int(RATE * 0.8):-int(RATE * 0.8)]
            step = max(abs(x[i + 1] - x[i]) for i in range(len(x) - 1))
            check(step <= src_step * 1.25,
                  "channel %d: no step over %.4f while switching (largest %.4f)"
                  % (c + 1, src_step * 1.25, step))
    except Exception as e:
        check(False, "switching while audio flows", repr(e))

    print("pw_gain: %d failure(s)\n" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
