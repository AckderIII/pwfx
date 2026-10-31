#!/usr/bin/env python3
"""
bypass.py -- bypassing a filter that delays its output keeps its timing.

A lookahead filter delays its output by the latency it reports. Its bypass
used to crossfade to the input *undelayed*, which did two things this test
was written against, measured on pw_comp before the fix:

  - every toggle moved the output in time by the latency -- 480 samples
    earlier at bypass on, 480 later at bypass off, at 10 ms of lookahead --
    while the filter went on reporting a latency it no longer added;
  - the 10 ms crossfade mixed the signal with itself shifted by the
    latency, a comb: a tone whose half period is the latency fell from
    0.300 to 0.086 at both toggles.

Each lookahead filter is made transparent, so its processed signal is the
input delayed by exactly its latency and anything that changes is the
bypass. Then, in the graph:

  timing     impulses every 12000 samples, bypass switched on and off
             again mid-stream: every gap between them stays 12000.
             Measured within one recording, so the graph's own latency,
             whatever it is, drops out.
  crossfade  a tone at the frequency whose half period is the latency the
             filter reports -- the one the old crossfade cancelled
             completely -- stays at its level through both toggles.

usage: bypass.py [BIN...]
"""

import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import Filter, check, fails, run_audio, wait_for_node   # noqa: E402

RATE = 48000
NODE = "bypass-test"

# Each filter as nothing but its delay: no compression, no gating, nothing
# over the ceiling, no cut.
TRANSPARENT = {
    "pw_comp": ["-t", "0", "-R", "1", "-w", "0"],
    "pw_gate": ["-g", "0"],
    "pw_limit": ["-C", "0", "-i", "0"],
    "pw_deess": ["-g", "0"],
}


def start(binpath, look_ms):
    name = os.path.basename(binpath)
    f = Filter([binpath, "-c", "1", "--node-name", NODE, "-k", str(look_ms)]
               + TRANSPARENT[name])
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
    return f, int(dump["latency"])


def toggled(f, seen):
    """Bypass on at 1.6 s, off at 3.1 s; note the latency while bypassed."""
    def go():
        time.sleep(1.6)
        f.send("bypass on")
        time.sleep(0.3)
        f.send("latency")
        end = time.time() + 2
        while time.time() < end:              # replies are matched by name
            l = f.line(0.5, skip_comments=True)
            if l and l.startswith("latency "):
                seen.append(int(l.split()[1]))
                break
        time.sleep(1.2)
        f.send("bypass off")
    t = threading.Thread(target=go, daemon=True)
    t.start()
    return t


def main():
    bins = sys.argv[1:] or ["./bin/" + n for n in TRANSPARENT]
    for binpath in bins:
        name = os.path.basename(binpath)
        print("== %s: bypass keeps the timing ==" % name)

        # timing, at 10 ms of lookahead
        try:
            f, lat = start(binpath, 10)

            def pulses(n, rate):
                x = [0.0] * n
                for k in range(1, int(n / (rate * 0.25))):
                    x[k * rate // 4] = 0.8
                return x

            seen = []
            t = toggled(f, seen)
            mono, _, _ = run_audio(NODE, RATE, 1, [], seconds=5.0, extra_src=pulses)
            t.join(timeout=10)
            f.stop()
            on = [i for i in range(1, len(mono)) if abs(mono[i]) > 0.25 >= abs(mono[i - 1])]
            gaps = [b - a for a, b in zip(on, on[1:])]
            check(len(gaps) >= 12 and all(g == RATE // 4 for g in gaps),
                  "%d impulses, every gap %d samples through bypass on and off"
                  % (len(on), RATE // 4), "gaps %s" % gaps)
            check(seen == [lat], "reports latency %d while bypassed, as it adds" % lat,
                  "got %s" % seen)
        except Exception as e:
            check(False, "timing", repr(e))

        # the crossfade, at the tone the old one cancelled
        try:
            f, lat = start(binpath, 1)
            freq = RATE / (2.0 * lat)
            t = toggled(f, [])
            mono, _, _ = run_audio(NODE, RATE, 1, [freq], seconds=5.0)
            t.join(timeout=10)
            f.stop()
            per = max(1, int(round(RATE / freq)))
            peaks = [max(abs(v) for v in mono[i:i + per])
                     for i in range(0, len(mono) - per, per)]
            full = [i for i, p in enumerate(peaks) if p > 0.29]
            inside = peaks[full[0]:full[-1] + 1]
            check(min(inside) >= 0.29,
                  "a %.0f Hz tone, half a period of its %d-sample latency, holds its "
                  "level through both crossfades (lowest %.3f of 0.300)"
                  % (freq, lat, min(inside)))
        except Exception as e:
            check(False, "crossfade", repr(e))
        print()

    print("bypass: %d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
