#!/usr/bin/env python3
"""
live.py -- exercise one pw_* filter against the running PipeWire daemon.

It checks the things only a live run can check:

  * the startup dump on stdout is one "name value" line per parameter,
  * the node and its ports appear in the graph,
  * parameter lines on stdin are accepted, echoed, and bad ones rejected
    without disturbing the filter,
  * audio really flows through it, with the response it promises: a test
    signal is played into the input ports, recorded off the output ports,
    and the level at each probe tone is compared against what the filter's
    settings say it should be.

The audio path is wired by hand with pw-link rather than left to the session
manager: these nodes are plain filters with no media.class, so nothing
auto-connects them, which is also how you would use them for real.

usage:
  live.py BIN [filter-args...]
          [-- "cmd" expected ...]
          [--audio "tone[,tone...]" "freq=db+-tol[,...]"]
          [--peak "db+-tol"] [--meter] [--gr "db+-tol"] [--gain "db+-tol"]

--audio levels are relative to the amplitude of each tone as played, so an
unaffected band reads 0 dB.  --peak is the absolute sample peak of the
recorded window in dBFS, which is how a limiter's ceiling gets checked.
--gr checks the highest gain reduction the filter's "gr" meter reported
during the audio run against the reduction the settings predict; it turns
the meter on by itself. --gain does the same for "gain-applied", signed,
taking the median report: a leveler's gain is not held like gr, it ramps to
where the settings put it, stays there while the tone does, and moves again
for a moment as the stream ends -- the reading that matters is the one it
spends the run at.

--meter turns on in-level/out-level during the audio run and checks them
against the signal played in and the signal recorded out.
"""

import json
import math
import os
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

TIMEOUT = 10.0
fails = []


# ---- talking to the filter ------------------------------------------------

class Filter:
    def __init__(self, argv):
        self.p = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self.out, self.err = queue.Queue(), queue.Queue()
        for f, q in ((self.p.stdout, self.out), (self.p.stderr, self.err)):
            threading.Thread(target=self._pump, args=(f, q), daemon=True).start()

    @staticmethod
    def _pump(f, q):
        for line in f:
            q.put(line.rstrip("\n"))
        q.put(None)

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def line(self, timeout=TIMEOUT, skip_comments=False):
        """One stdout line.  #-comments are protocol noise -- a state change
        can land at any moment -- so a caller waiting for a parameter echo
        skips them instead of mistaking one for the answer."""
        end = time.time() + timeout
        while True:
            try:
                l = self.out.get(timeout=max(0.0, end - time.time()))
            except queue.Empty:
                return None
            if skip_comments and l is not None and l.startswith("#"):
                continue
            return l

    def errline(self, timeout=1.0):
        try:
            return self.err.get(timeout=timeout)
        except queue.Empty:
            return None

    def set(self, line):
        """Send a setting and wait for its echo, so the next step cannot race
        a parameter that has not been applied yet.

        A change can legitimately produce more than one line: moving the
        lookahead moves the reported latency with it, and the filter says so.
        So wait for the line that names the parameter actually being set and
        let the others past."""
        want = line.split()[0].split("=")[0].split(":")[0]
        self.send(line)
        end = time.time() + 3.0
        extra = []
        while time.time() < end:
            l = self.line(max(0.1, end - time.time()), skip_comments=True)
            if l is None:
                break
            if l.split(" ")[0] == want:
                self.extra = extra
                return l
            extra.append(l)
        self.extra = extra
        return None

    def stop(self):
        try:
            self.send("quit")
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()
            self.p.wait(timeout=5)
        return self.p.returncode


def check(ok, what, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + (("  -- " + detail) if detail else ""))
    if not ok:
        fails.append(what)
    return ok


# ---- the graph ------------------------------------------------------------

def pw_dump():
    try:
        return json.loads(subprocess.run(
            ["pw-dump"], capture_output=True, text=True, timeout=15).stdout)
    except Exception:
        return []


def find_node(objs, name):
    for o in objs:
        if o.get("type") == "PipeWire:Interface:Node" and \
           o.get("info", {}).get("props", {}).get("node.name") == name:
            return o
    return None


def ports_of(objs, node_id):
    out = []
    for o in objs:
        if o.get("type") != "PipeWire:Interface:Port":
            continue
        pr = o.get("info", {}).get("props", {})
        if pr.get("node.id") == node_id:
            out.append((pr.get("port.direction"), pr.get("port.name")))
    return out


def node_latency_ns(objs, name):
    """The ProcessLatency the node publishes, as the graph sees it."""
    n = find_node(objs, name)
    if not n:
        return None
    for p in n.get("info", {}).get("params", {}).get("ProcessLatency", []):
        if isinstance(p, dict):
            return p
    return None


def wait_for_node(name, timeout=TIMEOUT):
    end = time.time() + timeout
    while time.time() < end:
        objs = pw_dump()
        if find_node(objs, name):
            return objs
        time.sleep(0.3)
    return None


# ---- signals --------------------------------------------------------------

def write_wav_f32(path, rate, ch, samples):
    data = struct.pack("<%df" % len(samples), *samples)
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
           struct.pack("<IHHIIHH", 16, 3, ch, rate, rate * ch * 4, ch * 4, 32) +
           b"data" + struct.pack("<I", len(data)))
    open(path, "wb").write(hdr + data)


def read_wav_f32(path):
    b = open(path, "rb").read()
    i = b.find(b"data")
    if i < 0:
        return []
    body = b[i + 8:]
    return list(struct.unpack("<%df" % (len(body) // 4), body[:len(body) // 4 * 4]))


def tone_amplitude(x, rate, f):
    """Amplitude of the component at f, Hann-windowed so a deep notch is not
    swamped by leakage from a loud neighbouring tone."""
    n = len(x)
    if n < 64:
        return 0.0
    re = im = ws = 0.0
    for i in range(n):
        w = 0.5 - 0.5 * math.cos(2 * math.pi * i / n)
        a = 2 * math.pi * f * i / rate
        re += w * x[i] * math.cos(a)
        im -= w * x[i] * math.sin(a)
        ws += w
    return 2.0 * math.hypot(re, im) / ws


def db(x):
    return 20 * math.log10(x) if x > 1e-12 else -240.0


def run_audio(node_name, rate, ch, tones, seconds=4.0, amp=0.3, extra_src=None,
              out_node=None):
    """Play a sum of tones into the filter, record what comes out, and return
    the recorded samples of channel 0, the raw interleaved stream, and the
    peak of the signal that was played, in dBFS.

    out_node taps a different node than the one being fed, which is how a
    chain of several filters gets measured end to end."""
    for tool in ("pw-cat", "pw-link"):
        if not shutil.which(tool):
            raise RuntimeError(tool + " not available")

    tmp = tempfile.mkdtemp(prefix="pwf-")
    src, dst = os.path.join(tmp, "in.wav"), os.path.join(tmp, "out.wav")
    n = int(rate * seconds)
    if extra_src is not None:
        frames = extra_src(n, rate)
    else:
        frames = [sum(amp * math.sin(2 * math.pi * f * i / rate) for f in tones)
                  for i in range(n)]
    write_wav_f32(src, rate, ch, [v for v in frames for _ in range(ch)])

    rec = subprocess.Popen(
        ["pw-cat", "-r", "--target", "0", "--rate", str(rate),
         "--channels", str(ch), "--format", "f32",
         "-P", "{ node.name = pwfrec }", dst],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    play = subprocess.Popen(
        ["pw-cat", "-p", "--target", "0", "-P", "{ node.name = pwfsrc }", src],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # both stream nodes have to exist before they can be linked
        end = time.time() + TIMEOUT
        while time.time() < end:
            objs = pw_dump()
            if find_node(objs, "pwfsrc") and find_node(objs, "pwfrec"):
                break
            time.sleep(0.2)
        for a, b in (("pwfsrc", node_name), (out_node or node_name, "pwfrec")):
            subprocess.run(["pw-link", a, b], capture_output=True, text=True)
        play.wait(timeout=seconds + 30)
        time.sleep(0.5)
    finally:
        for p in (rec, play):
            if p.poll() is None:
                p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()

    s = read_wav_f32(dst)
    shutil.rmtree(tmp, ignore_errors=True)
    return s[0::ch], s, db(max(abs(v) for v in frames))


def steady_window(x, rate, want=1.0, skip=0.5):
    """A window of the recording that is past the start-up transient."""
    thr = max((abs(v) for v in x), default=0.0) * 0.2
    start = 0
    for i, v in enumerate(x):
        if abs(v) > thr:
            start = i
            break
    start += int(skip * rate)
    n = int(want * rate)
    if start + n > len(x):
        start = max(0, len(x) - n)
    return x[start:start + n]


# ---- main -----------------------------------------------------------------

def parse_expect(spec):
    """"200=0+-0.5,8000=-72+-6" -> [(200.0, 0.0, 0.5), (8000.0, -72.0, 6.0)]"""
    out = []
    for part in spec.split(","):
        f, _, rest = part.partition("=")
        val, _, tol = rest.partition("+-")
        out.append((float(f), float(val), float(tol or 1.0)))
    return out


def main():
    argv = sys.argv[1:]
    audio = peak_spec = None
    meter = "--meter" in argv
    if meter:
        argv.remove("--meter")
    gr_spec = None
    if "--gr" in argv:
        i = argv.index("--gr")
        gr_spec = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    gain_spec = None
    if "--gain" in argv:
        i = argv.index("--gain")
        gain_spec = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if "--peak" in argv:
        i = argv.index("--peak")
        peak_spec = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if "--audio" in argv:
        i = argv.index("--audio")
        audio = (argv[i + 1], argv[i + 2])
        argv = argv[:i] + argv[i + 3:]
    if "--" in argv:
        i = argv.index("--")
        binargs, rest = argv[:i], argv[i + 1:]
    else:
        binargs, rest = argv, []
    pairs = [(rest[i], rest[i + 1]) for i in range(0, len(rest) - 1, 2)]

    binpath = binargs[0]
    name = os.path.basename(binpath) + "-test"
    print("== %s ==" % os.path.basename(binpath))
    f = Filter(binargs + ["--node-name", name])

    # 1. startup dump
    params = {}
    end = time.time() + TIMEOUT
    while time.time() < end:
        l = f.line()
        if l is None or l.startswith("#"):
            break
        k, _, v = l.partition(" ")
        params[k] = v
    check(len(params) >= 8, "startup dump has %d parameters" % len(params))
    check(all(k in params for k in ("rate", "channels", "bypass", "latency")),
          "dump includes the core parameters")

    # 2. node and ports
    objs = wait_for_node(name)
    if check(objs is not None, "node '%s' is in the graph" % name):
        node = find_node(objs, name)
        ports = ports_of(objs, node["id"])
        nch = int(params.get("channels", "2"))
        # a sidechain input is one more, mono, and not one of the channels
        nin = len([p for p in ports if p[0] == "in" and p[1] != "sc_in"])
        nout = len([p for p in ports if p[0] == "out"])
        check(nin == nch and nout == nch, "%d in / %d out ports" % (nin, nout),
              str(sorted(p[1] for p in ports)))
        want_lat = int(params.get("latency", "0"))
        lat = node_latency_ns(objs, name)
        check(lat is not None and int(lat.get("rate", -1)) == want_lat,
              "reports %d samples of latency to the graph" % want_lat, str(lat))

    # 3. live parameter changes
    for cmd, want in pairs:
        if want == "ERROR":
            f.send(cmd)
            got = f.errline(2.0)
            check(got is not None and got.startswith("error:"),
                  "rejected: %s" % cmd, str(got))
        else:
            got = f.set(cmd)
            detail = "got %r" % got
            if getattr(f, "extra", None):
                detail += " (also reported %s)" % ", ".join(f.extra)
            check(got == want, "%-30s -> %s" % (cmd, want), detail)

    # 4. the audio path and the response it promises
    if audio:
        rate = int(params.get("rate", "48000"))
        ch = int(params.get("channels", "2"))
        tones = [float(t) for t in audio[0].split(",")]
        expect = parse_expect(audio[1])
        try:
            metering = meter or gr_spec or gain_spec
            if metering:
                f.set("meter 50")
            mono, _, src_db = run_audio(name, rate, ch, tones)
            levels = {"in-level": [], "out-level": [], "gr": [], "gain-applied": []}
            if metering:
                # the reader thread kept draining while audio ran, so the
                # reports are queued up waiting
                while True:
                    l = f.line(0.2, skip_comments=True)
                    if l is None:
                        break
                    k, _, v = l.partition(" ")
                    if k in levels:
                        levels[k].append(float(v))
                f.set("meter 0")
                for l in getattr(f, "extra", []):
                    k, _, v = l.partition(" ")
                    if k in levels:
                        levels[k].append(float(v))
            if not mono:
                check(False, "audio passes through the filter", "nothing recorded")
            else:
                w = steady_window(mono, rate)
                ref = 0.3
                if peak_spec:
                    want_db, _, tol = peak_spec.partition("+-")
                    got = db(max(abs(v) for v in w))
                    check(abs(got - float(want_db)) <= float(tol or 1.0),
                          "sample peak %+.3f dBFS (want %+.3f +- %s)"
                          % (got, float(want_db), tol or "1.0"))
                for freq, want_db, tol in expect:
                    got_db = db(tone_amplitude(w, rate, freq) / ref)
                    check(abs(got_db - want_db) <= tol,
                          "%7.0f Hz: %+7.2f dB (want %+.2f +- %.2f)"
                          % (freq, got_db, want_db, tol))
            if meter:
                # A peak meter falls back between reports, so any single
                # reading can sit under the true peak; the highest reading
                # over the run is the one that has to match.
                got_in = max(levels["in-level"], default=-240.0)
                got_out = max(levels["out-level"], default=-240.0)
                rec_db = db(max(abs(v) for v in mono)) if mono else -240.0
                check(len(levels["in-level"]) > 5,
                      "meter reported %d in-level / %d out-level lines"
                      % (len(levels["in-level"]), len(levels["out-level"])))
                check(abs(got_in - src_db) <= 1.0,
                      "in-level %+.1f dBFS matches the signal played in "
                      "(%+.2f)" % (got_in, src_db))
                check(abs(got_out - rec_db) <= 1.0,
                      "out-level %+.1f dBFS matches the signal recorded out "
                      "(%+.2f)" % (got_out, rec_db))
            if gr_spec:
                # Held like the levels, so the highest reading is the one
                # to trust; the reduction is steady once the tone is.
                want, _, tol = gr_spec.partition("+-")
                got = max(levels["gr"], default=None)
                check(got is not None and abs(got - float(want)) <= float(tol or 1.0),
                      "gr %s dB matches the reduction the settings predict "
                      "(want %s +- %s, %d reports)"
                      % (got, want, tol or "1.0", len(levels["gr"])))
            if gain_spec:
                want, _, tol = gain_spec.partition("+-")
                got = sorted(levels["gain-applied"])[len(levels["gain-applied"]) // 2] \
                    if levels["gain-applied"] else None
                check(got is not None and abs(got - float(want)) <= float(tol or 1.0),
                      "gain-applied %s dB matches the gain the settings predict "
                      "(want %s +- %s, %d reports)"
                      % (got, want, tol or "1.0", len(levels["gain-applied"])))
        except Exception as e:
            check(False, "audio path", repr(e))

    # 5. clean exit
    rc = f.stop()
    check(rc == 0, "exits cleanly on 'quit'", "rc=%s" % rc)

    print("%s: %d failure(s)\n" % (os.path.basename(binpath), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
