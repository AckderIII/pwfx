#!/usr/bin/env python3
"""
deess.py -- the de-esser core against its design equations, offline.

The reference comparison in run.sh proves src/deess_dsp.h and
reference/deess.c agree to the bit; this proves what they agree on is what
the design says. Steady tones go through bin/t_deess (the code the filter
runs), and the level of each tone in the settled output is compared with:

  idle      split mode with nothing to cut passes its input bit for bit --
            given input that never gives it anything to cut: an abrupt
            onset is a broadband click, which it rightly trims, and the
            reduction then decays to exactly 0 only below 1e-12 dB
  abs       band RMS L dB, threshold T, ratio R: a cut of (L - T)(1 - 1/R)
            at the centre, outside the knee -- exactly g, since the band
            has unity gain and zero phase there
  range     a cut never past the range, however hard it is pushed
  rel       the band's level relative to the whole signal's is what is
            compared; two equal tones, one in the band, put it at -3.01 dB
            on average -- but the design averages both over 5 ms and holds
            the peak of the reduction, so what it cuts by is the peak of
            that ratio as a 5 ms RMS sees it. That is modelled here from
            the description, not read off the C.
  skirt     off the centre the cut is |1 - (1 - g) H(f)|, H the band-pass
  wide      the whole signal down by the same g
  listen    the band alone: |H(f)| per tone

H is computed here from the RBJ band-pass formula on its own, not taken
from the C. Prints ok/FAIL lines and exits with the number of failures.
"""

import cmath
import math
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


def H(f, f0=6500.0, q=1.5):
    """RBJ constant-0-dB-peak band-pass at f, complex."""
    w0 = 2 * math.pi * f0 / RATE
    alpha = math.sin(w0) / (2 * q)
    a0 = 1 + alpha
    b = (alpha / a0, 0.0, -alpha / a0)
    a = (1.0, -2 * math.cos(w0) / a0, (1 - alpha) / a0)
    z = cmath.exp(-1j * 2 * math.pi * f / RATE)
    return (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)


def tones(freqs, amp=0.3, secs=1.5, fade=0.0):
    n, m = int(RATE * secs), int(RATE * fade)
    env = lambda i: 0.5 - 0.5 * math.cos(math.pi * i / m) if i < m else 1.0
    return [env(i) * sum(amp * math.sin(2 * math.pi * f * i / RATE) for f in freqs)
            for i in range(n)]


def run(x, *args):
    data = struct.pack("<%df" % len(x), *x)
    out = subprocess.run(["./bin/t_deess", "-r", str(RATE), "-c", "1", *args],
                         input=data, capture_output=True).stdout
    return data, out, list(struct.unpack("<%df" % (len(out) // 4), out))


def amp_at(y, f):
    """Amplitude of the tone at f over the settled second half."""
    y = y[len(y) // 2:]
    c = sum(v * math.cos(2 * math.pi * f * i / RATE) for i, v in enumerate(y))
    s = sum(v * math.sin(2 * math.pi * f * i / RATE) for i, v in enumerate(y))
    return 2 * math.hypot(c, s) / len(y)


def db(x):
    return 20 * math.log10(x) if x > 0 else -240.0


def gr(level_db, thr, ratio, rng):
    """Cut the gain computer asks for, outside the knee, capped."""
    return min(max(0.0, (level_db - thr) * (1 - 1 / ratio)), rng)


band_db = db(0.3 / math.sqrt(2))                  # RMS of a 0.3 tone, -13.47

# idle: rel default, a voice band only, faded in -- nothing to cut at any
# point, so the bytes come back unchanged
x = tones([200, 440, 1000], amp=0.2, fade=0.05)
data, out, _ = run(x)
check(data == out, "nothing to cut: split mode passes its input bit for bit")

# abs at the centre
want = gr(band_db, -30, 4, 30)
_, _, y = run(tones([6500]), "-d", "abs", "-t", "-30", "-R", "4", "-g", "30")
got = db(amp_at(y, 6500) / 0.3)
check(abs(got + want) <= 0.1, "abs: centre cut %.2f dB (want -%.2f)" % (got, want))

# range caps it
_, _, y = run(tones([6500]), "-d", "abs", "-t", "-40", "-R", "20", "-g", "6")
got = db(amp_at(y, 6500) / 0.3)
check(abs(got + 6.0) <= 0.05, "range 6: cut %.2f dB, no more" % got)

def rel_peak(freqs, amp=0.3, rms_ms=5.0):
    """The documented detector, on its own: 5 ms one-pole RMS of the band
    (each tone through H) and of the whole signal, and the highest their
    ratio reaches once settled."""
    a = math.exp(-1000.0 / (rms_ms * RATE))
    hs = [H(f) for f in freqs]
    ab = ax = 0.0
    top = -240.0
    for i in range(RATE):
        x = b = 0.0
        for f, h in zip(freqs, hs):
            ph = 2 * math.pi * f * i / RATE
            x += amp * math.sin(ph)
            b += amp * abs(h) * math.sin(ph + cmath.phase(h))
        ab = a * ab + (1 - a) * b * b
        ax = a * ax + (1 - a) * x * x
        if i > RATE // 2:
            top = max(top, 10 * math.log10(ab / ax))
    return top


# rel, two equal tones; the 200 Hz one only moves by the band's skirt
h2 = abs(H(200)) ** 2
det = rel_peak([200, 6500])
want = gr(det, -12, 2, 30)
g = 10 ** (-want / 20)
_, _, y = run(tones([200, 6500]), "-t", "-12", "-R", "2", "-g", "30")
hi = db(amp_at(y, 6500) / 0.3)
lo = db(amp_at(y, 200) / 0.3)
lo_want = db(abs(1 - (1 - g) * H(200)))
check(abs(hi + want) <= 0.05,
      "rel: band at %.2f dB relative (at its 5 ms peak) cuts the 6.5 kHz tone %.2f dB (want -%.2f)"
      % (det, hi, want))
check(abs(lo - lo_want) <= 0.02,
      "rel: the 200 Hz tone moves only by the skirt, %.3f dB (want %.3f)" % (lo, lo_want))

# skirt: a tone off the centre, cut by |1 - (1 - g) H(f)|
_, _, y = run(tones([6500, 4000]), "-d", "abs", "-t", "-40", "-R", "20", "-g", "12")
g = 10 ** (-12 / 20)
got = db(amp_at(y, 4000) / 0.3)
want_db = db(abs(1 - (1 - g) * H(4000)))
check(abs(got - want_db) <= 0.05,
      "skirt: 4 kHz, off the centre, cut %.2f dB (want %.2f)" % (got, want_db))

# wide: everything down by g
want = gr(band_db, -30, 4, 30)
_, _, y = run(tones([200, 6500]), "-d", "abs", "-t", "-30", "-R", "4", "-g", "30", "-m", "wide")
# the 200 Hz tone adds a little band energy through the skirt
det = 10 * math.log10(0.045 * (1 + h2))
want = gr(det, -30, 4, 30)
for f in (200, 6500):
    got = db(amp_at(y, f) / 0.3)
    check(abs(got + want) <= 0.1, "wide: %d Hz down %.2f dB with the rest (want -%.2f)" % (f, got, want))

# listen: the band alone
_, _, y = run(tones([200, 6500]), "-L")
for f in (200, 6500):
    got = db(amp_at(y, f) / 0.3)
    want_db = db(abs(H(f)))
    check(abs(got - want_db) <= 0.05, "listen: %d Hz at %.2f dB, the band's own (want %.2f)" % (f, got, want_db))

sys.exit(fails)
