#!/usr/bin/env python3
"""
stress.py -- hammer a running filter's parameters while audio flows through
it, to exercise the part that cannot be checked offline: the handoff of
freshly designed coefficients from the main loop to the realtime thread.

Parameters are discovered from the filter's own --help, so this stays correct
as filters gain settings. Random values are drawn across each declared range,
including combinations the filter is expected to reject; a rejection is a
pass, a crash or a NaN in the output is not.

usage: stress.py BIN [filter-args...]
"""

import os
import random
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import Filter, run_audio, wait_for_node, db     # noqa: E402

ROUNDS = 400


def discover(binpath):
    """(name, kind, spec) for every live-settable parameter, from --help."""
    txt = subprocess.run([binpath, "--help"], capture_output=True,
                         text=True).stdout.splitlines()
    out, pending = [], None
    for line in txt:
        m = re.match(r"\s+(?:-\w,\s+)?--([\w-]+)\s", line)
        if m:
            pending = m.group(1)
            continue
        if pending is None:
            continue
        info = line.strip()
        if "[read-only]" in info or "[command line only]" in info:
            pending = None
            continue
        rng = re.match(r"\(([-\d.e+]+)\.\.([-\d.e+]+)", info)
        enum = re.match(r"\(([a-z0-9|]+), default", info)
        if rng:
            out.append((pending, "num", (float(rng.group(1)),
                                         float(rng.group(2)))))
        elif enum and "|" in enum.group(1):
            out.append((pending, "enum", enum.group(1).split("|")))
        elif info.startswith("(default o"):
            out.append((pending, "bool", None))
        pending = None
    return out


def main():
    binargs = sys.argv[1:]
    binpath = binargs[0]
    name = os.path.basename(binpath) + "-stress"
    params = discover(binpath)
    print("== %s == %d live parameters: %s"
          % (os.path.basename(binpath), len(params),
             ", ".join(p[0] for p in params)))

    f = Filter(binargs + ["--node-name", name])
    while True:                                   # consume the startup dump
        l = f.line(10.0)
        if l is None or l.startswith("#"):
            break

    if wait_for_node(name) is None:
        print("  FAIL node never appeared")
        return 1

    random.seed(20260921)
    sent = [0]
    stop = threading.Event()

    def hammer():
        while not stop.is_set() and sent[0] < ROUNDS:
            nm, kind, spec = random.choice(params)
            if kind == "num":
                lo, hi = spec
                # mostly in range, sometimes past it, to exercise rejection
                v = random.uniform(lo, hi) if random.random() < 0.85 \
                    else random.uniform(lo - abs(lo) - 10, hi + abs(hi) + 10)
                f.send("%s %.4f" % (nm, v))
            elif kind == "enum":
                f.send("%s %s" % (nm, random.choice(spec)))
            else:
                f.send("%s %s" % (nm, random.choice(("on", "off", "toggle"))))
            sent[0] += 1
            time.sleep(0.004)

    t = threading.Thread(target=hammer, daemon=True)
    t.start()
    try:
        mono, _, _ = run_audio(name, 48000, 2, [220, 3000], seconds=3.0)
    finally:
        stop.set()
        t.join(timeout=5)

    alive = f.p.poll() is None
    bad = [v for v in mono if v != v or abs(v) == float("inf")]
    loud = max((abs(v) for v in mono), default=0.0)

    ok = True
    for cond, what, detail in (
            (alive, "survives %d parameter changes under load" % sent[0], ""),
            (not bad, "no NaN or Inf in the output",
             "%d bad samples" % len(bad)),
            (len(mono) > 1000, "audio kept flowing",
             "%d frames recorded" % len(mono)),
            (loud < 100.0, "output stayed bounded", "peak %.3f" % loud)):
        print(("  ok   " if cond else "  FAIL ") + what +
              (("  -- " + detail) if detail and not cond else ""))
        ok = ok and cond

    if alive:
        rc = f.stop()
        print(("  ok   " if rc == 0 else "  FAIL ") +
              "exits cleanly  -- rc=%s" % rc)
        ok = ok and rc == 0
    print()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
