# pwfx — a patchbay for the `pw_*` filters

A Rust/Slint desktop app that treats the filter binaries in `../bin` as
plugins. It shows every source and sink on the system, lets you build any
number of filter chains out of them, gives you virtual devices to hang them
on, and keeps the PipeWire graph wired to match.

```sh
make app          # from the top of the tree; builds the filters too
./app/target/release/pwfx
```

![the window](doc/window.png)

Or with no window at all, which is how a microphone chain wants to run once
it is set up:

```sh
./app/target/release/pwfx --headless            # what the window last left
./app/target/release/pwfx --headless podcast     # a preset, or a path to a file
```

See [Running without the window](#running-without-the-window).

## What it does

- **Finds the filters itself.** Every `pw_*` program on the search path is
  asked for its `--help` and the reply becomes a parameter table. A seventh
  filter dropped into `bin/` shows up in the "Add filter" list with the right
  knobs, ranges, units and defaults, and nothing in this app has to be
  recompiled. See [`src/plugin.rs`](src/plugin.rs).
- **Chains.** A chain is a source, an ordered row of filters and a sink. Each
  stage runs as its own process, and the stages are linked in series. Stages
  can be added, removed, reordered and bypassed while audio flows.
- **Routing.** Sources and sinks are chosen from the live graph — buses,
  hardware inputs and outputs, virtual devices, sink monitors and application
  streams. A chain with no filters in it at all is a plain patch cable.
- **Following the default.** Either end can be **Default input** or
  **Default output** instead of a device. That end goes wherever the
  session manager's default goes, so a chain into "the default output"
  moves to the headphones when they become the default and back again
  when they are unplugged. The picker and the line under it say what the
  default is right now. A default that is one of this app's own buses is
  not followed: making a source bus the system microphone is the normal
  thing to do, and a chain taking "the default microphone" would then be
  fed from its own output.
- **Buses.** A bus is a virtual device the app asks the daemon for, and it is
  what other applications on the machine talk to. A **source** bus is a
  microphone: chains feed it, and your conferencing app records from it. A
  **sink** bus is an output: applications play into it, and chains take the
  audio back off it. Any number of chains can share one bus in either
  direction — PipeWire sums what arrives — so a bus is where several chains
  become one thing the rest of the system can select. Buses go when the app
  does; nothing is left behind in the graph.
- **Applications on a bus.** A bus can name the applications that go to it:
  their sound into a sink bus, or their recording out of a source bus --
  the bus's kind decides which, since that is the only way the session
  manager will link a stream to it. Pick an application while it is
  running and every stream it opens is sent there, now and each time it
  starts. It goes back to its usual device when it is removed from the
  bus, when the bus is switched off, or when pwfx stops; and moving one
  somewhere else yourself, in pavucontrol say, is left alone until it next
  starts. One application goes to one bus of each kind, so adding it to a
  second moves it.
  An application with more than one stream going that way also offers each
  stream on its own, by its name -- OBS's are its audio sources, "Mic/Aux",
  "Desktop Audio" -- so one source can go to a bus while the rest stay where
  they are. A rule for one stream wins over one for its whole application,
  so the two can sit on different buses. (A stream is named by `media.name`,
  not `node.name`: OBS gives every one of its streams the `node.name` "OBS".)
- **Sidechains.** A filter with a sidechain input (`pw_sccomp`, so far)
  gets a **sidechain** picker on its stage card, offering everything a
  chain's input could take: devices, sink monitors, application streams,
  buses and the default input. A stereo source is folded onto the one mono
  port, both sides summed. To key one chain from another -- duck the music
  under the processed voice -- send the voice chain into a bus as well and
  pick that bus. Like every other end, it is stored by name and comes back
  when the device does, and a chain keyed from a bus is listed among the
  bus's users.
- **Channels.** A chain is mono, stereo or up to 64 channels wide. Matching
  the world to it is automatic: same-named channels line up (so FL/FR find
  FL/FR whatever order the device declares them in), and anything else falls
  back to position, wrapping on both sides — which is what makes a mono mic
  feed both sides of a stereo chain and a stereo source fold down into a mono
  one. Any pairing can be overridden in the channel matrix.
- **Undo and redo.** Every edit can be undone -- ↶ and ↷ in the toolbar,
  or Ctrl+Z and Ctrl+Shift+Z (Ctrl+Y too) -- and the log says what was
  undone: "undid: delete chain Voice". A slider drag is one step, not one
  per pixel, and loading a preset or restoring the default is a step like
  any other, so an accidental one comes back. Undoing applies the earlier
  state as loading a preset does: only what it moved restarts. In a text
  field Ctrl+Z belongs to the field.
- **Saving.** The session is written out as you work. Named presets and a
  "default state" that **Restore default** goes back to are the same file
  format, so they are interchangeable and editable by hand.
- **Response curves.** Every stage draws what it does under its knobs: the
  frequency response of a high-pass, low-pass or EQ, on a log axis from
  20 Hz to 20 kHz, and the static input-to-output curve of a compressor,
  gate or limiter, square, so a ratio reads as a slope and a gate's
  hysteresis as a loop. The filter computes it from the coefficients it is
  running and the app only draws the points, asking again whenever the
  filter reports a change.
- **Meters.** Every stage shows the input and output peak levels the filter
  reports, as a bar and as a figure in dBFS, and the latency it adds. The
  figure is what the filter last reported: a peak hold that falls back at
  20 dB/s, so the highest reading over a run is the one to trust. Silence
  reads as `—` rather than as the filters' -240 dB floor. A gate, compressor,
  limiter, de-esser or noise suppressor shows a third strip below those two, **gr**: how far it is
  pulling the level down, as a bar growing from the right over 24 dB and a figure such as
  `-14.7`. The leveler, which turns up as well as down, shows **gain** in
  that place instead: a bar from the middle, right for a boost and left
  for a cut, over 24 dB each way, and a signed figure such as `+5.5`. A
  stage with a sidechain input shows one more strip above
  that, **sc**: the peak level arriving at the sidechain, read like in
  and out. Which stages get any of these comes from the filter's `--help`, like
  everything else here. A chain shows the sum of its stages' latency, in
  its header and beside its name in the list: what its filters report,
  which is what the graph compensates for. `pw_delay` adds none to it --
  its delay is the effect.

## Running without the window

`pwfx --headless [FILE]` runs the same engine and the same PipeWire thread
as the window, driven by a plain loop instead of the window's timers.
Without a FILE it runs what the window last left, `session.json`, falling
back to `default.json`; with one, that file or the preset of that name.

- **It never saves.** The file it started from is the authority. Edit that
  file, or close the service, use the window and start it again.
- **SIGHUP reloads.** The re-read state goes through the same comparison as
  a loaded preset, so only what moved restarts and the rest keeps playing.
  A file that no longer reads is reported and what is running stays.
- **SIGTERM or Ctrl-C stops** it in the same order the window does: links,
  then filters, then buses.
- **A file that will not read is fatal at start**, unlike in the window. A
  window can start empty and say why; a service that starts empty looks
  like it works and does nothing.
- The meters are off, since there is nobody to look at them.
- Everything goes to stderr, which under systemd is the journal: each
  filter's state, links going down and coming back as devices leave and
  return, and any error.

**One pwfx at a time.** Two would ask for the same node names and fight
over them, so the window and the headless mode both take a lock in
`$XDG_RUNTIME_DIR`, and the second to start refuses and names the first's
pid. A refused window says so in a dialog as well as on stderr, since from a
launcher nobody is reading stderr; it looks at the holder's command line to
tell another window, which is to be switched to, from the headless mode,
which is to be stopped first. The kernel drops the lock with the process, so a crash leaves nothing
stale behind.

[`pwfx.service`](pwfx.service) is a systemd user unit for running it from
login; its comments have the three commands to install it by hand.
`pwfx --install-service` writes it into `~/.config/systemd/user` with
`ExecStart=` naming the binary or AppImage it was run from, and prints the
`systemctl` lines that enable it. It does not run them: a service started
while the window is open would only be refused.

## How it is put together

One loop, repeated at four levels, and nothing else:

```
  AppState          what the user asked for        src/state.rs
     │ Engine::apply
  processes         one filter binary per stage    src/process.rs
     │ PwCmd::SetNodes
  bus nodes         one null sink per bus          src/pwthread.rs
     │ PwCmd::SetLinks
  links             resolved by name, per tick     src/pwthread.rs
```

Each level compares what *is* against what *should be* and closes the gap.
Every user action is therefore the same action — edit the state, then apply
it — and the awkward cases are not special-cased anywhere, because they are
just the next comparison coming out differently:

| what happens | what makes it work |
|---|---|
| an interface is unplugged | its node leaves the registry, its links stop resolving, the rest of the chain keeps running |
| it is plugged back in | the name resolves again and the links are remade |
| a filter crashes | the next pass sees it is gone and starts it again, with backoff if it will not stay up |
| the daemon restarts | the PipeWire thread reconnects and replays the wanted links |
| a preset is loaded | it is compared against what is running; stages that did not change are left alone, audio and all |
| the default device changes | a chain on the default stores `@default-sink`, not a device; the new name comes in with the graph and the links are worked out again |
| an application restarts | its new stream is pointed at the bus again; it is told apart from the old one by `object.serial`, because the id is often the same |
| a bus is renamed | its `node.description` moved, so the node is made again under the new one; its `node.name` did not, so the chains on it never notice |

Two rules follow from the filters' own design and are worth knowing before
changing anything here:

**The filter is the authority on its own parameters.** The app sends what the
user asked for and displays what comes back. Rounding, clamping, a rejected
cross-parameter combination, and the extra `latency` line that a changed
lookahead drags with it all land in the window without it modelling any of
them — see `Engine::absorb_reports`. A value the filter refuses simply never
moves on screen, and the reason appears in the log.

**Links are declared, not commanded.** The GUI never says "connect this to
that". It hands the PipeWire thread the set of links that ought to exist, by
node and port *name*, and that thread re-resolves the whole set on every
registry change. Ids change constantly — every restart, every profile switch —
and names do not.

## Layout

| file | |
|---|---|
| `src/plugin.rs` | discovers the binaries and parses `--help` into a parameter table |
| `src/process.rs` | one running filter and the line protocol on its pipes |
| `src/graph.rs` | the PipeWire graph as the GUI sees it, and the channel matching |
| `src/pwthread.rs` | the PipeWire thread: registry, snapshots, node and link reconciliation |
| `src/engine.rs` | state → processes → wanted buses and links |
| `src/state.rs` | the save file, which is the whole user-visible model |
| `src/ui.rs` | slider positions and value formatting |
| `src/main.rs` | the command line, and the window: state into models, clicks into edits |
| `src/install.rs` | `--install-desktop` and `--install-service`: the desktop file, icon and systemd unit, written where they are looked for |
| `src/headless.rs` | the same, with no window: a loop, signals, and the save file read but never written |
| `ui/*.slint` | the window itself |

## As an AppImage

```sh
make appimage     # from the top of the tree; needs podman
./dist/pwfx-*-x86_64.AppImage
./dist/pwfx-*-x86_64.AppImage --headless
./dist/pwfx-*-x86_64.AppImage --install-desktop
```

`--install-desktop` puts `pwfx.desktop` and the icon into `~/.local/share`
(`$XDG_DATA_HOME`), with `Exec=` naming the AppImage. That is what lets the
desktop show the window's name and icon: the window's app id is `pwfx`, and
GNOME matches it against desktop files in the XDG data directories only, which
the copy inside the image is not in. Run it again after moving the AppImage;
delete the two files it names to undo it. An integration tool such as Gear
Lever does the same.

One file holding `pwfx` and the ten filters. It is built in a Debian 12
container, so it runs on any system with glibc 2.36 and PipeWire 0.3.65 or
newer -- Debian 12, Ubuntu 23.04 and 24.04 LTS, Fedora 37 and everything
since -- and needs no `libfuse2`. Not Ubuntu 22.04: its PipeWire 0.3.48 is
older than the app's PipeWire bindings support. It carries no libraries: PipeWire, the graphics stack and the
fonts are the system's, which is how they have to be found anyway.

For headless mode from login, `./pwfx-*.AppImage --install-service` writes the
systemd unit pointing at the AppImage; enable it as it says. The filters are found
inside the image the same way they are found next to the binary in `bin/`.

The build runs `make test` inside the container too, so the filters in the
AppImage have been proven byte-identical to their references under the
compiler that built them; and it starts the result on a stock Debian 12 with
nothing but `libpipewire-0.3-0` installed, to check it runs and finds its
filters.

## Environment

| | |
|---|---|
| `PWFX_FILTER_PATH` | colon-separated directories to look for `pw_*` in. Otherwise: next to the executable, then the tree's own `bin/`. |
| `PWFX_CONFIG_DIR` | where to keep the save files. Otherwise `$XDG_CONFIG_HOME/pwfx`, or `~/.config/pwfx`. |

Save files: `session.json` (written as you work, restored at start),
`default.json` (**Set default** / **Restore default**, and what a first
run starts from), `presets/*.json`.

## Notes and limits

- Changing a chain's **width** restarts its filters: the channel count is a
  command-line parameter in them, not a live one. Everything else is live.
- Changing a **lookahead** steps the filter's delay line and is audible. That
  is the filters' documented behaviour, not something this app smooths over —
  set it and leave it.
- Chains cannot be fed from other chains directly — the filter stages are
  kept out of the endpoint pickers — but they can be fed **through a bus**,
  which is the supported way to join them up. Nothing stops you wiring a
  chain's output back round to its own input through one; the app does not
  detect cycles and PipeWire will simply refuse to run the loop.
- Changing a bus's **width** or its **kind** makes the node again, because
  both are fixed when a null sink is created. Any application streaming
  through it at the time is moved off it by the session manager.
- Links and buses alike are made without `object.linger`, so closing the app
  removes exactly what it made and leaves the rest of the graph as it found
  it. The filter processes get `PR_SET_PDEATHSIG` as well, so they do not
  outlive the GUI even if it is killed.

## A trap worth knowing about

The std widgets own their own value. Typing into a `LineEdit` or dragging a
`Slider` writes the property, and in Slint writing a property **removes the
binding that was feeding it**. From that moment the widget shows what the user
did rather than what the model says, and nothing gets it back in step.

That cost a bug: `LineEdit::accepted` only fires on Enter, so a figure typed
and then clicked away from was displayed but never committed. It looked saved
until the next start, and it was worst on `pw_eq`, which is sixteen numeric
fields and whose defaults are a passthrough — so a lost setting means the EQ
audibly does nothing.

So every editor in `ui/parts.slint` does two things, and a new one has to do
both: commit on losing focus as well as on Enter, and push the model back into
the widget with a `changed p` handler whenever it moves — but never into a
widget that currently has focus, which would fight the user mid-edit. Reading
the model back straight after committing is what makes a value the filter
clamped or refused correct itself on screen.

The dropdowns cost a second bug, because a widget does not need the user's
help to write its own value. `ComboBoxBase::reset-current()` clamps and writes
`current-index` **every time the model changes**, which is often here — every
device that appears and every application that starts playing is a new entry
in the endpoint lists, and a daemon restart empties them and fills them again.
The write is a no-op while the clamp does not move the value, so nothing goes
wrong for a while; the first time a list shrinks past the selection the
dropdown comes off its binding for good and shows whatever index it kept, read
against a list that has since grown back. A chain that was plugged in reads as
plugged into something else, often the first entry.

Every dropdown whose value belongs to the state therefore goes through
`Picker`, which pushes the index back on `changed options` as well as on
`changed index` — both, because the list can change while the index stays the
same number, and that is precisely the case the widget gets wrong on its own.

`Picker` inherits `ComboBox` rather than wrapping one, and that detail is
load-bearing too: a wrapper is a layout, and a layout cannot give its child
less than the 160px the widget asks for. Written that way, `width: 150px` on
the preset dropdown left the box hanging 10px out of its slot and over the
**Load** button beside it. So the tests measure the real window — every
dropdown keeps a row's worth of space between itself and whatever follows.

## Tests

```sh
cargo test
```

75 of them, and one more that is not a test but a way to look: run
`PWFX_RENDER_OUT=/tmp/w.rgba cargo test render_the_window -- --ignored` and
it renders the window, with real filters and their curves, to raw RGBA
off-screen. Five of the tests are worth pointing at:

- `window_tests` drives the real widgets headlessly through Slint's testing
  backend — it clicks into a field, types, clicks away, and asserts the value
  was committed and that the field still follows the model afterwards; and it
  shrinks an endpoint list past the selection and grows it back, asserting the
  dropdown still names the right node; and it measures the laid-out window to
  catch a dropdown overlapping the control beside it. These are the regression
  tests for the traps above; each fails if its half is removed.
- `engine::round_trip` starts a real `pw_eq`, feeds it a saved chain and
  asserts the filter reports back exactly what was asked for, then that
  adopting its report and a JSON cycle both leave the state alone. Needs a
  PipeWire daemon; skips without one.
- `plugin::against_the_real_filters` parses the actual binaries in `bin/`, so
  a change to the help formatter in `src/pwf.c` fails here rather than
  silently emptying the window.
- `engine::tests` pins the exact link set each routing case should produce,
  including mono fan-out, stereo fold-down, an absent device, and two chains
  summing into one bus — plus the shape of the node each bus asks for.
- `picker_tests` pins the one place a bus has to behave unlike the app's
  other nodes: the endpoint pickers hide filter stages and offer buses,
  although both carry the `pwfx.` prefix.

## Building without `fontconfig-devel`

`Cargo.toml` depends on `fontique` purely to turn on its `fontconfig-dlopen`
feature. Slint reaches fontconfig through that crate, whose default path wants
`fontconfig.pc` at build time; loading `libfontconfig` at runtime instead keeps
the build free of system `-devel` packages. Nothing here calls fontique.
