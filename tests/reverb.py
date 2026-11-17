#!/usr/bin/env python3
"""
reverb.py -- the reverb core against its design, offline, through
bin/t_reverb, the code the filter runs.

The reference comparison in run.sh proves the core and reference/reverb.c
agree; this proves what they agree on is a reverb that does what the
documentation says. Every expectation comes from acoustics or statistics,
not from the C:

  decay      the impulse response's energy decay curve (Schroeder's
             backward integral), fitted from -5 to -25 dB, falls 60 dB in
             decay seconds in a band well below damp-freq and in
             decay * hf-decay well above it -- at every size, with and
             without modulation, at 44.1 and 48 kHz.
  density    past the first 100 ms the tail is as dense as Gaussian noise:
             Abel and Huang's normalised echo density, the share of samples
             beyond one standard deviation over erfc(1/sqrt 2), is near 1.
             A sparse or metallic tail reads far under it.
  width      one source in both channels comes out of them decorrelated.
  predelay   nothing at all comes out before the predelay, and the first
             echoes within the diffuser's span after it.
  dry        at mix 0 the output is the input, bit for bit.
  glide      changing size or predelay while a tone plays does not click:
             a tone has nothing above 3 kHz and a click everything, so the
             output above 3 kHz stays 60 dB under the tone.
  blocks     the output does not depend on the block size.
  stability  the longest, brightest, most modulated setting decays.

Prints ok/FAIL lines and exits with the number of failures.
"""

import math
import random
import struct
import subprocess
import sys

fails = 0


def check(ok, what, detail=""):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what + (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        fails += 1


def run(frames, rate, ch, *args, raw=False):
    data = struct.pack("<%df" % len(frames), *frames)
    out = subprocess.run(["./bin/t_reverb", "-r", str(rate), "-c", str(ch), *args],
                         input=data, capture_output=True).stdout
    if raw:
        return out
    return struct.unpack("<%df" % (len(out) // 4), out)


def impulse(rate, secs, ch=1, *args):
    n = int(rate * secs)
    x = [0.0] * (n * ch)
    for c in range(ch):
        x[c] = 1.0
    y = run(x, rate, ch, "-m", "100", *args)
    return [y[i::ch] for i in range(ch)]


def biquad(x, b0, b1, b2, a1, a2):
    s1 = s2 = 0.0
    y = []
    for v in x:
        o = b0 * v + s1
        s1 = b1 * v - a1 * o + s2
        s2 = b2 * v - a2 * o
        y.append(o)
    return y


def butter4(x, fc, rate, high):
    """4th-order Butterworth, two RBJ sections at its two Qs."""
    for q in (0.5411961, 1.3065630):
        w = 2 * math.pi * fc / rate
        al = math.sin(w) / (2 * q)
        cw = math.cos(w)
        g = (1 + cw) / 2 if high else (1 - cw) / 2
        a0 = 1 + al
        b1 = -2 * g if high else 2 * g
        x = biquad(x, g / a0, b1 / a0, g / a0, -2 * cw / a0, (1 - al) / a0)
    return x


def rt60(y, rate, lo=-5.0, hi=-25.0):
    """Schroeder backward integration, a least-squares line from lo to hi
    dB, extrapolated to 60 dB."""
    e, acc = [0.0] * len(y), 0.0
    for i in range(len(y) - 1, -1, -1):
        acc += y[i] * y[i]
        e[i] = acc
    pts = [(i / rate, 10 * math.log10(e[i] / e[0])) for i in range(0, len(y), 16)
           if e[i] > 0 and hi <= 10 * math.log10(e[i] / e[0]) <= lo]
    n = len(pts)
    mt = sum(t for t, _ in pts) / n
    md = sum(d for _, d in pts) / n
    slope = sum((t - mt) * (d - md) for t, d in pts) / sum((t - mt) ** 2 for t, _ in pts)
    return -60.0 / slope


# -- decay, both ends of the band --------------------------------------------
for decay, hf, damp, size, mod, rate in ((2.0, 50, 1000, 70, 0, 48000),
                                         (2.0, 50, 1000, 70, 30, 48000),
                                         (0.6, 100, 2000, 15, 30, 48000),
                                         (4.0, 25, 800, 100, 60, 44100)):
    lo_t, hi_t = decay, decay * hf / 100.0
    secs = 1.0 * max(lo_t, hi_t) + 0.4
    (y,) = impulse(rate, secs, 1, "-d", str(decay), "-H", str(hf), "-F", str(damp),
                   "-s", str(size), "-M", str(mod), "-p", "0")
    t_lo = rt60(butter4(y, damp / 8.0, rate, False), rate)
    t_hi = rt60(butter4(y, rate / 3.0, rate, True), rate)
    what = "decay %g hf %g%% damp %g size %g mod %g at %g Hz" % (decay, hf, damp, size, mod, rate)
    check(abs(t_lo / lo_t - 1) < 0.06,
          "%s: RT60 %.3f s below damp-freq (want %.3f)" % (what, t_lo, lo_t))
    check(abs(t_hi / hi_t - 1) < 0.08,
          "%s: RT60 %.3f s above it (want %.3f)" % (what, t_hi, hi_t))

# -- echo density -------------------------------------------------------------
ERFC1 = math.erfc(1 / math.sqrt(2))
for size, mod in ((70, 0), (70, 30), (20, 30), (100, 0)):
    rate = 48000
    (y,) = impulse(rate, 0.6, 1, "-d", "3", "-H", "100", "-s", str(size), "-M", str(mod),
                   "-p", "0")
    win = int(0.02 * rate)
    neds = []
    for a in range(int(0.1 * rate), int(0.5 * rate), win):
        seg = y[a:a + win]
        sd = math.sqrt(sum(v * v for v in seg) / win)
        neds.append(sum(1 for v in seg if abs(v) > sd) / win / ERFC1)
    m = sum(neds) / len(neds)
    check(m > 0.9, "size %g mod %g: echo density %.3f of Gaussian noise's, 100-500 ms"
          % (size, mod, m))

# -- width: one source in both channels, two tails ----------------------------
rate = 48000
L, R = impulse(rate, 1.2, 2, "-d", "2", "-p", "0")
a, b = int(0.05 * rate), int(1.2 * rate)
num = sum(L[i] * R[i] for i in range(a, b))
den = math.sqrt(sum(v * v for v in L[a:b]) * sum(v * v for v in R[a:b]))
check(abs(num / den) < 0.1, "a centred source's tails correlate %.3f between the channels"
      % (num / den))

# -- predelay -----------------------------------------------------------------
for pd, size in ((0, 70), (37.5, 70), (120, 30), (250, 100)):
    (y,) = impulse(rate, 0.5, 1, "-p", str(pd), "-s", str(size))
    P = int(pd * rate / 1000 + 0.5)
    first = next(i for i, v in enumerate(y) if v != 0.0)
    span = int(size / 100.0 * 40e-3 * rate / 15.0) + 1     # the first step's
    check(first >= P and first <= P + span,
          "predelay %g ms: silence for %d samples, the first echo at %d (by %d)"
          % (pd, P, first, P + span))

# -- dry ------------------------------------------------------------------------
gen = subprocess.run(["python3", "tests/gen.py", "48000", "2", "1"],
                     capture_output=True).stdout
out = subprocess.run(["./bin/t_reverb", "-r", "48000", "-c", "2", "-m", "0", "-b", "97"],
                     input=gen, capture_output=True).stdout
check(out == gen, "mix 0 passes the input bit for bit")

# -- blocks ---------------------------------------------------------------------
outs = [subprocess.run(["./bin/t_reverb", "-r", "48000", "-c", "2", "-b", b, "-M", "80",
                        "-L", "90"], input=gen, capture_output=True).stdout
        for b in ("1", "97", "4096")]
check(outs[0] == outs[1] == outs[2], "the same output at block sizes 1, 97 and 4096")

# -- glide ----------------------------------------------------------------------
# A click is broadband; a glide only moves the pitch, by at most half again
# in the diffuser and 5 % in the tank. So a 220 Hz tone, all wet, has
# nothing above 3 kHz before the change and must have next to nothing after
# it: an instant jump puts the step there at -18 dB under the tone, and the
# glide has to keep it 60 dB under.
tone = [0.5 * math.sin(2 * math.pi * 220 * i / rate) for i in range(int(rate * 3))]
for base, args, what in ((["-s", "80"], ["-Y", "20"], "size 80 -> 20"),
                         (["-s", "30"], ["-Y", "100"], "size 30 -> 100"),
                         (["-s", "80"], ["-Z", "200"], "predelay 10 -> 200"),
                         (["-p", "80"], ["-Z", "0"], "predelay 80 -> 0")):
    y = run(tone, rate, 1, "-m", "100", "-d", "1.5", *base, "-X", str(2 * rate), *args)
    h = butter4(list(y), 3000, rate, True)
    before = max(abs(v) for v in h[int(1.5 * rate):2 * rate])
    after = max(abs(v) for v in h[2 * rate:int(3 * rate)])
    check(after < 0.5e-3, "%s while a tone plays: no click, %.0f dB above 3 kHz (%.0f before)"
          % (what, 20 * math.log10(after / 0.5), 20 * math.log10(before / 0.5)))

# -- stability ------------------------------------------------------------------
(y,) = impulse(rate, 10, 1, "-d", "60", "-H", "100", "-M", "100", "-s", "10", "-p", "0")
e1 = sum(v * v for v in y[:rate])
e9 = sum(v * v for v in y[-rate:])
fin = all(math.isfinite(v) for v in y)
# 60 dB in 60 s is 9 dB over the nine seconds between the two
drop = 10 * math.log10(e1 / e9) if e9 > 0 else 999
check(fin and abs(drop - 9.0) < 1.5,
      "decay 60 s, hf 100, modulation 100, size 10: finite and 9 dB down 9 s on (%.2f)" % drop)

sys.exit(fails)
