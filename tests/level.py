#!/usr/bin/env python3
"""
level.py -- the leveler core against BS.1770 and its design, offline.

The reference comparison in run.sh proves src/level_dsp.h and
reference/level.c agree to the bit; this proves what they agree on is what
the design says. Signals go through bin/t_level (the code the filter runs)
and the output is measured:

  loudness   a steady tone of known amplitude settles at the gain that puts
             it at the target, where its loudness is computed here from
             BS.1770's own published 48 kHz K-weighting coefficients -- not
             from the C, which designs them from analogue prototypes. At
             100 Hz to 10 kHz and on the high-pass's skirt; a 997 Hz sine at
             0 dBFS is -3.01 LUFS, the standard's own check.
  rates      the same tone at 44.1 and 96 kHz reads the same loudness.
  channels   the channels' powers add: the same tone in two reads +3.01.
  power      a tone switching between two levels within the window reads
             their mean power: loudness, not peaks.
  pauses     a tone switched on and off within the window reads as the
             steady tone: hops under the gate are left out of the average,
             so pauses neither dilute the reading nor get ridden up.
  caps       the gain never past max-boost or max-cut, however far off.
  strength   50 % goes half the distance.
  relative   bursts over a noise floor just above the gate read as the
             bursts: hops 20 dB under the recent mean are dropped as well.
  scale      real speech (tests/data/speech.wav) and its own room noise,
             played 20 dB apart, settle 20 dB apart however low the gate
             is set: the relative gate keeps the room out of both.
  speech     the same speech at three levels 20 dB apart comes out within
             a dB of itself, by BS.1770's integrated loudness computed
             here -- 1-2 dB under the target, since that counts the gaps
             within phrases and the leveler does not.
  gate       below the gate the gain holds where it was, exactly, and a
             leveler that never got past it passes its input through bit
             for bit.
  timing     after a step the gain is 63 % of the way there one time
             constant later -- attack down, release up -- within the window
             it takes the detector to see the step.
  lookahead  with L samples of lookahead the gain on output sample n is the
             gain without it on sample n + L, to float precision.
  linked     every channel gets the same gain.

Prints ok/FAIL lines and exits with the number of failures.
"""

import cmath
import math
import random
import struct
import subprocess
import sys
import wave

RATE = 48000
fails = 0

# ITU-R BS.1770-4, table 1 and table 2: the K-weighting at 48 kHz.
SHELF = ((1.53512485958697, -2.69169618940638, 1.19839281085285),
         (1.0, -1.69065929318241, 0.73248077421585))
RLB = ((1.0, -2.0, 1.0), (1.0, -1.99004745483398, 0.99007225036621))


def check(ok, what, detail=""):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what + (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        fails += 1


def k_power(f):
    """|K(f)|^2 at 48 kHz from the published coefficients."""
    z = cmath.exp(-1j * 2 * math.pi * f / 48000.0)
    h = 1.0
    for b, a in (SHELF, RLB):
        h *= (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)
    return abs(h) ** 2


def lufs_of_tone(amp, f, channels=1):
    return -0.691 + 10 * math.log10(channels * amp * amp / 2 * k_power(f))


def tone(f, amp, secs, rate=RATE, phase=0.0):
    return [amp * math.sin(2 * math.pi * f * i / rate + phase) for i in range(int(rate * secs))]


def run(x, *args, rate=RATE, channels=1):
    """x is interleaved; returns the output, interleaved, and the per-block
    gain trace t_level prints with -v."""
    data = struct.pack("<%df" % len(x), *x)
    p = subprocess.run(["./bin/t_level", "-r", str(rate), "-c", str(channels), "-v", *args],
                       input=data, capture_output=True)
    y = list(struct.unpack("<%df" % (len(p.stdout) // 4), p.stdout))
    trace = [float(l.split()[1]) for l in p.stderr.decode().splitlines() if l.startswith("gain ")]
    return y, trace


def gain_db(x, y, a, b):
    """The gain from x to y over samples a..b, by power."""
    px = sum(v * v for v in x[a:b])
    py = sum(v * v for v in y[a:b])
    return 10 * math.log10(py / px)


# Settings that settle within a second: a short window, fast both ways,
# wide caps, and a gate nothing here falls under.
FAST = ["-w", "400", "-a", "20", "-e", "20", "-B", "30", "-C", "30", "-g", "-80"]


def settled(x, *args, rate=RATE, channels=1, secs=2.0):
    y, _ = run(x, *FAST, *args, rate=rate, channels=channels)
    a = int(rate * (secs - 0.5)) * channels
    return gain_db(x, y, a, len(x))


def k_weight(x):
    y = x
    for b, a in (SHELF, RLB):
        s1 = s2 = 0.0
        out = []
        for v in y:
            o = b[0] * v + s1
            s1 = b[1] * v - a[1] * o + s2
            s2 = b[2] * v - a[2] * o
            out.append(o)
        y = out
    return y


# ---- loudness --------------------------------------------------------------

for f in (100.0, 997.0, 3000.0, 10000.0, 30.0):
    amp = 0.1
    want = -23.0 - lufs_of_tone(amp, f)
    got = settled(tone(f, amp, 2.0), "-l", "-23")
    check(abs(got - want) < 0.02,
          "%5.0f Hz at %.1f LUFS is ridden to -23: %+.3f dB (BS.1770 says %+.3f)"
          % (f, lufs_of_tone(amp, f), got, want))

got = settled(tone(997.0, 1.0, 2.0), "-l", "-3.01")
check(abs(got) < 0.02, "a 997 Hz sine at 0 dBFS reads -3.01 LUFS: %+.3f dB of gain" % got)

for rate in (44100, 96000):
    want = -23.0 - lufs_of_tone(0.1, 997.0)
    got = settled(tone(997.0, 0.1, 2.0, rate=rate), "-l", "-23", rate=rate)
    check(abs(got - want) < 0.02,
          "at %d Hz the same tone reads the same: %+.3f dB (want %+.3f)" % (rate, got, want))

x = tone(997.0, 0.1, 2.0)
st = [v for v in x for _ in range(2)]
got = settled(st, "-l", "-23", channels=2)
want = -23.0 - lufs_of_tone(0.1, 997.0, channels=2)
check(abs(got - want) < 0.02,
      "two channels' powers add, +3.01: %+.3f dB (want %+.3f)" % (got, want))

# 20 ms loud, 20 ms 6 dB down: ten whole periods in the 400 ms window, and
# whole hops, so its power is exactly the mean of the two whichever hop it
# is read at -- 10 log10((1 + 1/4) / 2) = -2.04 dB from the louder.
on = int(RATE * 0.02)
x = [v if (i // on) % 2 == 0 else 0.5 * v for i, v in enumerate(tone(997.0, 0.1, 2.0))]
y, _ = run(x, *FAST, "-l", "-23")
a = int(RATE * 1.5)
got = gain_db(x, y, a, len(x))
want = -23.0 - (lufs_of_tone(0.1, 997.0) + 10 * math.log10(1.25 / 2))
check(abs(got - want) < 0.05,
      "two levels in the window read as their mean power: %+.3f dB (want %+.3f)" % (got, want))

# 100 ms on, 100 ms of silence: the pauses are under the gate and do not
# count, so it reads as the steady tone does -- not 3 dB down -- and the
# gain does not move through them.
on = int(RATE * 0.1)
x = [v if (i // on) % 2 == 0 else 0.0 for i, v in enumerate(tone(997.0, 0.1, 3.0))]
y, trace = run(x, *FAST, "-g", "-50", "-l", "-23")
want = -23.0 - lufs_of_tone(0.1, 997.0)
tail = trace[len(trace) // 2:]
check(all(abs(t - want) < 0.02 for t in tail),
      "pauses in the window neither count nor get ridden up: %+.3f..%+.3f dB (want %+.3f)"
      % (min(tail), max(tail), want))

# The same bursts with noise in the gaps at -48 LUFS, over a -50 gate: the
# absolute gate lets the noise through, and without the relative one half
# the window's hops would be noise and the reading 3 dB low. The mean of
# the hops it lets through is about -26, so the relative gate sits at -46;
# what is left is a hop of each burst's own decay, now and then.
random.seed(1)
noise = [random.gauss(0.0, 1.0) for _ in range(int(RATE * 3.0))]
kn = k_weight(noise)
scale = 10 ** ((-48.0 + 0.691 - 10 * math.log10(sum(v * v for v in kn) / len(kn))) / 20)
x = [v if (i // on) % 2 == 0 else scale * noise[i]
     for i, v in enumerate(tone(997.0, 0.1, 3.0))]
y, trace = run(x, *FAST, "-g", "-50", "-l", "-23")
tail = trace[len(trace) // 2:]
check(all(abs(t - want) < 0.3 for t in tail),
      "noise over the gate between bursts is dropped by the relative gate: "
      "%+.3f..%+.3f dB (want %+.3f)" % (min(tail), max(tail), want))

# ---- real speech -----------------------------------------------------------

def integrated(x):
    """BS.1770-4 integrated loudness, mono: 400 ms blocks at 75 % overlap,
    gated at -70 LUFS and then 10 LU under the mean."""
    y = k_weight(x)
    blk, step = int(0.4 * RATE), int(0.1 * RATE)
    lu = lambda p: -0.691 + 10 * math.log10(p) if p > 0 else -1e9
    ps = [sum(v * v for v in y[i:i + blk]) / blk for i in range(0, len(y) - blk + 1, step)]
    ps = [p for p in ps if lu(p) > -70]
    rel = lu(sum(ps) / len(ps)) - 10
    ps = [p for p in ps if lu(p) > rel]
    return lu(sum(ps) / len(ps))


w = wave.open("tests/data/speech.wav")
voice = [v / 32768.0 for v in struct.unpack("<%dh" % w.getnframes(), w.readframes(w.getnframes()))]
seg = voice * 3
for gate in ("-80", "-65"):
    got = []
    for g in (-12.0, 8.0):
        _, tr = run([v * 10 ** (g / 20) for v in seg], "-l", "-20", "-B", "30", "-C", "30",
                    "-g", gate)
        got.append(tr[-1])
    check(abs(got[0] - got[1] - 20.0) < 0.3,
          "speech 20 dB apart, room and all, settles %.2f dB apart with the gate at %s"
          % (got[0] - got[1], gate))

seg = voice * 4                                      # 24 s, the last 6 measured
gains = (-12.0, 8.0, -4.0)
x = [v * 10 ** (g / 20) for g in gains for v in seg]
# the default times, a gate under the quiet copy's softest syllables and caps
# with room to spare: the quietest wants +19 dB, and a cap at 20 clips the
# tops of a reading that swings by a few dB over 400 ms of speech
y, _ = run(x, "-l", "-20", "-B", "30", "-C", "30", "-g", "-65")
n = len(seg)
ins = [integrated(x[(k + 1) * n - len(voice):(k + 1) * n]) for k in range(3)]
outs = [integrated(y[(k + 1) * n - len(voice):(k + 1) * n]) for k in range(3)]
check(max(outs) - min(outs) < 1.0 and all(-22.0 <= o <= -20.5 for o in outs),
      "speech at %s LUFS comes out at %s LUFS, levelled to -20"
      % ("/".join("%.1f" % v for v in ins), "/".join("%.1f" % v for v in outs)))

# ---- the gain computer -----------------------------------------------------

got = settled(tone(997.0, 0.001, 2.0), "-l", "-18", "-B", "12")
check(abs(got - 12.0) < 0.005, "a tone at -63 LUFS is boosted by max-boost and no more: %+.3f" % got)
got = settled(tone(997.0, 0.5, 2.0), "-l", "-40", "-C", "6")
check(abs(got + 6.0) < 0.005, "a tone at -9 LUFS is cut by max-cut and no more: %+.3f" % got)

L = lufs_of_tone(0.1, 997.0)
got = settled(tone(997.0, 0.1, 2.0), "-l", "-30", "-s", "50")
check(abs(got - 0.5 * (-30 - L)) < 0.02,
      "strength 50 goes half the way: %+.3f dB (want %+.3f)" % (got, 0.5 * (-30 - L)))

# ---- the gate --------------------------------------------------------------

# Loud enough to be cut 10 dB, then quiet far under the gate: the gain holds
# at -10 through the quiet part instead of climbing to boost it. At the
# default release a window that counted the pause would have read the
# phrase fading for 400 ms and turned up about 0.7 dB on the way into it;
# what is left is the last few hops' reading of the tone itself.
loud = tone(997.0, 0.1, 2.0)
quiet = tone(997.0, 0.0005, 2.0)
x = loud + quiet
lvl = lufs_of_tone(0.1, 997.0)
y, trace = run(x, "-w", "400", "-a", "20", "-e", "3000", "-g", "-50",
               "-l", "%.6f" % (lvl - 10.0))
before = gain_db(x, y, int(RATE * 1.5), int(RATE * 2.0))
during = gain_db(x, y, int(RATE * 3.0), len(x))
check(abs(before + 10.0) < 0.02 and abs(during - before) < 0.05,
      "under the gate the gain holds: %+.3f dB before, %+.3f in the pause" % (before, during))

noise_floor = tone(997.0, 0.0005, 1.0)          # about -69 LUFS
y, trace = run(noise_floor, "-g", "-50", "-k", "20")
check(y == [struct.unpack("<f", struct.pack("<f", v))[0] for v in noise_floor]
      and all(t == 0.0 for t in trace),
      "never past the gate, the input comes through bit for bit")

# ---- timing ----------------------------------------------------------------

def crossing(trace, start, frac, g0, g1, block):
    """Seconds after the block 'start' at which the gain has gone frac of
    the way from g0 to g1."""
    target = g0 + frac * (g1 - g0)
    for i in range(start, len(trace)):
        if (g1 - g0) * (trace[i] - target) >= 0:
            return (i - start + 1) * block / RATE
    return None


BLOCK = 128
for name, a1, a2, tau_opt in (("attack", 0.01, 0.1, "-a"), ("release", 0.1, 0.01, "-e")):
    # a step of 20 dB in level, settled on both sides at slow times on the
    # side not being measured
    tau = 2.0
    x = tone(997.0, a1, 3.0) + tone(997.0, a2, 8.0)
    target = -25.0
    g0 = target - lufs_of_tone(a1, 997.0)
    g1 = target - lufs_of_tone(a2, 997.0)
    opts = ["-w", "100", "-g", "-80", "-B", "30", "-C", "30", "-l", str(target), "-b", str(BLOCK)]
    opts += ["-a", "20", "-e", "20"]
    opts[opts.index(tau_opt) + 1] = str(tau * 1000)
    # the other direction fast, so the first segment settles
    _, trace = run(x, *opts)
    step = int(3.0 * RATE) // BLOCK
    settled_g = trace[step - 1]
    t = crossing(trace, step, 1 - math.exp(-1), settled_g, g1, BLOCK)
    check(abs(settled_g - g0) < 0.05 and t is not None and 0 <= t - tau <= 0.1,
          "%s: 63 %% of a %+.1f dB move %.3f s after the step (tau %.1f s, plus up to the "
          "100 ms window)" % (name, g1 - g0, t or -1, tau),
          "settled at %+.3f, want %+.3f" % (settled_g, g0))

# ---- lookahead and linking -------------------------------------------------

# Never zero, so the gain is the ratio of output to input at every sample.
x = [0.4 + v for v in tone(997.0, 0.05, 1.0)] + [0.4 + v for v in tone(997.0, 0.3, 1.0)]
y0, _ = run(x, "-a", "50", "-e", "50", "-l", "-20")
yk, _ = run(x, "-a", "50", "-e", "50", "-l", "-20", "-k", "200")
L = int(0.2 * RATE)
worst = max(abs(math.log10(yk[n] / x[n]) - math.log10(y0[n + L] / x[n + L])) * 20
            for n in range(0, len(x) - L))
n = int(0.95 * RATE)
moved = 20 * math.log10((yk[n] / x[n]) / (y0[n] / x[n]))
check(worst < 1e-4 and moved < -3.0,
      "200 ms of lookahead applies the gain 9600 samples early (worst %.2g dB; "
      "50 ms before the step %+.2f dB below the gain without it)" % (worst, moved))

xs = tone(997.0, 0.1, 1.5)
st = [v * k for v in xs for k in (1.0, -0.25)]
y, _ = run(st, *FAST, "-l", "-30", channels=2)
worst = max(abs(y[2 * n] / st[2 * n] - y[2 * n + 1] / st[2 * n + 1])
            for n in range(len(xs)) if abs(st[2 * n + 1]) > 1e-3)
check(worst < 1e-5, "one gain for every channel (worst ratio difference %.2g)" % worst)

sys.exit(fails)
