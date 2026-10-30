#!/usr/bin/env python3
"""
delay.py -- pw_delay in the graph: that the delay it adds is exactly the
time it was set to, that it reports none of it as latency, and that
changing the time while audio flows does not click.

The measurement is self-referenced, so it does not depend on how long the
graph takes. At mix 50 and no feedback every impulse comes out twice on
the one path -- the dry copy, then the delayed one -- and the gap between
the two is the delay and nothing else, to the sample.

Comparing two paths instead (one direct, or one through a second node set
to 0) is not reliable here: freshly linked nodes are sometimes a whole
quantum apart (4096 frames at pw-cat's quantum), on one path in one run and
the other in the next. Two nodes in series are no further apart than one,
so it is not something a filter adds per stage -- but it would sit in the
offset, so the delay is measured within one path instead.

usage: delay.py [BIN]
"""

import math
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import Filter, check, fails, run_audio, wait_for_node   # noqa: E402

RATE = 48000
NODE = "pw_delay-test"


def start(binpath, args):
    f = Filter([binpath, "-c", "1", "--node-name", NODE] + args)
    dump = {}
    while True:
        l = f.line(10.0)
        if l is None or l.startswith("#"):
            break
        k, _, v = l.partition(" ")
        dump[k] = v
    if wait_for_node(NODE) is None:
        f.stop()
        raise RuntimeError("node never appeared")
    return f, dump


def onsets(x, thresh=0.25):
    """Every sample that rises over the threshold from below it."""
    return [i for i in range(1, len(x)) if abs(x[i]) > thresh >= abs(x[i - 1])]


def main():
    binpath = sys.argv[1] if len(sys.argv) > 1 else "./bin/pw_delay"
    print("== pw_delay: the delay in the graph, to the sample ==")

    # An impulse every 0.6 s, so a lost one or a doubled one shows, and
    # longer apart than the longest time tested.
    def pulses(n, rate):
        x = [0.0] * n
        for k in range(1, 6):
            x[k * rate * 6 // 10] = 0.8
        return x

    for t in (10.0, 1.0, 0.0, 333.0):
        want = int(t * RATE / 1000 + 0.5)
        try:
            f, dump = start(binpath, ["-t", str(t), "-x", "50"])
            check(dump.get("latency") == "0",
                  "time %g ms: reports latency 0, not %s" % (t, want), "got %r" % dump.get("latency"))
            mono, _, _ = run_audio(NODE, RATE, 1, [], seconds=4.0, extra_src=pulses)
            f.stop()
        except Exception as e:
            check(False, "time %g ms" % t, repr(e))
            continue
        on = onsets(mono)
        if want == 0:
            # dry and delayed land on the same sample: one impulse each, whole
            ok = len(on) == 5 and all(abs(mono[i] - 0.8) < 1e-6 for i in on)
            check(ok, "time 0: each impulse comes out once, untouched", "onsets %s" % on)
        else:
            pairs = [on[i:i + 2] for i in range(0, len(on), 2)]
            gaps = [b - a for a, b in pairs if len([a, b]) == 2]
            check(len(on) == 10 and all(g == want for g in gaps),
                  "time %g ms: each echo lands exactly %d samples after its impulse" % (t, want),
                  "onsets %s" % on)

    # Change the time while a tone plays, all wet. The steepest step the tone
    # takes on its own is the bound; a crossfade between two taps adds at
    # most twice the peak over its 481 samples to it.
    def switch(f):
        time.sleep(1.2)
        for cmd in ("time 25", "time 5", "time 40", "time 41", "time 42", "time 12"):
            f.send(cmd)
            time.sleep(0.12)

    try:
        f, _ = start(binpath, ["-t", "10"])
        t = threading.Thread(target=switch, args=(f,), daemon=True)
        t.start()
        mono, _, _ = run_audio(NODE, RATE, 1, [220], seconds=4.0)
        t.join(timeout=10)
        f.stop()
        w = 2 * math.pi * 220 / RATE
        src_step = 0.3 * 2 * math.sin(w / 2)
        x = mono[int(RATE * 0.6):-int(RATE * 0.6)]
        step = max(abs(x[i + 1] - x[i]) for i in range(len(x) - 1))
        check(step <= src_step * 1.25,
              "no step over %.4f while the time changes (largest %.4f)" % (src_step * 1.25, step))
    except Exception as e:
        check(False, "changing the time while audio flows", repr(e))

    print("pw_delay: %d failure(s)\n" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
