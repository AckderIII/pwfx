#!/usr/bin/env python3
"""
sidechain.py -- pw_sccomp in the graph: the gain follows sc_in.

Offline, tests/sccomp.py holds the core to reference/comp.c and the design.
This checks what only the graph can: that the port the framework adds is
there, is the one the detector listens to, and is not passed on.

  ports     in_1, in_2, sc_in, out_1, out_2 -- and nothing else
  keyed     a 1 kHz tone at 0.3 on both audio inputs, a 300 Hz key at 0.3
            (-10.46 dBFS peak) on sc_in; threshold -30, ratio 4, hard knee:
            (30 - 10.46) x 3/4 = 14.66 dB off both channels, and gr says so;
            sc-level reads the key's -10.46
  unheard   the key's 300 Hz is nowhere in the output
  unkeyed   the same audio with nothing linked to sc_in: untouched, and
            sc-level stays at the floor

Every link is made port by port: a node-to-node pw-link would be free to
put the audio on sc_in. Each run gets a filter of its own, as live.py's do:
a node that was left running on a real device's clock -- the key outlives
the audio -- stays on it, and the next run's test streams are then
rate-matched against that device and come out a fraction of a percent off
pitch, which reads as a level change at the probe frequency.

usage: sidechain.py [BIN]
"""

import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live import (Filter, check, fails, find_node, ports_of, pw_dump,   # noqa: E402
                  wait_for_node, write_wav_f32, read_wav_f32, tone_amplitude,
                  db, TIMEOUT)

RATE = 48000
NODE = "sidechain-test"
WANT_GR = (30 + 20 * math.log10(0.3)) * (1 - 1 / 4.0)


def port_names(objs, node, direction):
    n = find_node(objs, node)
    return sorted(p[1] for p in ports_of(objs, n["id"]) if p[0] == direction) if n else []


def play(path, name):
    return subprocess.Popen(
        ["pw-cat", "-p", "--target", "0", "-P", "{ node.name = %s }" % name, path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run(keyed, secs=4.0):
    """Play the audio (and the key, if keyed) through the filter; return the
    two recorded channels."""
    # The last run's streams linger a moment after their pw-cat exits, and a
    # link by node name could land on one of them instead of the new one.
    end = time.time() + TIMEOUT
    while time.time() < end and any(find_node(pw_dump(), w)
                                    for w in ("pwfrec", "pwfsrc", "pwfkey")):
        time.sleep(0.2)
    tmp = tempfile.mkdtemp(prefix="pwf-sc-")
    src, key, dst = (os.path.join(tmp, f) for f in ("in.wav", "key.wav", "out.wav"))
    n = int(RATE * secs)
    a = [0.3 * math.sin(2 * math.pi * 1000 * i / RATE) for i in range(n)]
    write_wav_f32(src, RATE, 2, [v for v in a for _ in range(2)])
    # longer than the audio, so it is still playing whatever the start-up
    write_wav_f32(key, RATE, 1, [0.3 * math.sin(2 * math.pi * 300 * i / RATE)
                                 for i in range(n + 2 * RATE)])

    procs = [subprocess.Popen(
        ["pw-cat", "-r", "--target", "0", "--rate", str(RATE), "--channels", "2",
         "--format", "f32", "-P", "{ node.name = pwfrec }", dst],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    procs.append(play(src, "pwfsrc"))
    if keyed:
        procs.append(play(key, "pwfkey"))
    want = ["pwfrec", "pwfsrc"] + (["pwfkey"] if keyed else [])
    try:
        end = time.time() + TIMEOUT
        while time.time() < end:
            objs = pw_dump()
            if all(find_node(objs, w) and port_names(objs, w, "in" if w == "pwfrec" else "out")
                   for w in want):
                break
            time.sleep(0.2)
        srcp = port_names(objs, "pwfsrc", "out")
        recp = port_names(objs, "pwfrec", "in")
        links = [("pwfsrc:" + srcp[0], NODE + ":in_1"),
                 ("pwfsrc:" + srcp[1], NODE + ":in_2"),
                 (NODE + ":out_1", "pwfrec:" + recp[0]),
                 (NODE + ":out_2", "pwfrec:" + recp[1])]
        if keyed:
            # the key first, so it is already playing when the audio starts
            links.insert(0, ("pwfkey:" + port_names(objs, "pwfkey", "out")[0],
                             NODE + ":sc_in"))
        for a_, b_ in links:
            r = subprocess.run(["pw-link", a_, b_], capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError("pw-link %s %s: %s" % (a_, b_, r.stderr.strip()))
        procs[1].wait(timeout=secs + 30)
        time.sleep(0.5)
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    s = read_wav_f32(dst)
    shutil.rmtree(tmp, ignore_errors=True)
    return s[0::2], s[1::2]


def window(x):
    """Two seconds from 1 s after the audio arrives: past every onset."""
    thr = max((abs(v) for v in x), default=0.0) * 0.2
    start = next((i for i, v in enumerate(x) if abs(v) > thr), 0) + RATE
    return x[start:start + 2 * RATE]


def reports(f):
    """The gr and sc-level lines queued up since the last call."""
    got = {"gr": [], "sc-level": []}
    while True:
        l = f.line(0.2, skip_comments=True)
        if l is None:
            break
        k, _, v = l.partition(" ")
        if k in got:
            got[k].append(float(v))
    return got


def main():
    binpath = sys.argv[1] if len(sys.argv) > 1 else "bin/pw_sccomp"
    for tool in ("pw-cat", "pw-link", "pw-dump"):
        if not shutil.which(tool):
            print("  FAIL %s not available" % tool)
            return 1
    print("== %s: the sidechain in the graph ==" % os.path.basename(binpath))
    for keyed in (True, False):
        f = Filter([binpath, "-c", "2", "--node-name", NODE, "-t", "-30", "-R", "4",
                    "-w", "0", "-a", "1", "-e", "200", "-k", "5"])
        try:
            objs = wait_for_node(NODE)
            if not check(objs is not None, "node is in the graph"):
                continue
            n = find_node(objs, NODE)
            ports = sorted(p[1] for p in ports_of(objs, n["id"]))
            check(ports == ["in_1", "in_2", "out_1", "out_2", "sc_in"],
                  "ports are in_1 in_2 sc_in out_1 out_2", str(ports))
            f.set("meter 50")
            reports(f)                 # drop the startup dump
            l, r = run(keyed)
            got = reports(f)
            grs, scs = got["gr"], got["sc-level"]
            if not check(len(l) > 2 * RATE, "%s: audio came through" %
                         ("keyed" if keyed else "unkeyed"), "%d frames" % len(l)):
                continue
            want = -WANT_GR if keyed else 0.0
            for c, x in (("1", l), ("2", r)):
                w = window(x)
                got = db(tone_amplitude(w, RATE, 1000) / 0.3)
                check(abs(got - want) <= 0.5,
                      "%s: 1 kHz on channel %s at %+.2f dB (want %+.2f)"
                      % ("keyed" if keyed else "unkeyed", c, got, want))
                if keyed:
                    k = db(tone_amplitude(w, RATE, 300) / 0.3)
                    check(k < -90, "unheard: the key's 300 Hz on channel %s "
                          "at %.1f dB" % (c, k))
            # held peaks: the highest reading is the one to trust
            sc = max(scs, default=None)
            if keyed:
                g = max(grs, default=None)
                check(g is not None and abs(g - WANT_GR) <= 0.5,
                      "keyed: gr %s dB (want %.2f, %d reports)"
                      % (g, WANT_GR, len(grs)))
                check(sc is not None and abs(sc - db(0.3)) <= 0.2,
                      "keyed: sc-level %s dBFS, the key's peak (want %.2f, %d reports)"
                      % (sc, db(0.3), len(scs)))
            else:
                check(bool(grs) and max(grs) == 0.0,
                      "unkeyed: gr stays at 0 (highest %s)"
                      % (max(grs) if grs else None))
                check(sc is not None and sc <= -240.0,
                      "unkeyed: sc-level stays at the floor (highest %s)" % sc)
        finally:
            rc = f.stop()
        check(rc == 0, "exits cleanly on 'quit'", "rc=%s" % rc)
    print("%s: %d failure(s)\n" % (os.path.basename(binpath), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
