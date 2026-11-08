#!/usr/bin/env python3
"""
sccomp.py -- the sidechain compressor's core, offline: against
reference/comp.c to the bit, and against the design where the reference has
nothing to say.

pw_sccomp has no reference of its own, because there was nothing new to
port: it is pw_comp's core with the detector fed from a key. The original
can be made to hear exactly that key, though, so bin/t_sccomp (the code the
filter runs) is held byte for byte against bin/ref/comp on two kinds of
stream:

  self      mono, the key the audio itself: the plain compressor, linked by
            max over its one channel. Every setting, sidechain filters
            included, since the key goes through them as the audio would.
  wide      N channels whose every sample is the key times a factor in
            [-1, 1], played to the reference as N + 1 channels with the key
            first and linked by max: its loudest channel is then always the
            key, so its one gain is the key's, applied to every channel --
            which is what pw_sccomp claims to do. The key's channel is
            dropped from the reference's output. No sidechain filters here:
            filtered, the key is no longer guaranteed the loudest.

And against the design, with the key and the audio different signals:

  silent    no key, no makeup: the audio comes out bit for bit
  below     a key under the knee: likewise bit for bit
  level     a key tone at L dBFS peak, hard knee, threshold T, ratio R:
            the audio is cut by (L - T)(1 - 1/R), whatever its own level
  release   the cut after the key stops falls as exp(-t / release)
  ahead     the gain moves `lookahead` samples before the audio that sits
            beside the key's onset reaches the output

Prints ok/FAIL lines and exits with the number of failures.
"""

import math
import os
import random
import struct
import subprocess
import sys

RATE = 48000
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "bin")
fails = 0


def check(ok, what, detail=""):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what +
          (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        fails += 1


def f32(xs):
    """Round to float32, as the stream will carry them."""
    return list(struct.unpack("<%df" % len(xs), struct.pack("<%df" % len(xs), *xs)))


def pack(frames):
    flat = [v for fr in frames for v in fr]
    return struct.pack("<%df" % len(flat), *flat)


def unpack(b, ch):
    v = struct.unpack("<%df" % (len(b) // 4), b)
    return [v[i:i + ch] for i in range(0, len(v), ch)]


def run(prog, args, data):
    r = subprocess.run([os.path.join(BIN, prog)] + args, input=data,
                       capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("%s %s: rc %d %s" % (prog, " ".join(args),
                                                r.returncode, r.stderr))
    return r.stdout


def sccomp(args, audio, key):
    """audio: list of frames (tuples), key: list of samples."""
    ch = len(audio[0])
    out = run("t_sccomp", ["-r", str(RATE), "-c", str(ch)] + args,
              pack([tuple(a) + (k,) for a, k in zip(audio, key)]))
    return unpack(out, ch)


def gen_mono():
    """Channel 0 of the stream every other comparison uses."""
    raw = subprocess.run([sys.executable, os.path.join(HERE, "gen.py"),
                          str(RATE), "1", "4"], capture_output=True).stdout
    return list(struct.unpack("<%df" % (len(raw) // 4), raw))


# ---- against the reference ----------------------------------------------

def self_case(x, args):
    ref = run(os.path.join("ref", "comp"),
              ["-r", str(RATE), "-c", "1", "-l", "max"] + args, pack([(v,) for v in x]))
    new = run("t_sccomp", ["-r", str(RATE), "-c", "1"] + args,
              pack([(v, v) for v in x]))
    check(ref == new, "self  %s" % " ".join(args),
          "%d vs %d bytes, differ at %s" % (len(ref), len(new),
          next((i // 4 for i in range(min(len(ref), len(new)))
                if ref[i] != new[i]), "the end")))


def wide_case(x, ch, args, seed):
    rnd = random.Random(seed)
    audio = []
    for k in x:
        audio.append(tuple(f32([k * rnd.uniform(-1.0, 1.0) for _ in range(ch)])))
    # |round(k r)| <= |k| for |r| <= 1, since k is a float32 already and
    # rounding is monotone: the key stays the loudest channel, exactly.
    assert all(abs(a) <= abs(k) for fr, k in zip(audio, x) for a in fr)
    ref = run(os.path.join("ref", "comp"),
              ["-r", str(RATE), "-c", str(ch + 1), "-l", "max"] + args,
              pack([(k,) + fr for k, fr in zip(x, audio)]))
    want = pack([fr[1:] for fr in unpack(ref, ch + 1)])
    got = run("t_sccomp", ["-r", str(RATE), "-c", str(ch)] + args,
              pack([fr + (k,) for k, fr in zip(x, audio)]))
    check(want == got, "wide  -c %d %s" % (ch, " ".join(args)),
          "%d vs %d bytes" % (len(want), len(got)))


# ---- against the design --------------------------------------------------

def amplitude(x, f):
    """Amplitude of the tone at f, over a whole number of its periods."""
    per = RATE / f
    n = int(int(len(x) / per) * per)
    re = sum(v * math.cos(2 * math.pi * f * i / RATE) for i, v in enumerate(x[:n]))
    im = sum(v * math.sin(2 * math.pi * f * i / RATE) for i, v in enumerate(x[:n]))
    return 2 * math.hypot(re, im) / n


def tone(f, amp, n):
    return f32([amp * math.sin(2 * math.pi * f * i / RATE) for i in range(n)])


def design_checks():
    n = RATE  # one second
    audio = [(a, 0.7 * a) for a in tone(1000, 0.3, n)]

    out = sccomp([], audio, [0.0] * n)
    check(out == [tuple(f32(fr)) for fr in audio],
          "silent: no key passes the audio bit for bit")

    # threshold -30, knee 6: identity below -33; a key at -40 never reaches it
    out = sccomp(["-t", "-30", "-w", "6"], audio, tone(300, 0.01, n))
    check(out == [tuple(f32(fr)) for fr in audio],
          "below: a key under the knee passes the audio bit for bit")

    # A 300 Hz key at 0.3 peak, hard knee: peak detection with an instant
    # attack and a long release holds the cut at the key's peak to within
    # the release's droop over half a period, a few thousandths of a dB.
    lk = 20 * math.log10(0.3)
    want = -(lk + 30) * (1 - 1 / 4)
    args = ["-t", "-30", "-R", "4", "-w", "0", "-a", "0", "-e", "2000"]
    key = tone(300, 0.3, n)
    for amp in (0.3, 0.001):
        a = tone(1000, amp, n)
        out = sccomp(args, [(v, 0.5 * v) for v in a], key)
        tail = slice(n // 2, n)
        for c, scale in ((0, 1.0), (1, 0.5)):
            got = 20 * math.log10(amplitude([fr[c] for fr in out[tail]], 1000)
                                  / (amp * scale))
            check(abs(got - want) < 0.02,
                  "level: audio at %g, channel %d cut %.3f dB by the key "
                  "(want %.3f)" % (amp, c + 1, -got, -want))

    # Release, on DC audio so the gain is readable sample by sample. The key
    # stops at n1; from then on the detector only decays, in dB, by aR per
    # sample -- exactly, since nothing new is above threshold.
    rel = 100.0
    n1 = n // 2
    key = tone(300, 0.3, n1) + [0.0] * (n - n1)
    out = sccomp(["-t", "-30", "-R", "4", "-w", "0", "-a", "0", "-e",
                  str(rel), "-k", "0"], [(0.1,)] * n, key)
    gr = [-20 * math.log10(fr[0] / f32([0.1])[0]) for fr in out]
    m = int(rel * RATE / 1000)
    ratio = gr[n1 + m] / gr[n1]
    want = math.exp(-1000.0 / (rel * RATE)) ** m
    check(abs(ratio / want - 1) < 1e-3,
          "release: %g ms later the cut is %.5f of what it was (want %.5f)"
          % (rel, ratio, want))

    # Lookahead: the key's onset at n0 cuts the audio from n0 - L on. The
    # harness absorbs the delay as the reference does, so the audio beside
    # the key is where it went in, and the gain arrives L samples early.
    look = 5.0
    L = int(look * RATE / 1000 + 0.5)
    n0 = n // 2
    key = [0.0] * n0 + [0.5] * (n - n0)
    out = sccomp(["-t", "-30", "-R", "4", "-w", "0", "-a", "0", "-k", str(look)],
                 [(0.1,)] * n, key)
    first = next(i for i, fr in enumerate(out) if fr[0] != f32([0.1])[0])
    check(first == n0 - L,
          "ahead: the cut starts %d samples before the key (want %d)"
          % (n0 - first, L))


def main():
    x = gen_mono()

    for args in ([], ["-b", "1"],
                 ["-t", "-18", "-R", "3", "-a", "10", "-e", "120", "-w", "6", "-m", "6"],
                 ["-b", "97", "-t", "-24", "-R", "2", "-a", "25", "-e", "250", "-w", "10", "-M"],
                 ["-b", "256", "-t", "-30", "-R", "20", "-w", "0", "-k", "0", "-a", "0", "-e", "0"],
                 ["-b", "333", "-t", "-12", "-R", "4", "-q", "30", "-F", "120"],
                 ["-b", "512", "-t", "-12", "-R", "4", "-q", "30", "-G", "8000"],
                 ["-b", "1024", "-t", "-40", "-R", "8", "-k", "50", "-m", "-6",
                  "-F", "150", "-G", "6000"]):
        self_case(x, args)

    for ch, args in ((2, []),
                     (2, ["-b", "97", "-t", "-24", "-R", "2", "-a", "25", "-e", "250",
                          "-w", "10", "-M"]),
                     (2, ["-b", "333", "-t", "-12", "-R", "4", "-q", "30", "-k", "20"]),
                     (5, ["-b", "1024", "-t", "-40", "-R", "8", "-k", "50", "-m", "-6"])):
        wide_case(x, ch, args, seed=ch * 1000 + len(args))

    design_checks()
    return fails


if __name__ == "__main__":
    sys.exit(main())
