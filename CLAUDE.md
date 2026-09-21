# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Six audio filters (`pw_lpf`, `pw_hpf`, `pw_eq`, `pw_gate`, `pw_comp`, `pw_limit`),
each a standalone PipeWire filter node built on `pw_filter`, ported from the raw
stdin/stdout reference implementations in `reference/`.

## Commands

```sh
make                      # build all filters into bin/
make test                 # offline: DSP cores diffed against reference/, byte for byte
make check                # the above plus the live PipeWire tests (needs a daemon)
make clean

PWF_OFFLINE_ONLY=1 tests/all.sh      # skip everything that needs a daemon
```

Running one test rather than the suite:

```sh
# one DSP comparison case, by hand -- this is exactly what run_case does
python3 tests/gen.py 48000 2 4 > /tmp/in.raw
./bin/ref/lpf -r 48000 -c 2 -n 4 -f 1000 < /tmp/in.raw > /tmp/a.raw
./bin/t_lpf   -r 48000 -c 2 -n 4 -f 1000 < /tmp/in.raw > /tmp/b.raw
cmp /tmp/a.raw /tmp/b.raw            # must be byte-identical

python3 tests/protocol.py bin/pw_eq  # stdin/stdout contract, one or more filters
python3 tests/live.py bin/pw_lpf -c 2 -f 1000 \
    -- "cutoff 500" "cutoff 500" \
    --audio "200,8000" "200=0+-0.6,8000=-72+-7"
python3 tests/stress.py bin/pw_comp -c 2
python3 tests/chain.py
```

`make` and the harnesses are driven by wildcards: a new `src/pw_X.c` and
`tests/t_X.c` are picked up with no Makefile edit. Add the comparison cases for
a new filter to `tests/cases.sh`, and its live expectations to `tests/all.sh`.

Filters do not auto-connect (they are plain filter nodes with no `media.class`),
so wire them by hand: `pw-link some-source pw_lpf`. When driving them from
`pw-cat` in a test, pass `--target 0` or the session manager grabs the stream
and links it to a real device instead.

## Architecture

`src/pwf.{h,c}` is the whole program; each `src/pw_*.c` is a `pwf_desc` of
callbacks plus a parameter table. `pwf_run()` parses argv from that table,
creates the node with N mono DSP port pairs, prints the parameters, polls stdin,
and calls into the filter. A filter file contains no PipeWire code.

**The split that matters is main thread vs. realtime thread.** Nothing mutable
is shared:

- `design()` runs on the main thread, on every parameter or rate change. It
  fills a **plain-data** coefficient blob — no pointers, it is zeroed and
  written into a slot of a ring — and returns the latency in samples.
- `process()` runs on the realtime thread. It acquire-loads a published counter,
  reads that slot, and never writes to it.

Consequences for any new code:

- **Never allocate, lock or block in `process()`.** Anything whose size depends
  on a parameter (delay lines, lookahead windows) is allocated once in `init()`
  for the worst case — `max-rate` at the largest legal setting — and the blob
  carries only the length currently in use.
- The published counter doubles as the change signal: `process()` receives
  `changed != 0` on the first call that sees a new blob. That is where a filter
  resets whatever geometry moved (sections that just became active, a boxcar
  whose length changed). Resetting everything on any change clicks; reset only
  what actually moved.
- `validate()` sees the **filter's** parameter table only, so `pwf_value(p, ...)`
  resolves filter parameters, not core ones (`channels`, `rate`, `bypass`, ...).
  It is for cross-parameter constraints; ranges are checked by the framework.
- `design()` must also clamp defensively. The graph can drop to a rate that puts
  a validated cutoff above Nyquist, and `validate()` cannot see that coming.

**stdout is a protocol, not a log.** Every line is either `name value` or a
`#`-comment; errors go to stderr. Anything printed from filter code must go
through that, or a controller parsing stdout breaks. Multi-line help text is why
`fput_prefixed()` exists. stdout is set line-buffered in `pwf_run()`; do not
reintroduce a code path that writes without a newline and expects it to arrive.

**Latency is real and must be reported.** The lookahead filters delay their
output by L samples and publish it as `SPA_PARAM_ProcessLatency` (the return
value of `design()`), so the graph compensates. Changing a lookahead live steps
the delay line and is audible — that is accepted and documented, not a bug.

## The reference implementations are the ground truth

`reference/*.c` are the originals. The DSP in `src/` is a port, not a rewrite:
same coefficients, same transposed direct form II, same double-precision state,
same denormal flushing. `make test` proves it by running each filter's actual
core through the reference's stream handling and `cmp`-ing against the reference
binary — currently 44 cases across orders, block sizes, sidechain settings and
edges, all byte-identical, plus four that drive the limiter hard into its
ceiling and insist the breach count stays zero.

**Any DSP change must keep `make test` byte-identical, or be a deliberate,
stated divergence** — do not "fix" a difference by loosening the comparison.

This is why the lookahead filters keep their core in a header (`gate_dsp.h`,
`comp_dsp.h`, `limit_dsp.h`) shared between `src/pw_*.c` and `tests/t_*.c`:
`*_run()` delays its output, and `tests/t_*.c` reinstates the reference's
absorb-and-flush (drop the first L outputs, feed L frames of silence at EOF)
around the same code so the two streams line up. `pw_lpf`/`pw_hpf`/`pw_eq` have
no delay line and use `src/dsp.h` directly.

`tests/live.py` measures rather than asserts liveness: it plays a test signal
into the input ports, records the output ports, and compares the level at each
probe tone against what the settings predict — 24 dB/octave per order for the
Butterworths, the static curve for the compressor, the ceiling for the limiter.
When adding a filter, add an expectation derived from theory, not from whatever
the code currently produces.

## Parameter shapes that differ from the reference CLI

Deliberate, to suit a one-value-per-line protocol — preserve them:

- `pw_eq` splits each band into `b3-freq` / `b3-gain` / `b3-q` instead of the
  reference's packed `--b3 2500:3:0.9`.
- `pw_gate` takes `hysteresis` (the gap below the open threshold) instead of the
  reference's absolute `--close`, so moving the threshold carries the close
  point with it. `--close -41` with `-t -35` is `hysteresis 6` here.
- Sample rate is read-only and comes from the graph; there is no `-r`.
- There is no `-b/--block`; the quantum is the graph's, requested via
  `--node-latency`.
