#!/usr/bin/env python3
"""
curves.py -- check each filter's response curve (--curve) against what
theory says it should be, offline. The curve is computed from the very
coefficients the filter runs, so this pins both: that the curve is drawn
right, and that the design is the one the documentation claims.

Every expectation is derived from first principles, not from the code:

  Butterworth  the bilinear transform prewarped at fc maps the analog
               response onto the digital axis exactly, so at every point
               |H|^2 = 1 / (1 + W^(2n)) with W = tan(pi f/fs) / tan(pi fc/fs)
               (W^-1 for the high-pass). To the printed 0.01 dB.
  compressor   y = x + makeup below the threshold, thr + (x - thr)/ratio +
               makeup above it, for a hard knee.
  limiter      y = min(x + input-gain, ceiling).
  gain         y = x + gain; muted, nothing; balance only ever turns a side
               down, so the louder side's line is still y = x + gain.
  gate         rising, shut by the range until the threshold; falling, open
               down to threshold - hysteresis: a loop, in that order.
  EQ           all gains 0 is exactly flat; a bell peaks at its gain; a
               shelf reaches its gain well past its corner.

Prints ok/FAIL lines and exits with the number of failures.
"""

import math
import subprocess
import sys

RATE = 48000.0
fails = 0


def check(ok, what, detail=""):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what + (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        fails += 1


def curve(binary, *args):
    out = subprocess.run(["./bin/" + binary, *args, "--curve"],
                         capture_output=True, text=True).stdout.strip()
    name, kind, pts = out.split(" ", 2)
    assert name == "curve", out[:80]
    return kind, [tuple(map(float, p.split(":"))) for p in pts.split(",")]


def butter_db(f, fc, n, high):
    w = math.tan(math.pi * f / RATE) / math.tan(math.pi * fc / RATE)
    if high:
        w = 1.0 / w
    return -10.0 * math.log10(1.0 + w ** (2 * n))


def worst(pts, want):
    return max(abs(y - want(x)) for x, y in pts)


for binary, high in (("pw_lpf", False), ("pw_hpf", True)):
    for fc, n in ((1000, 4), (80, 1), (5000, 7), (200, 16)):
        kind, pts = curve(binary, "-f", str(fc), "-n", str(n))
        err = worst([p for p in pts if p[1] > -200], lambda f: butter_db(f, fc, n, high))
        check(kind == "freq" and err <= 0.006,
              "%s fc %g order %d: matches the Butterworth response at %d points"
              % (binary, fc, n, len(pts)), "worst %.4f dB" % err)

kind, pts = curve("pw_lpf", "-f", "1000", "-n", "4")
check(pts[0][0] == 20 and abs(pts[-1][0] - 20000) < 1, "frequency grid runs 20 Hz to 20 kHz",
      "%g..%g" % (pts[0][0], pts[-1][0]))

kind, pts = curve("pw_eq")
check(all(y == 0.0 for _, y in pts), "pw_eq at defaults is exactly flat")
for bells in ("orfanidis", "rbj"):
    kind, pts = curve("pw_eq", "--b3-freq", "1000", "--b3-gain", "6", "--b3-q", "2", "--bells", bells)
    peak = max(pts, key=lambda p: p[1])
    # the grid has 16 points an octave, so the nearest one sits within
    # 1/32 octave of the centre, where a Q 2 bell is down a few hundredths
    check(abs(peak[1] - 6.0) <= 0.1 and abs(math.log2(peak[0] / 1000)) < 0.05,
          "pw_eq %s bell +6 dB at 1 kHz peaks at its gain" % bells,
          "%.2f dB at %.0f Hz" % (peak[1], peak[0]))
kind, pts = curve("pw_eq", "--ls-freq", "200", "--ls-gain", "-9")
check(abs(pts[0][1] + 9.0) <= 0.1 and abs(pts[-1][1]) <= 0.05,
      "pw_eq low shelf -9 dB at 200 Hz: -9 at the bottom, 0 at the top",
      "%.2f / %.2f" % (pts[0][1], pts[-1][1]))

kind, pts = curve("pw_comp", "-t", "-30", "-R", "4", "-w", "0", "-m", "6")
err = worst(pts, lambda x: (x if x <= -30 else -30 + (x + 30) / 4.0) + 6)
check(kind == "level" and err <= 0.006,
      "pw_comp threshold -30 ratio 4 makeup 6: the static curve", "worst %.4f dB" % err)
kind, pts = curve("pw_comp", "-t", "-20", "-R", "2", "-w", "10")
# a soft knee bends away from both lines, never above the identity, and
# meets the hard curve again outside it
err = worst([p for p in pts if abs(p[0] + 20) >= 5],
            lambda x: x if x <= -20 else -20 + (x + 20) / 2.0)
check(err <= 0.006 and all(y <= x + 0.005 for x, y in pts),
      "pw_comp soft knee 10 dB: hard curve outside the knee, under the identity inside")

kind, pts = curve("pw_limit", "-C", "-1", "-i", "6")
err = worst(pts, lambda x: min(x + 6, -1))
check(kind == "level" and err <= 0.006, "pw_limit ceiling -1 input-gain 6", "worst %.4f dB" % err)

for args, want, what in ((["-g", "6"], lambda x: x + 6, "gain +6"),
                         (["-g", "-60"], lambda x: x - 60, "gain -60"),
                         (["-g", "6", "-b", "70", "-i", "all"], lambda x: x + 6,
                          "gain +6, balance 70, inverted: still the louder side, unsigned")):
    kind, pts = curve("pw_gain", *args)
    err = worst(pts, want)
    check(kind == "level" and err <= 0.006, "pw_gain %s" % what, "worst %.4f dB" % err)
kind, pts = curve("pw_gain", "-m")
check(all(y <= -240 for _, y in pts), "pw_gain muted: nothing comes out")

kind, pts = curve("pw_gate", "-t", "-40", "-T", "6", "-g", "20")
xs = [x for x, _ in pts]
up = xs.index(max(xs))
rising, falling = pts[:up + 1], pts[up:]
jump = [i for i in range(1, len(rising)) if rising[i][0] == rising[i - 1][0]]
drop = [i for i in range(1, len(falling)) if falling[i][0] == falling[i - 1][0]]
check(kind == "level" and len(jump) == 1 and rising[jump[0]][0] == -40
      and rising[jump[0] - 1][1] == -60 and rising[jump[0]][1] == -40,
      "pw_gate rising: shut 20 dB down until -40, then open", str(rising[jump[0] - 1:jump[0] + 1]) if jump else "no jump")
check(len(drop) == 1 and falling[drop[0]][0] == -46 and falling[drop[0]][1] == -66
      and all(abs(x - y) < 0.005 for x, y in falling[:drop[0]]),
      "pw_gate falling: open down to -46 (hysteresis 6), then shut",
      str(falling[drop[0] - 1:drop[0] + 1]) if drop else "no drop")

r = subprocess.run(["./bin/pw_eq", "--curve", "--b3-gain", "3"], capture_output=True, text=True)
check(r.stdout.count("\n") == 1 and r.returncode == 0,
      "--curve prints one line and exits 0, wherever it sits on the command line")

sys.exit(fails)
