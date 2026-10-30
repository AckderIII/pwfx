# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Six audio filters (`pw_lpf`, `pw_hpf`, `pw_eq`, `pw_gate`, `pw_comp`, `pw_limit`),
each a standalone PipeWire filter node built on `pw_filter`, ported from the raw
stdin/stdout reference implementations in `reference/`; plus `app/`, a Rust and
Slint desktop app that drives them as plugins.

## Commands

```sh
make                      # build all filters into bin/
make test                 # offline: DSP cores diffed against reference/, byte for byte
make check                # the above plus the live PipeWire tests (needs a daemon)
make app                  # the filters and the GUI
make app-test             # the GUI's own tests
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

The **level meters are the one path that runs the other way**: the data thread
measures, the main thread reads. That needs none of the ring machinery — the
value is one scalar and dropping an update costs nothing — so it is a plain
relaxed store per block. The level is carried as float bits in an
`_Atomic uint32_t`, not an `_Atomic double`, because 8-byte atomics are not
lock-free on every target and nothing in `process()` may take a lock. The peak
is held in dB so the fallback is a subtraction proportional to the block length
and the only transcendental is one `log10` per block.

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

**It is also event-driven — a line appears because something changed or was
asked for.** `in-level`/`out-level` are the sole exception and are therefore
off by default (`meter 0`), so a consumer that never asks for them sees the
stream it saw before they existed; `tests/protocol.py` guards both halves of
that. Anything else periodic must be opt-in the same way. A change may emit
more than one line (`lookahead` moves `latency`), so match echoes by name, not
by position — `live.py:set()` is the reference for how a consumer should read
this.

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

## The app (`app/`)

`pwfx`: chains of these filters, plus the buses they hang on, wired into the
graph. Rust, Slint, one binary. Its own README has the detail; what matters
when changing either side:

**It learns the filters from `--help`, not from a table.** `app/src/plugin.rs`
parses the output of `print_param_help()` in `src/pwf.c` back into a parameter
table -- names, ranges, units, defaults, read-only and command-line-only flags.
A new `src/pw_X.c` therefore appears in the GUI with the right knobs and no
Rust changes at all. **The flip side is that the help format is load-bearing.**
`cargo test` in `app/` parses the real binaries and asserts what comes back, so
a change to that formatter fails there rather than quietly emptying the window.

**The filter stays the authority on its own values.** The app sends what the
user asked for and then displays whatever is echoed back, so clamping,
rounding, a rejected cross-parameter combination and the second line a changed
`lookahead` drags with it need no modelling on the Rust side. Do not add a
shadow copy of a parameter's value that does not come from the filter.

**Links are declared, never commanded.** The GUI hands `app/src/pwthread.rs`
the set of links that ought to exist, addressed by node and port *name*, and
that thread re-resolves the whole set on every registry change. Ids are
reassigned on every restart and every profile change; names are not. This is
what makes an unplugged interface, a crashed filter and a restarted daemon
ordinary rather than special. A change that resolves a name once and caches the
id breaks all three.
A chain end on the system default is the same idea one level up: it stores
`@default-source`/`@default-sink`, and `Graph::lookup` turns that into whatever
the `default` metadata names in the current snapshot. Look endpoints up through
`lookup`/`resolve_name`, never `Graph::node`, or a default end reads as
unplugged. A default naming one of the app's own `pwfx.` nodes is deliberately
not followed -- see `resolve_name`.

**Four reconcile loops, no commands anywhere.** State -> processes -> bus
nodes -> links, each closing the gap between what is and what should be.
Adding a feature means adding to the state and to what the comparison derives
from it, not adding an action that does something. `PwCmd::SetNodes` is
`SetLinks` one level up: the complete set of nodes that ought to exist, not a
create or a destroy.

**The window is optional.** `pwfx --headless` (`app/src/headless.rs`) runs
the same engine and PipeWire thread from a plain loop; `boot`, `apply` and
`stop` in `main.rs` are shared by both, so anything the engine needs on every
graph change or every second has to go through them, not into a Slint timer.
Only one pwfx runs at a time (`state::lock_instance`), window or not.

**Destroy a PipeWire object by dropping its proxy.** The drop calls
`pw_proxy_destroy`, which already asks the server to destroy it;
`Core::destroy_object` on top asks twice, and the "unknown resource" error the
second request earns used to be taken for a lost connection and tore every bus
and link down. Only `-EPIPE` on the core means the connection is gone.

**A bus is one node, not a process.** `app/src/pwthread.rs` asks the daemon
for a `support.null-audio-sink` through the `adapter` factory, whose input
ports turn up again on its monitor -- so both kinds of bus have ports at both
ends and either can sit at either end of a chain. `media.class` is the whole
difference: `Audio/Source/Virtual` offers it to other applications as a
microphone, `Audio/Sink` as an output. A source bus is `node.passive` because
something always feeds it; a sink bus is not, because what plays into it has
no clock of its own. Everything about a null sink is fixed at creation, so
`NodeSpec` equality *is* the comparison -- a changed width, kind or
description destroys the node and makes it again.

Two naming rules follow, and they pull in opposite directions on purpose:

- A bus's `node.name` is `pwfx.bus.<id>` and never moves. It is what every
  chain endpoint and every hand-written `pw-link` refers to.
- Its `node.description` is what the user typed, because that is what other
  applications show. Renaming a bus therefore *does* remake the node, which
  is the opposite of renaming a chain -- and that asymmetry is the point.

`pwfx.bus.` is also the line the endpoint pickers draw: filter stages are
hidden from them, buses are offered at the top of both. `fresh_chain_id`
refuses to hand out `bus` so a chain cannot name its stages into that space.

Two consequences to keep in mind:

- A chain's channel count is `--channels`, which is command line only in the
  filters, so changing a chain's width restarts its processes. Everything else
  is live and must stay that way.
- The app's own nodes are named `pwfx.<chain>.<stage>` from ids, not from
  display names, so renaming a chain does not restart it. Keep it that way.
- The std widgets own their value: typing into a `LineEdit` or dragging a
  `Slider` writes the property, which removes the binding feeding it from the
  model. Every editor in `ui/parts.slint` therefore commits on blur as well as
  on Enter and pushes the model back with a `changed p` handler. A new editor
  has to do both, and `window_tests` in `app/src/main.rs` fails if it does not.
- **A widget can write its own value without the user touching it.**
  `ComboBoxBase::reset-current()` clamps and writes `current-index` on every
  model change, so a dropdown comes off its binding the first time its list
  shrinks past the selection and then shows an index of its own against a list
  that has since moved. Every dropdown whose value comes from the state goes
  through `Picker` in `ui/parts.slint`, which pushes the index back on
  `changed options` as well as `changed index` -- both, because the list can
  change while the index stays the same number, and that is the case the
  widget gets wrong. Do not use a bare `ComboBox` for a value the model owns.
  `Picker` *inherits* `ComboBox` rather than wrapping one, because a wrapper
  is a layout and a layout cannot give a child less than the 160px the widget
  asks for -- a narrower width on the wrapper left the box hanging out of it
  and over the next control in the row. `window_tests` measures the real
  window for that.
