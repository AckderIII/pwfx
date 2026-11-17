#!/usr/bin/env python3
"""
notch.py -- the notch core against its design, offline, through bin/t_notch,
the code the filter runs.

The reference comparison in run.sh proves the core and reference/notch.c
agree; this proves what they agree on is what the documentation claims.
Every expectation is derived here, not read from the C:

  geometry   the -3 dB edges f1 < f0 < f2 lie exactly f0/q apart, and the
             bilinear transform makes them satisfy
                 tan(pi f1/fs) tan(pi f2/fs) = tan(pi f0/fs)^2.
             Solved by bisection here, not by the core's closed form.
  response   a Butterworth band-stop of order N on the warped axis
             W = tan(pi f/fs):
                 |H|^2 = 1 / (1 + (B W / (W0^2 - W^2))^(2N)),
             B = W2 - W1, W0 = tan(pi f0/fs). So -3.01 dB at both edges at
             every order, and the shoulders flatter as N grows.
  null       a tone at f0, and with harmonics at every k f0 below 0.49 fs,
             comes out more than 100 dB down once the notch has settled --
             at 44.1, 48 and 96 kHz alike.
  stability  the narrowest, steepest settings ring and decay: bounded,
             finite, the last second quieter than the first.

Tones are measured by RMS over whole seconds after the notch has settled.
Prints ok/FAIL lines and exits with the number of failures.
"""

import math
import struct
import subprocess
import sys

fails = 0


def check(ok, what, detail=""):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what + (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        fails += 1


def run(samples, rate, *args):
    data = struct.pack("<%df" % len(samples), *samples)
    out = subprocess.run(["./bin/t_notch", "-r", str(rate), "-c", "1", *args],
                         input=data, capture_output=True).stdout
    return struct.unpack("<%df" % (len(out) // 4), out)


def tone(f, rate, secs, amp=0.5):
    return [amp * math.sin(2 * math.pi * f * i / rate) for i in range(int(rate * secs))]


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def gain_db(f, rate, args, secs=3.0, settle=2.0):
    """Level of a tone at f through the notch, steady state, dB."""
    x = tone(f, rate, secs)
    y = run(x, rate, *args)
    a = int(settle * rate)
    r = rms(y[a:]) / rms(x[a:])
    return 20 * math.log10(r) if r > 0 else -999.0


def edges(f0, q, rate):
    """f1, f2 with f2 - f1 = f0/q and tan(f1) tan(f2) = tan(f0)^2, by
    bisection on f1 -- the product grows with f1 while f2 stays under
    Nyquist, so it has one root there."""
    k0 = math.tan(math.pi * f0 / rate) ** 2
    bw = f0 / q
    lo, hi = 0.0, min(f0, rate / 2 - bw)    # past that, tan(f2) wraps
    for _ in range(200):
        mid = (lo + hi) / 2
        if math.tan(math.pi * mid / rate) * math.tan(math.pi * (mid + bw) / rate) < k0:
            lo = mid
        else:
            hi = mid
    return lo, lo + bw


def theory_db(f, f0, q, n, rate):
    f1, f2 = edges(f0, q, rate)
    w = math.tan(math.pi * f / rate)
    w0 = math.tan(math.pi * f0 / rate)
    B = math.tan(math.pi * f2 / rate) - math.tan(math.pi * f1 / rate)
    if abs(w0 * w0 - w * w) < 1e-300:
        return -999.0
    x = B * w / (w0 * w0 - w * w)
    return -10 * math.log10(1 + x ** (2 * n))


# The geometry and the response, by measurement: edges at -3.01 dB at every
# order, and the response a Butterworth band-stop's half an octave out and
# a width either side.
for f0, q, n, rate in ((1000, 10, 1, 48000), (1000, 10, 2, 48000), (1000, 10, 4, 48000),
                       (12000, 4, 2, 48000), (18000, 3, 3, 44100), (120, 5, 8, 96000)):
    f1, f2 = edges(f0, q, rate)
    args = ["-f", str(f0), "-Q", str(q), "-n", str(n)]
    g1, g2 = gain_db(f1, rate, args), gain_db(f2, rate, args)
    check(abs(g1 + 3.0103) < 0.03 and abs(g2 + 3.0103) < 0.03,
          "f %g q %g order %d at %g Hz: -3.01 dB at %.2f and %.2f Hz, %g Hz apart"
          % (f0, q, n, rate, f1, f2, f0 / q), "%.3f / %.3f dB" % (g1, g2))
    probes = [f for f in (f0 / math.sqrt(2), f1 - f0 / q, f2 + f0 / q, f0 * math.sqrt(2))
              if 0 < f < rate * 0.45]
    worst = max(abs(gain_db(f, rate, args) - theory_db(f, f0, q, n, rate)) for f in probes)
    check(worst < 0.03, "  and the Butterworth band-stop response either side",
          "worst %.3f dB" % worst)

# Order buys flat shoulders at the same width: two widths off the centre,
# order 1 still cuts a quarter of a dB and order 4 a ten-thousandth.
g1 = gain_db(1200, 48000, ["-f", "1000", "-Q", "10", "-n", "1"])
g4 = gain_db(1200, 48000, ["-f", "1000", "-Q", "10", "-n", "4"])
check(abs(g1 - theory_db(1200, 1000, 10, 1, 48000)) < 0.01 and g1 < -0.25 and g4 > -0.001,
      "200 Hz off a 1 kHz q 10 notch: order 1 cuts %.2f dB, order 4 %.4f" % (g1, g4))

# The null, on the centre and on every harmonic, at three rates.
for rate in (44100, 48000, 96000):
    for f0, q, n, H in ((50, 20, 2, 8), (1000, 10, 1, 1), (60, 10, 4, 5)):
        args = ["-f", str(f0), "-Q", str(q), "-n", str(n), "-H", str(H)]
        worst = max(gain_db(f0 * k, rate, args, secs=4.0, settle=3.0) for k in range(1, H + 1))
        check(worst < -100,
              "f %g q %g order %d x%d at %g Hz: every harmonic over 100 dB down"
              % (f0, q, n, H, rate), "worst %.1f dB" % worst)

# Harmonics are notched and what lies between them is not: halfway between
# the 2nd and 3rd harmonics of 50 Hz, a q 20 order 2 stack leaves 125 Hz be.
g = gain_db(125, 48000, ["-f", "50", "-Q", "20", "-n", "2", "-H", "8"])
check(abs(g) < 0.05, "125 Hz between 50 Hz harmonics passes (%.3f dB)" % g)
# and a harmonic past the top is left out, not folded: 7.5 kHz x 4 at
# 48 kHz notches 7.5, 15 and 22.5 kHz; 30 would alias to 18, which passes
g = gain_db(18000, 48000, ["-f", "7500", "-Q", "20", "-n", "2", "-H", "4"])
check(abs(g) < 0.01, "a harmonic above 0.49 fs is dropped, not aliased (%.4f dB at 18 kHz)" % g)

# Stability where it is hardest: poles within 1e-5 of the unit circle.
for args in (["-f", "50", "-Q", "300", "-n", "8", "-H", "4"],
             ["-f", "10", "-Q", "300", "-n", "8"],
             ["-f", "23000", "-Q", "300", "-n", "8"]):
    rate = 48000
    imp = [1.0] + [0.0] * (rate * 10 - 1)
    y = run(imp, rate, *args)
    finite = all(math.isfinite(v) for v in y)
    first, last = rms(y[1:rate]), rms(y[-rate:])
    check(finite and max(abs(v) for v in y) <= 1.0 + 1e-6 and last < first,
          "impulse through %s: bounded and decaying" % " ".join(args),
          "finite %s, peak %.3g, first second %.3g, last %.3g"
          % (finite, max(abs(v) for v in y), first, last))

sys.exit(fails)
