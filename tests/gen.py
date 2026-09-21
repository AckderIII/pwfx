#!/usr/bin/env python3
"""Raw 32-bit float test signal on stdout: noise, a sweep, bursts and silence,
so a filter's steady state, its transients and its denormal behaviour all get
exercised by the same stream."""
import math, random, struct, sys

rate = int(sys.argv[1]) if len(sys.argv) > 1 else 48000
ch = int(sys.argv[2]) if len(sys.argv) > 2 else 2
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
random.seed(20260921)

n = int(rate * secs)
out = []
for i in range(n):
    t = i / rate
    seg = int(t * 4) % 4
    if seg == 0:                                    # white noise
        v = random.uniform(-0.5, 0.5)
    elif seg == 1:                                  # log sweep 20 Hz -> 18 kHz
        k = (t % 0.25) * 4
        f = 20 * (900 ** k)
        v = 0.5 * math.sin(2 * math.pi * f * t)
    elif seg == 2:                                  # gated bursts, then silence
        v = 0.7 * math.sin(2 * math.pi * 440 * t) if (i // 2000) % 2 == 0 else 0.0
    else:                                           # very quiet, into denormals
        v = 1e-20 * math.sin(2 * math.pi * 100 * t)
    for c in range(ch):
        out.append(v * (1.0 if c == 0 else 0.7))
sys.stdout.buffer.write(struct.pack("<%df" % len(out), *out))
