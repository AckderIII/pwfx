#!/usr/bin/env python3
"""
chain.py -- three filters wired in series in the live graph, which is how
they are meant to be used and the only way to check that they compose:
ports link up, latency adds up, and the limiter's ceiling still holds at the
end of the chain rather than only on its own.

  pw_hpf (200 Hz, 4th order) -> pw_comp (-30 dB, 4:1) -> pw_limit (-1 dBTP)

A 60 Hz tone and a 1 kHz tone go in. The 60 Hz one is there to prove the
first stage is really in the path; the 1 kHz one drives the compressor and
then the limiter, and what comes out has to sit exactly on the ceiling.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import (Filter, check, fails, wait_for_node, run_audio,     # noqa
                  tone_amplitude, steady_window, db, pw_dump, find_node)

STAGES = [
    ("chain-hpf",   ["./bin/pw_hpf", "-c", "2", "-f", "200", "-n", "4"]),
    ("chain-comp",  ["./bin/pw_comp", "-c", "2", "-t", "-30", "-R", "4",
                     "-w", "0", "-a", "1", "-e", "200", "-k", "5"]),
    ("chain-limit", ["./bin/pw_limit", "-c", "2", "-C", "-1", "-i", "30",
                     "-k", "5"]),
]


def main():
    print("== three filters in series ==")
    procs, latency = [], {}
    for name, argv in STAGES:
        f = Filter(argv + ["--node-name", name])
        while True:
            l = f.line(10.0)
            if l is None or l.startswith("#"):
                break
            k, _, v = l.partition(" ")
            if k == "latency":
                latency[name] = int(v)
        procs.append(f)

    try:
        for name, _ in STAGES:
            check(wait_for_node(name) is not None, "%s is in the graph" % name)

        for a, b in zip([s[0] for s in STAGES], [s[0] for s in STAGES][1:]):
            r = subprocess.run(["pw-link", a, b], capture_output=True,
                               text=True)
            check(r.returncode == 0, "link %s -> %s" % (a, b),
                  r.stderr.strip())

        # each stage reports its own latency; the chain's is the sum
        check(latency.get("chain-hpf") == 0, "pw_hpf adds no latency",
              str(latency))
        check(latency.get("chain-comp") == 240 and
              latency.get("chain-limit") == 252,
              "lookahead stages report %d + %d samples"
              % (latency.get("chain-comp", -1), latency.get("chain-limit", -1)))

        mono, _ = run_audio("chain-hpf", 48000, 2, [60, 1000], seconds=4.0,
                            out_node="chain-limit")
        check(len(mono) > 1000, "audio traverses all three stages",
              "%d frames" % len(mono))
        if mono:
            w = steady_window(mono, 48000)
            peak = db(max(abs(v) for v in w))
            got_1k = db(tone_amplitude(w, 48000, 1000) / 0.3)
            got_60 = db(tone_amplitude(w, 48000, 60) / 0.3)

            # -10.46 dBFS in, 14.66 dB of compression, +30 dB of input gain
            # into the limiter puts it well over the ceiling, so it lands on
            # the ceiling: -1 dBFS, which is +9.46 dB on a 0.3 reference.
            check(abs(got_1k - 9.46) <= 0.4,
                  "1000 Hz: %+.2f dB (want +9.46)" % got_1k)
            check(abs(peak + 1.0) <= 0.25,
                  "sample peak %+.3f dBFS -- the ceiling holds through the "
                  "whole chain" % peak)
            # the high-pass really is in the path; the exact figure is not
            # meaningful because two time-varying gain stages follow it
            check(got_60 < got_1k - 30,
                  "60 Hz is %.1f dB below 1 kHz, so pw_hpf is in the path"
                  % (got_1k - got_60))
    finally:
        for f in procs:
            f.stop()

    print("chain: %d failure(s)\n" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
