#!/usr/bin/env python3
"""
denoise.py -- the noise suppressor on real speech and real noise, offline.

The reference comparison in run.sh proves the core's framing agrees with
reference/denoise.c at every block size. Both rest on one assumption, though
-- that RNNoise's own delay is exactly one frame -- and if it were wrong they
would both mix the denoised signal with a misaligned dry one, comb-filter
it, and still agree. So this measures that, and what the filter is for:

  alignment  at full reduction the output is RNNoise's alone; on speech it
             correlates with the input best at lag 0, not a frame off
  identity   reduction 0 gives back the input, bit for bit
  limit      on pure noise the reduction is a limit: 12 dB asks for about
             12 dB, 30 for about 30, never more than asked
  gate       pure noise through the voice gate comes out at exactly
             f * dry: -20.00 dB at reduction 20, once the hold has run out
  speech     speech with noise under it: its loud parts lose under 1.5 dB,
             the noise in its pauses at least 15 dB
  other rate at anything but 48 kHz the core passes its input through
             untouched, with no delay -- run through bin/t_denoise, since
             the reference refuses any other rate

The speech is tests/data/speech.wav (public domain, see its README), run
through bin/ref/denoise, which is aligned with its input.
"""

import math
import os
import random
import struct
import subprocess
import sys
import wave

RATE = 48000
HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0


def check(ok, what, detail=""):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what + (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        fails += 1


def speech():
    w = wave.open(os.path.join(HERE, "data", "speech.wav"))
    assert (w.getnchannels(), w.getframerate(), w.getsampwidth()) == (1, RATE, 2)
    n = w.getnframes()
    return [v / 32768.0 for v in struct.unpack("<%dh" % n, w.readframes(n))]


def noise(n, db, seed=20261101):
    rnd = random.Random(seed)
    a = 10 ** (db / 20) * math.sqrt(3)        # uniform: RMS = a / sqrt(3)
    return [rnd.uniform(-a, a) for _ in range(n)]


def run(x, *args):
    data = struct.pack("<%df" % len(x), *x)
    out = subprocess.run([os.path.join(HERE, "..", "bin", "ref", "denoise"), "-c", "1", *args],
                         input=data, capture_output=True).stdout
    return data, out, list(struct.unpack("<%df" % (len(out) // 4), out))


def rms_db(x):
    e = sum(v * v for v in x) / max(1, len(x))
    return 10 * math.log10(e) if e > 0 else -240.0


def blocks(x, size=4800):
    return [x[i:i + size] for i in range(0, len(x) - size + 1, size)]


s = speech()
n = len(s)

# alignment: where does RNNoise's output line up with its input?
_, _, wet = run(s, "-n", "100")
lo, hi = int(0.5 * RATE), int(5.5 * RATE)


def corr(lag):
    return sum(s[i] * wet[i + lag] for i in range(lo, hi))


lags = range(-960, 961, 8)
best = max(lags, key=corr)
fine = max(range(best - 8, best + 9), key=corr)
check(fine == 0, "RNNoise's output lines up with its input at lag 0", "best lag %d" % fine)

# identity
data, out, _ = run(s, "-n", "0")
check(data == out, "reduction 0 gives back the input bit for bit")

# the reduction as a limit, on pure noise
x = noise(RATE * 4, -30)
for want in (12, 30):
    _, _, y = run(x, "-n", str(want))
    got = rms_db(y[RATE:]) - rms_db(x[RATE:])
    check(-want - 1.5 <= got <= -want + 0.5,
          "noise alone at reduction %d: down %.1f dB" % (want, -got))

# the gate: exactly f * dry once the hold is over
_, _, y = run(x, "-n", "20", "--vad", "0.5")
tail = range(RATE * 2, RATE * 4)
err = max(abs(y[i] - 0.1 * x[i]) for i in tail)
got = rms_db([y[i] for i in tail]) - rms_db([x[i] for i in tail])
check(err < 1e-6, "noise through the gate at reduction 20: %.2f dB, exactly 0.1 x dry" % got,
      "largest difference %.2e" % err)

# speech with noise under it
nz = noise(n, -50, seed=7)
mixed = [a + b for a, b in zip(s, nz)]
_, _, y = run(mixed, "-n", "30")
loud, quiet = [], []
for i, (b_in, b_out, b_sp) in enumerate(zip(blocks(mixed), blocks(y), blocks(s))):
    level = rms_db(b_sp)
    if level > -32:
        loud.append(rms_db(b_out) - rms_db(b_in))
    elif level < -58:                              # a pause: only noise
        quiet.append(rms_db(b_out) - rms_db(b_in))
speech_loss = sum(loud) / len(loud)
noise_drop = sum(quiet) / len(quiet)
check(speech_loss > -1.5,
      "speech loses %.2f dB on average over %d loud 100 ms blocks" % (-speech_loss, len(loud)))
check(noise_drop < -15,
      "the noise in %d pauses drops %.1f dB" % (len(quiet), -noise_drop))

# any other rate: the input, untouched
x = noise(44100, -20, seed=3)
data = struct.pack("<%df" % len(x), *x)
out = subprocess.run([os.path.join(HERE, "..", "bin", "t_denoise"), "-r", "44100", "-c", "1"],
                     input=data, capture_output=True).stdout
check(out == data, "at 44.1 kHz the input passes through untouched, undelayed")

sys.exit(fails)
