//! pwfx -- a patchbay for the `pw_*` filter nodes.
//!
//! The shape of the program is one loop repeated at four levels, and nothing
//! else:
//!
//! ```text
//!   AppState          what the user asked for          (src/state.rs)
//!      | Engine::apply
//!   processes         one filter binary per stage      (src/process.rs)
//!      | PwCmd::SetNodes
//!   bus nodes         one null sink per bus            (src/pwthread.rs)
//!      | PwCmd::SetLinks
//!   links             resolved by name, every tick     (src/pwthread.rs)
//! ```
//!
//! Each level compares what is with what should be and closes the gap, so
//! every user action is the same action -- edit the state and apply it -- and
//! a device being unplugged, a filter crashing or the daemon restarting are
//! not special cases anywhere. They are just the next comparison coming out
//! differently.
//!
//! This file is the window: it renders the state into models and turns clicks
//! back into edits. It holds no state of its own beyond what is being looked
//! at.

mod engine;
mod graph;
mod headless;
mod install;
mod history;
mod plugin;
mod process;
mod pwthread;
mod state;
mod ui;

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::mpsc::{channel, Receiver};
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;
use std::time::Duration;

use slint::{ComponentHandle, Model, ModelRc, SharedString, VecModel};

use engine::Engine;
use graph::{Dir, Mapping};
use plugin::ParamKind;
use process::ProcEvent;
use pwthread::PwEvent;
use state::{Bus, BusKind, Chain, Endpoint, Stage};

slint::include_modules!();

/// How often events from the filter processes and the PipeWire thread are
/// drained into the window. Meters report every 80 ms, so this is what makes
/// them look continuous without redrawing between readings.
const UI_TICK: Duration = Duration::from_millis(40);

/// The slower pass: notice a filter that died, adopt values the filters
/// clamped, and write the session out if it moved.
const HOUSEKEEPING: Duration = Duration::from_millis(1000);

enum AppEvent {
    Proc(ProcEvent),
    Pw(PwEvent),
}

/// Which of the two lists on the left the detail pane is showing. They are
/// different things -- a chain is processes and links, a bus is one node --
/// but only one of them is being looked at at a time.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
enum Focus {
    #[default]
    Chain,
    Bus,
}

/// What is being looked at, as opposed to what is being run.
#[derive(Default)]
struct View {
    focus: Focus,
    selected: usize,
    selected_bus: usize,
    /// `node.name` for each entry of the endpoint pickers, parallel to the
    /// strings the window shows. Entry 0 is "none" and is empty.
    source_nodes: Vec<String>,
    sink_nodes: Vec<String>,
    /// Likewise for the sidechain pickers on the stage cards, which all
    /// share one list.
    sidechain_nodes: Vec<String>,
    /// Which end the channel matrix is open on, if any.
    map: Option<Dir>,
    /// Port names in the matrix's row order.
    map_ports: Vec<String>,
    presets: Vec<String>,
    /// The application picked in the bus pane's "add" list, by name, and
    /// the names behind that list's entries. Entry 0 is the prompt.
    bus_app_pick: Option<String>,
    bus_app_names: Vec<String>,
}

#[derive(Clone)]
struct App {
    engine: Rc<RefCell<Engine>>,
    view: Rc<RefCell<View>>,
    win: slint::Weak<AppWindow>,
    pw: Rc<pwthread::PwHandle>,
    dirty: Rc<Cell<bool>>,
    models: Rc<RefCell<Models>>,
    /// The last (wanted, made) the PipeWire thread reported, for the status
    /// line. Kept here rather than written straight to the window so that any
    /// refresh shows the current figure rather than the one from whenever a
    /// link last moved.
    links: Rc<Cell<(usize, usize)>>,
    history: Rc<RefCell<history::History>>,
}

const USAGE: &str = "\
usage: pwfx                     open the window
       pwfx --headless [FILE]   run the chains and buses with no window
       pwfx --install-desktop   add pwfx to the desktop's applications
       pwfx --install-service   add a systemd user unit for --headless

FILE is a save file, or the name of a preset. Without one, --headless runs
what the window last left: session.json, or default.json if there is none.

With no window, nothing is saved. SIGHUP reads FILE again and changes only
what moved in it; SIGTERM or Ctrl-C stops everything and removes the buses.

--install-desktop writes pwfx.desktop and its icon into ~/.local/share
(XDG_DATA_HOME), starting this AppImage or binary: the desktop then shows the
window's name and icon. --install-service writes pwfx.service into
~/.config/systemd/user (XDG_CONFIG_HOME), running this file --headless, and
leaves enabling it to you. Run either again after moving the file.

Only one pwfx runs at a time, window or not.";

enum Mode {
    Window,
    Headless(Option<PathBuf>),
    InstallDesktop,
    InstallService,
    Help,
}

fn parse_args(args: &[String]) -> Result<Mode, String> {
    match args {
        [] => Ok(Mode::Window),
        [h] if h == "-h" || h == "--help" => Ok(Mode::Help),
        [h] if h == "--headless" => Ok(Mode::Headless(None)),
        [h] if h == "--install-desktop" => Ok(Mode::InstallDesktop),
        [h] if h == "--install-service" => Ok(Mode::InstallService),
        [h, file] if h == "--headless" && !file.starts_with('-') => {
            Ok(Mode::Headless(Some(file.into())))
        }
        _ => Err(format!("unexpected arguments: {}", args.join(" "))),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mode = match parse_args(&args) {
        Ok(Mode::Help) => {
            // Not println!, which panics when the reader has gone:
            // `pwfx --help | head` is not a reason to crash.
            use std::io::Write;
            let _ = writeln!(std::io::stdout(), "{USAGE}");
            return ExitCode::SUCCESS;
        }
        // These touch no state and no graph, so they need no lock: they can
        // be run while pwfx is open.
        Ok(mode @ (Mode::InstallDesktop | Mode::InstallService)) => {
            let service = matches!(mode, Mode::InstallService);
            let done = if service { install::service() } else { install::desktop() };
            return match done {
                Ok(paths) => {
                    for p in paths {
                        println!("wrote {}", p.display());
                    }
                    if service {
                        println!("to start it from login:\n  \
                                  systemctl --user daemon-reload\n  \
                                  systemctl --user enable --now pwfx\n\
                                  (only one pwfx runs at a time: close the window first)");
                    }
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("pwfx: {e}");
                    ExitCode::FAILURE
                }
            };
        }
        Ok(mode) => mode,
        Err(e) => {
            eprintln!("pwfx: {e}\n\n{USAGE}");
            return ExitCode::from(2);
        }
    };
    // Held until main returns, which is after everything has been stopped.
    let _lock = match state::lock_instance() {
        Ok(f) => f,
        Err(e) => {
            eprintln!("pwfx: {e}");
            if let (Mode::Window, state::LockError::Held(pid)) = (&mode, &e) {
                show_refusal(*pid);
            }
            return ExitCode::FAILURE;
        }
    };
    match mode {
        Mode::Headless(file) => headless::run(file),
        _ => {
            window();
            ExitCode::SUCCESS
        }
    }
}

/// Name the window after `pwfx.desktop`, which is how the desktop finds its
/// name and icon: on Wayland the app id is the only link between a window and
/// a desktop file, and without one GNOME shows a generic icon and "Unknown".
/// Needs the platform, so it comes after the first component is created, and
/// takes effect when the window is shown.
fn set_app_id() {
    let _ = slint::set_xdg_app_id("pwfx");
}

/// Say why the window is not opening, in a window: started from a launcher
/// there is nobody reading stderr, and a second launch that silently does
/// nothing looks broken. Best effort -- with no display to put it on, the
/// line already on stderr is all there is.
fn show_refusal(pid: Option<u32>) {
    let (heading, body) = refusal(pid, |pid| {
        std::fs::read(format!("/proc/{pid}/cmdline")).map(|raw| {
            raw.split(|b| *b == 0)
                .any(|arg| arg == b"--headless")
        }).ok()
    });
    let Ok(win) = RefusedWindow::new() else { return };
    set_app_id();
    win.set_heading(heading.into());
    win.set_body(body.into());
    let weak = win.as_weak();
    win.on_dismiss(move || {
        if let Some(w) = weak.upgrade() {
            let _ = w.hide();
        }
    });
    let _ = win.run();
}

/// The dialog's heading and text. What to do about it depends on what holds
/// the lock: another window is to be switched to, the headless mode is to be
/// stopped. `headless` says which, or `None` when it cannot be told.
fn refusal(pid: Option<u32>, headless: impl Fn(u32) -> Option<bool>) -> (String, String) {
    const WHY: &str = "Only one pwfx can run at a time, because two would create \
                       the same nodes and fight over them.";
    let Some(pid) = pid else {
        return ("pwfx is already running".into(), WHY.into());
    };
    match headless(pid) {
        Some(true) => (
            "pwfx is running without a window".into(),
            format!(
                "It is already running your chains in the background (pid {pid}). \
                 {WHY}\n\n\
                 To open the window, stop it first: systemctl --user stop pwfx \
                 if it runs as a service, otherwise kill {pid}. Start it again \
                 after closing the window to run what the window saved."
            ),
        ),
        Some(false) => (
            "pwfx is already open".into(),
            format!("Another pwfx window is open (pid {pid}). {WHY}"),
        ),
        None => (
            "pwfx is already running".into(),
            format!("Another pwfx is running (pid {pid}). {WHY}"),
        ),
    }
}

/// What both modes start the same way: the filters found, the engine built
/// around the given state, and the PipeWire thread started, with everything
/// either of them reports arriving on the one receiver.
fn boot(
    state: state::AppState,
    load_error: Option<String>,
) -> (Engine, pwthread::PwHandle, Receiver<AppEvent>) {
    let dirs = plugin::search_path();
    let (plugins, errors) = plugin::discover(&dirs);

    let (tx, rx) = channel::<AppEvent>();
    let proc_sink: process::EventSink = {
        let tx = tx.clone();
        Arc::new(move |e| {
            let _ = tx.send(AppEvent::Proc(e));
        })
    };
    let pw = pwthread::start({
        let tx = tx.clone();
        move |e| {
            let _ = tx.send(AppEvent::Pw(e));
        }
    });

    let mut engine = Engine::new(plugins, state, proc_sink);
    if engine.plugins.is_empty() {
        engine.note(
            format!(
                "no pw_* filters found in {}. Build them with `make`, or set PWFX_FILTER_PATH.",
                dirs.iter()
                    .map(|d| d.display().to_string())
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
            true,
        );
    }
    for e in errors {
        engine.note(e, true);
    }
    if let Some(e) = load_error {
        engine.note(e, true);
    }
    (engine, pw, rx)
}

fn window() {
    let (state, load_error) = state::startup();
    let (engine, pw, rx) = boot(state, load_error);

    let win = AppWindow::new().expect("cannot create the window");
    set_app_id();
    let app = App {
        engine: Rc::new(RefCell::new(engine)),
        view: Rc::new(RefCell::new(View {
            presets: state::list_presets(),
            ..Default::default()
        })),
        win: win.as_weak(),
        pw: Rc::new(pw),
        dirty: Rc::new(Cell::new(false)),
        models: Rc::new(RefCell::new(Models::default())),
        links: Rc::new(Cell::new((0, 0))),
        history: Rc::new(RefCell::new(history::History::default())),
    };

    app.wire(&win);
    app.apply();
    app.refresh();

    let _ticker = app.start_ticker(rx);
    let _keeper = app.start_housekeeping();

    win.run().expect("the event loop failed");

    app.save_session();
    stop(&mut app.engine.borrow_mut(), &app.pw);
}

/// Reconcile the processes, and declare the buses and the links the state
/// implies. The buses go first, so that a chain and the bus it was just
/// pointed at cannot arrive in the wrong order.
fn apply(engine: &mut Engine, pw: &pwthread::PwHandle) {
    let (nodes, links) = engine.apply();
    pw.declare_nodes(nodes);
    pw.declare_links(links);
    pw.declare_routes(engine.desired_routes());
}

/// Leave, in order: the links before the filters, so the graph never sees a
/// node disappear out from under a link it is still carrying, and the buses
/// last, for the same reason one level up. They would all go anyway when the
/// PipeWire thread drops its proxies, so this is only about the order.
fn stop(engine: &mut Engine, pw: &pwthread::PwHandle) {
    // Applications back where the session manager would put them, before
    // the buses they were sent to go.
    pw.declare_routes(Vec::new());
    pw.declare_links(Vec::new());
    engine.shutdown();
    pw.declare_nodes(Vec::new());
}

impl App {
    // ------------------------------------------------------------------
    // the two loops

    fn start_ticker(&self, rx: Receiver<AppEvent>) -> slint::Timer {
        let app = self.clone();
        let timer = slint::Timer::default();
        timer.start(slint::TimerMode::Repeated, UI_TICK, move || {
            let mut full = false;
            let mut meters = false;
            let mut relink = false;
            let mut status = false;

            for ev in rx.try_iter() {
                match ev {
                    AppEvent::Proc(p) => {
                        // A meter reading moves one number; anything else can
                        // move a value the window is showing.
                        let level = is_meter_reading(&p);
                        app.engine.borrow_mut().on_proc_event(p);
                        if level {
                            meters = true;
                        } else {
                            full = true;
                        }
                    }
                    AppEvent::Pw(PwEvent::Graph(g)) => {
                        app.engine.borrow_mut().set_graph(g);
                        full = true;
                        relink = true;
                    }
                    AppEvent::Pw(PwEvent::Links { wanted, made }) => {
                        app.links.set((wanted, made));
                        status = true;
                    }
                    AppEvent::Pw(PwEvent::Error(e)) => {
                        app.engine.borrow_mut().note(e, true);
                        full = true;
                    }
                }
            }

            if relink {
                // The graph moved: a device may have appeared, which changes
                // what an automatic channel mapping resolves to.
                app.apply();
            }
            if full {
                // Whatever just came in may have moved a filter's response;
                // one request per filter per pass, however many lines it was.
                app.engine.borrow_mut().request_curves();
            }
            if full || status {
                app.refresh();
            } else if meters {
                app.refresh_meters();
            }
        });
        timer
    }

    fn start_housekeeping(&self) -> slint::Timer {
        let app = self.clone();
        let timer = slint::Timer::default();
        timer.start(slint::TimerMode::Repeated, HOUSEKEEPING, move || {
            // A filter that died is restarted by the same comparison that
            // started it in the first place.
            app.apply();
            if app.engine.borrow_mut().absorb_reports() {
                app.dirty.set(true);
                app.refresh();
            }
            if app.dirty.replace(false) {
                app.save_session();
            }
        });
        timer
    }

    fn apply(&self) {
        apply(&mut self.engine.borrow_mut(), &self.pw);
    }

    /// Every edit goes through here: change the state, then let the engine
    /// work out what that means. Whatever the edit changed becomes a step
    /// that can be undone, named by what changed (see `history::describe`).
    fn edit(&self, f: impl FnOnce(&mut Engine, &mut View)) {
        self.edit_as(None, f);
    }

    /// The same, with the step's name given rather than worked out -- for
    /// an edit that replaces the whole state, where the difference says
    /// nothing useful about why.
    fn edit_as(&self, label: Option<String>, f: impl FnOnce(&mut Engine, &mut View)) {
        let before = self.engine.borrow().state.clone();
        f(&mut self.engine.borrow_mut(), &mut self.view.borrow_mut());
        {
            let e = self.engine.borrow();
            if history::differs(&before, &e.state) {
                let (auto, key) = history::describe(&before, &e.state);
                self.history.borrow_mut().record(
                    before,
                    label.unwrap_or(auto),
                    key,
                    std::time::Instant::now(),
                );
            }
        }
        self.dirty.set(true);
        self.apply();
        self.refresh();
    }

    /// Step back, or forward. The state put back is applied like any other,
    /// so the reconcile loops restart only what the step actually moved.
    /// The meters stay as they are now: they are how the window looks, not
    /// part of what was edited.
    fn step(&self, forward: bool) {
        let current = self.engine.borrow().state.clone();
        let meters = current.meters;
        let got = if forward {
            self.history.borrow_mut().redo(current)
        } else {
            self.history.borrow_mut().undo(current)
        };
        let Some((mut st, label)) = got else { return };
        st.meters = meters;
        {
            let mut e = self.engine.borrow_mut();
            let mut v = self.view.borrow_mut();
            // What was selected may be what the step took away.
            v.selected = v.selected.min(st.chains.len().saturating_sub(1));
            v.selected_bus = v.selected_bus.min(st.buses.len().saturating_sub(1));
            v.map = None;
            e.state = st;
            e.note(format!("{}: {label}", if forward { "redid" } else { "undid" }), false);
        }
        self.dirty.set(true);
        self.apply();
        self.refresh();
    }

    fn save_session(&self) {
        let st = self.engine.borrow().state.clone();
        if let Err(e) = state::write(&state::session_path(), &st) {
            self.engine.borrow_mut().note(e, true);
        }
    }

    fn status_text(&self, e: &Engine) -> String {
        let (wanted, made) = self.links.get();
        if !e.graph.connected {
            return "not connected to PipeWire".into();
        }
        let running = e
            .state
            .chains
            .iter()
            .filter(|c| c.enabled)
            .flat_map(|c| c.stages.iter().map(move |s| c.node_name(s)))
            .filter(|n| e.runtime(n).is_some_and(|r| r.running))
            .count();
        let buses = e.state.buses.iter().filter(|b| b.enabled).count();
        let live = e
            .state
            .buses
            .iter()
            .filter(|b| b.enabled && e.graph.node(&b.node_name()).is_some())
            .count();
        format!(
            "{} node{} in the graph · {running} filter{} running · \
             {live}/{buses} bus{} up · {made}/{wanted} link{} up",
            e.graph.nodes.len(),
            plural(e.graph.nodes.len()),
            plural(running),
            if buses == 1 { "" } else { "es" },
            plural(wanted),
        )
    }

    // ------------------------------------------------------------------
    // rendering

    fn refresh(&self) {
        let Some(win) = self.win.upgrade() else { return };
        let e = self.engine.borrow();
        let mut v = self.view.borrow_mut();
        let mut m = self.models.borrow_mut();

        if v.selected >= e.state.chains.len() {
            v.selected = e.state.chains.len().saturating_sub(1);
        }
        if v.selected_bus >= e.state.buses.len() {
            v.selected_bus = e.state.buses.len().saturating_sub(1);
        }
        // Only one of the two panes is showing, and which one is what the
        // user last clicked in. The other list keeps its place but loses its
        // highlight, so it is obvious which selection the pane belongs to.
        let selected = (v.focus == Focus::Chain)
            .then(|| e.state.chains.get(v.selected))
            .flatten();
        let bus = (v.focus == Focus::Bus)
            .then(|| e.state.buses.get(v.selected_bus))
            .flatten();

        // --- the chain list ---
        let chains: Vec<ChainRow> = e
            .state
            .chains
            .iter()
            .map(|c| {
                let missing_plugin = c.stages.iter().any(|s| e.plugin(&s.plugin).is_none());
                let missing_node = [c.input.as_ref(), c.output.as_ref()]
                    .into_iter()
                    .chain(c.stages.iter().map(|s| s.sidechain.as_ref()))
                    .flatten()
                    .any(|ep| e.graph.lookup(&ep.node).is_none());
                ChainRow {
                    id: c.id.as_str().into(),
                    name: c.name.as_str().into(),
                    enabled: c.enabled,
                    detail: chain_detail(c, e.chain_latency_ms(c)).into(),
                    problem: missing_plugin || missing_node,
                }
            })
            .collect();
        let key: Vec<String> = chains
            .iter()
            .map(|c| format!("{}|{}|{}|{}|{}", c.id, c.name, c.enabled, c.detail, c.problem))
            .collect();
        if m.chains_key != key {
            win.set_chains(ModelRc::new(VecModel::from(chains)));
            m.chains_key = key;
        }
        win.set_selected(if selected.is_some() { v.selected as i32 } else { -1 });
        win.set_has_chain(selected.is_some());

        // --- the bus list ---
        let buses: Vec<BusRow> = e
            .state
            .buses
            .iter()
            .map(|b| BusRow {
                id: b.id.as_str().into(),
                name: b.name.as_str().into(),
                enabled: b.enabled,
                detail: bus_detail(b).into(),
                // Asked for, the daemon is there, and yet it is not in the
                // graph: something refused to make it, and the log says what.
                problem: b.enabled && e.graph.connected && e.graph.node(&b.node_name()).is_none(),
            })
            .collect();
        let key: Vec<String> = buses
            .iter()
            .map(|b| format!("{}|{}|{}|{}|{}", b.id, b.name, b.enabled, b.detail, b.problem))
            .collect();
        if m.buses_key != key {
            win.set_buses(ModelRc::new(VecModel::from(buses)));
            m.buses_key = key;
        }
        win.set_bus_selected(if bus.is_some() { v.selected_bus as i32 } else { -1 });
        win.set_has_bus(bus.is_some());

        // --- the selected bus ---
        if let Some(b) = bus {
            win.set_bus_name(b.name.as_str().into());
            win.set_bus_enabled(b.enabled);
            win.set_bus_node_name(b.node_name().into());
            win.set_bus_explain(b.kind.explain().into());
            win.set_bus_live(e.graph.node(&b.node_name()).is_some());
            win.set_bus_status(bus_status(&e, b).into());

            let kinds: Vec<String> = BusKind::ALL.iter().map(|k| k.label().to_string()).collect();
            if m.bus_kinds_key != kinds {
                win.set_bus_kind_options(strings(&kinds));
                m.bus_kinds_key = kinds;
            }
            win.set_bus_kind_index(
                BusKind::ALL.iter().position(|k| *k == b.kind).unwrap_or(0) as i32,
            );

            let widths: Vec<String> = (1..=b.width.max(8)).map(ui::width_label).collect();
            if m.bus_widths_key != widths {
                win.set_bus_width_options(strings(&widths));
                m.bus_widths_key = widths;
            }
            win.set_bus_width_index(b.width as i32 - 1);

            let uses: Vec<BusUseRow> = e
                .bus_uses(b)
                .into_iter()
                .map(|(chain, into)| BusUseRow {
                    chain: chain.as_str().into(),
                    direction: if into {
                        "sends into this bus".into()
                    } else {
                        "takes audio from this bus".into()
                    },
                })
                .collect();
            let key: Vec<String> = uses.iter().map(|u| format!("{}|{}", u.chain, u.direction)).collect();
            if m.bus_uses_key != key {
                win.set_bus_uses(ModelRc::new(VecModel::from(uses)));
                m.bus_uses_key = key;
            }

            // --- applications routed here ---
            let record = b.kind == BusKind::Source;
            let verb = if record { "recording" } else { "playing" };
            win.set_bus_apps_heading(
                if record {
                    "APPLICATIONS RECORDING FROM THIS BUS"
                } else {
                    "APPLICATIONS PLAYING INTO THIS BUS"
                }
                .into(),
            );
            let rows: Vec<BusAppRow> = b
                .apps
                .iter()
                .map(|app| {
                    let n = e.graph.streams_of(app, record);
                    BusAppRow {
                        name: app.as_str().into(),
                        detail: if !b.enabled {
                            "the bus is off — left where it is".into()
                        } else if n > 0 {
                            format!("{verb} · {n} stream{}", plural(n)).into()
                        } else {
                            "not running — sent here when it starts".into()
                        },
                        running: b.enabled && n > 0,
                    }
                })
                .collect();
            let key: Vec<String> = rows
                .iter()
                .map(|r| format!("{}|{}|{}", r.name, r.detail, r.running))
                .collect();
            if m.bus_apps_key != key {
                win.set_bus_apps(ModelRc::new(VecModel::from(rows)));
                m.bus_apps_key = key;
            }
            win.set_bus_apps_hint(bus_apps_hint(b).into());

            let (options, names) = bus_app_choices(&e, b);
            let index = v
                .bus_app_pick
                .as_ref()
                .and_then(|p| names.iter().position(|n| n == p))
                .unwrap_or(0);
            if m.bus_app_key != options {
                win.set_bus_app_options(strings(&options));
                m.bus_app_key = options;
            }
            win.set_bus_app_index(index as i32);
            v.bus_app_names = names;
        }

        // --- plugins available to add ---
        let plugins: Vec<String> = e
            .plugins
            .iter()
            .map(|p| format!("{} — {}", p.name, p.summary))
            .collect();
        if m.plugins_key != plugins {
            if win.get_plugin_index() as usize >= plugins.len() {
                win.set_plugin_index(0);
            }
            win.set_plugin_options(strings(&plugins));
            m.plugins_key = plugins;
        }

        // --- presets ---
        if m.presets_key != v.presets {
            if win.get_preset_index() as usize >= v.presets.len() {
                win.set_preset_index(if v.presets.is_empty() { -1 } else { 0 });
            }
            win.set_presets(strings(&v.presets));
            m.presets_key = v.presets.clone();
        }
        win.set_metering(e.state.meters);
        win.set_status(self.status_text(&e).into());
        win.set_can_undo(self.history.borrow().can_undo());
        win.set_can_redo(self.history.borrow().can_redo());
        win.set_status_bad(!e.graph.connected || {
            let (wanted, made) = self.links.get();
            made < wanted
        });

        // --- the log ---
        if m.log_key != e.log.len() {
            let log: Vec<String> = e
                .log
                .iter()
                .map(|l| format!("{}{}", if l.error { "! " } else { "  " }, l.text))
                .collect();
            win.set_log_errors(e.log.iter().filter(|l| l.error).count() as i32);
            win.set_log(strings(&log));
            m.log_key = e.log.len();
        }

        let Some(chain) = selected else {
            if !m.shape.is_empty() {
                m.clear(&win);
            }
            // The matrix belongs to a chain's endpoint, so it cannot be open
            // over the bus pane or over an empty one. Saying so here as well
            // as at the end matters because this path returns early.
            win.set_map_open(false);
            return;
        };

        // --- the chain's own controls ---
        win.set_chain_name(chain.name.as_str().into());
        win.set_chain_enabled(chain.enabled);
        win.set_chain_latency(ui::chain_latency_text(e.chain_latency_ms(chain)).into());
        let widths: Vec<String> = (1..=chain.width.max(8)).map(ui::width_label).collect();
        if m.widths_key != widths {
            win.set_width_options(strings(&widths));
            m.widths_key = widths;
        }
        win.set_width_index(chain.width as i32 - 1);

        // --- the two endpoints ---
        for dir in [Dir::Out, Dir::In] {
            let endpoint = match dir {
                Dir::Out => chain.input.as_ref(),
                Dir::In => chain.output.as_ref(),
            };
            let (options, nodes, index) = endpoint_model(&e, dir, endpoint);
            let pairs = e.endpoint_pairs(chain, dir);
            let detail = endpoint_detail(&e, endpoint, &pairs, chain.width);
            let manual = endpoint.is_some_and(|ep| ep.mapping != Mapping::Auto);

            match dir {
                Dir::Out => {
                    if m.source_key != options {
                        win.set_source_options(strings(&options));
                        m.source_key = options;
                    }
                    win.set_source_index(index);
                    win.set_source_detail(detail.into());
                    win.set_source_mapped(manual);
                    v.source_nodes = nodes;
                }
                Dir::In => {
                    if m.sink_key != options {
                        win.set_sink_options(strings(&options));
                        m.sink_key = options;
                    }
                    win.set_sink_index(index);
                    win.set_sink_detail(detail.into());
                    win.set_sink_mapped(manual);
                    v.sink_nodes = nodes;
                }
            }
        }

        // --- the sidechain sources, one list for every stage card ---
        let (sc_options, sc_nodes, sc_index) = sidechain_model(&e, chain);
        if m.sidechain_key != sc_options {
            win.set_sidechain_options(strings(&sc_options));
            m.sidechain_key = sc_options;
        }
        v.sidechain_nodes = sc_nodes;
        let sc = |i: usize| sc_index.get(i).copied().unwrap_or(0);

        // --- the stages ---
        //
        // Only a change of shape -- a stage added, removed, moved, or a
        // different chain selected -- builds new models. Everything else
        // writes rows into the models that are already there, because
        // replacing one makes Slint rebuild every element bound to it,
        // including the slider the user has hold of.
        let shape: Vec<String> = chain
            .stages
            .iter()
            .map(|s| format!("{}|{}", chain.node_name(s), s.plugin))
            .collect();

        let mut choices = std::mem::take(&mut m.choices);
        if m.shape != shape {
            m.params = chain
                .stages
                .iter()
                .map(|s| {
                    Rc::new(VecModel::from(match e.plugin(&s.plugin) {
                        Some(d) => param_rows(d, s, &mut choices),
                        None => Vec::new(),
                    }))
                })
                .collect();
            let rows: Vec<StageRow> = chain
                .stages
                .iter()
                .zip(&m.params)
                .enumerate()
                .map(|(i, (s, p))| stage_row(&e, chain, s, ModelRc::from(p.clone()), sc(i)))
                .collect();
            let stages = Rc::new(VecModel::from(rows));
            // Whether the gr strip is there is part of the shape: it is
            // decided once, here, and refresh_meters only moves the figures.
            let levels = Rc::new(VecModel::from(
                chain
                    .stages
                    .iter()
                    .map(|s| {
                        level_row(
                            -240.0,
                            -240.0,
                            has_gr(&e, s).then_some(0.0),
                            has_sc(&e, s).then_some(-240.0),
                        )
                    })
                    .collect::<Vec<_>>(),
            ));

            let curves = Rc::new(VecModel::from(
                chain.stages.iter().map(|s| curve_row(&e, chain, s)).collect::<Vec<_>>(),
            ));

            win.set_stages(ModelRc::from(stages.clone()));
            win.set_levels(ModelRc::from(levels.clone()));
            win.set_curves(ModelRc::from(curves.clone()));
            m.stages = Some(stages);
            m.levels = Some(levels);
            m.curves = Some(curves);
            m.shape = shape;
        } else if let Some(stages) = m.stages.clone() {
            // A curve moves only when the filter reports a new one, and is
            // written only then: the plot is redrawn on a real change and
            // left alone on every other refresh.
            if let Some(curves) = m.curves.as_ref() {
                for (i, s) in chain.stages.iter().enumerate() {
                    let row = curve_row(&e, chain, s);
                    if curves.row_data(i).as_ref() != Some(&row) {
                        curves.set_row_data(i, row);
                    }
                }
            }
            for (i, stage) in chain.stages.iter().enumerate() {
                if let Some(desc) = e.plugin(&stage.plugin) {
                    let pm = &m.params[i];
                    for (j, row) in param_rows(desc, stage, &mut choices).into_iter().enumerate() {
                        if pm.row_data(j).as_ref() != Some(&row) {
                            pm.set_row_data(j, row);
                        }
                    }
                }
                let row = stage_row(&e, chain, stage, ModelRc::from(m.params[i].clone()), sc(i));
                if stages.row_data(i).as_ref() != Some(&row) {
                    stages.set_row_data(i, row);
                }
            }
        }
        m.choices = choices;

        // --- the channel matrix, if it is open ---
        if let Some(dir) = v.map {
            let (title, endpoint) = match dir {
                Dir::Out => ("Input channels", chain.input.as_ref()),
                Dir::In => ("Output channels", chain.output.as_ref()),
            };
            let pairs = e.endpoint_pairs(chain, dir);
            let node = endpoint.and_then(|ep| e.graph.lookup(&ep.node));
            let ports: Vec<(String, String)> = node
                .map(|n| {
                    n.ports_for(dir)
                        .iter()
                        .map(|p| (p.name.clone(), p.channel.clone().unwrap_or_default()))
                        .collect()
                })
                .unwrap_or_default();

            let rows: Vec<MapRow> = ports
                .iter()
                .map(|(name, channel)| MapRow {
                    port: name.as_str().into(),
                    channel: channel.as_str().into(),
                    cells: ModelRc::new(VecModel::from(
                        (0..chain.width)
                            .map(|ch| pairs.iter().any(|(p, c)| p == name && *c == ch))
                            .collect::<Vec<bool>>(),
                    )),
                })
                .collect();

            v.map_ports = ports.into_iter().map(|(n, _)| n).collect();
            win.set_map_title(title.into());
            win.set_map_node(
                endpoint
                    .map(|ep| match e.graph.lookup(&ep.node) {
                        Some(n) if e.graph.default_for(&ep.node).is_some() => {
                            format!("{} ({})", ep.label, n.description)
                        }
                        Some(_) => ep.label.clone(),
                        None => format!("{} (not connected)", ep.label),
                    })
                    .unwrap_or_default()
                    .into(),
            );
            win.set_map_auto(endpoint.is_none_or(|ep| ep.mapping == Mapping::Auto));
            win.set_map_channels(strings(&graph::chain_channels(chain.width)));
            win.set_map_rows(ModelRc::new(VecModel::from(rows)));
        }
        win.set_map_open(v.map.is_some());

        drop(m);
        drop(v);
        drop(e);
        self.refresh_meters();
    }

    /// The meters move a dozen times a second. They are their own model so
    /// that a reading never touches a stage card, let alone rebuilds one.
    fn refresh_meters(&self) {
        let e = self.engine.borrow();
        let v = self.view.borrow();
        let m = self.models.borrow();
        let (Some(levels), Some(chain)) = (m.levels.as_ref(), e.state.chains.get(v.selected))
        else {
            return;
        };
        for (i, s) in chain.stages.iter().enumerate() {
            let rt = e.runtime(&chain.node_name(s));
            let (a, b) = rt.map(|r| (r.in_level, r.out_level)).unwrap_or((-240.0, -240.0));
            let row = level_row(
                a,
                b,
                has_gr(&e, s).then(|| rt.map_or(0.0, |r| r.gr)),
                has_sc(&e, s).then(|| rt.map_or(-240.0, |r| r.sc_level)),
            );
            if levels.row_data(i).as_ref() != Some(&row) {
                levels.set_row_data(i, row);
            }
        }
    }
}

/// Whether a line from a filter is only a meter moving, which redraws the
/// meters and nothing else. Anything else is a full refresh -- and a full
/// refresh many times a second keeps rebuilding every list in the window,
/// so a reading missing from here is a dropdown that cannot be used.
fn is_meter_reading(p: &ProcEvent) -> bool {
    matches!(p, ProcEvent::Param { name, .. }
             if matches!(name.as_str(), "in-level" | "out-level" | "gr" | "sc-level"))
}

/// The models the window is bound to, kept alive between refreshes.
///
/// Every list goes into the window only when its contents have changed --
/// hence the keys. Handing a dropdown a new list while its popup is open
/// rebuilds the popup under the pointer: it flickers, and the click that
/// was on its way to an entry lands on nothing.
#[derive(Default)]
struct Models {
    /// One entry per stage, identifying it. A change here, and only a change
    /// here, rebuilds the stage models.
    shape: Vec<String>,
    stages: Option<Rc<VecModel<StageRow>>>,
    params: Vec<Rc<VecModel<ParamRow>>>,
    levels: Option<Rc<VecModel<LevelRow>>>,
    curves: Option<Rc<VecModel<CurveRow>>>,
    chains_key: Vec<String>,
    buses_key: Vec<String>,
    bus_kinds_key: Vec<String>,
    bus_widths_key: Vec<String>,
    bus_uses_key: Vec<String>,
    bus_apps_key: Vec<String>,
    bus_app_key: Vec<String>,
    widths_key: Vec<String>,
    source_key: Vec<String>,
    sink_key: Vec<String>,
    sidechain_key: Vec<String>,
    plugins_key: Vec<String>,
    presets_key: Vec<String>,
    log_key: usize,
    /// One model per enum parameter, reused. A filter's choices never change,
    /// and handing out a fresh model each time would make every rebuilt row
    /// compare unequal to the one already there -- so the row would be
    /// rewritten on every refresh, and the combo box told its own value back
    /// while the user was looking at its open list.
    choices: HashMap<String, ModelRc<SharedString>>,
}

impl Models {
    fn clear(&mut self, win: &AppWindow) {
        win.set_stages(ModelRc::new(VecModel::from(Vec::<StageRow>::new())));
        win.set_levels(ModelRc::new(VecModel::from(Vec::<LevelRow>::new())));
        self.shape.clear();
        self.stages = None;
        self.params.clear();
        self.levels = None;
    }
}

/// `gr` is `None` for a filter with no gain-reduction meter, which then
/// shows no strip for one rather than one that always reads nothing; `sc`
/// likewise for a filter with no sidechain input.
fn level_row(in_db: f32, out_db: f32, gr: Option<f32>, sc: Option<f32>) -> LevelRow {
    LevelRow {
        has_sc: sc.is_some(),
        sc_db: sc.unwrap_or(-240.0),
        sc_text: ui::level_text(sc.unwrap_or(-240.0)).into(),
        in_db,
        in_text: ui::level_text(in_db).into(),
        out_db,
        out_text: ui::level_text(out_db).into(),
        has_gr: gr.is_some(),
        gr_db: gr.unwrap_or(0.0),
        gr_text: ui::gr_text(gr.unwrap_or(0.0)).into(),
    }
}

/// A stage's response curve, laid out for the plot -- or an empty row, which
/// draws nothing, for a filter that has none or has not reported one yet.
fn curve_row(e: &Engine, chain: &Chain, stage: &Stage) -> CurveRow {
    let Some(curve) = e.runtime(&chain.node_name(stage)).and_then(|r| r.curve.as_ref()) else {
        return CurveRow::default();
    };
    let d = ui::curve_draw(curve);
    CurveRow {
        kind: if d.level { "level" } else { "freq" }.into(),
        path: d.path.into(),
        grid: d.grid.into(),
        axis: d.axis.into(),
        top: d.top.into(),
        bottom: d.bottom.into(),
        zero: d.zero,
    }
}

/// Whether a stage's filter has a gain-reduction meter. Learned from its
/// `--help` like every other parameter, so a new dynamics filter gets one
/// with no change here.
fn has_gr(e: &Engine, stage: &Stage) -> bool {
    e.plugin(&stage.plugin).is_some_and(|d| d.param("gr").is_some())
}

/// Whether a stage's filter meters a sidechain input, learned the same way.
fn has_sc(e: &Engine, stage: &Stage) -> bool {
    e.plugin(&stage.plugin).is_some_and(|d| d.param("sc-level").is_some())
}

fn strings(v: &[String]) -> ModelRc<SharedString> {
    ModelRc::new(VecModel::from(
        v.iter().map(|s| SharedString::from(s.as_str())).collect::<Vec<_>>(),
    ))
}

/// One stage card. `sc_index` is its entry in the shared sidechain list.
fn stage_row(
    e: &Engine,
    chain: &Chain,
    stage: &Stage,
    params: ModelRc<ParamRow>,
    sc_index: i32,
) -> StageRow {
    let node = chain.node_name(stage);
    let rt = e.runtime(&node).cloned().unwrap_or_default();
    match e.plugin(&stage.plugin) {
        Some(desc) => StageRow {
            has_sidechain: desc.sidechain,
            sc_index,
            sc_detail: if desc.sidechain {
                match stage.sidechain.as_ref() {
                    None => "nothing linked".into(),
                    ep => endpoint_detail(e, ep, &e.sidechain_pairs(stage), 1).into(),
                }
            } else {
                SharedString::new()
            },
            node: node.as_str().into(),
            plugin: desc.name.as_str().into(),
            title: desc.name.as_str().into(),
            summary: desc.summary.as_str().into(),
            bypass: stage.bypass,
            running: rt.running,
            latency: latency_text(&rt).into(),
            error: rt.last_error.as_str().into(),
            params,
        },
        None => StageRow {
            node: node.as_str().into(),
            plugin: stage.plugin.as_str().into(),
            title: stage.plugin.as_str().into(),
            summary: "not installed".into(),
            bypass: stage.bypass,
            running: false,
            latency: "".into(),
            error: format!("No filter called {} was found.", stage.plugin).into(),
            params,
            ..Default::default()
        },
    }
}

fn param_rows(
    desc: &plugin::PluginDesc,
    stage: &Stage,
    choices: &mut HashMap<String, ModelRc<SharedString>>,
) -> Vec<ParamRow> {
    desc.live_params()
        .map(|p| {
            let raw = stage
                .params
                .get(&p.name)
                .cloned()
                .unwrap_or_else(|| p.default.clone());
            let number: f64 = raw.parse().unwrap_or_else(|_| p.default_f64());
            ParamRow {
                name: p.name.as_str().into(),
                label: p.name.as_str().into(),
                unit: p.unit.clone().unwrap_or_default().into(),
                help: p.help.as_str().into(),
                kind: match &p.kind {
                    ParamKind::Number { .. } => "number",
                    ParamKind::Bool => "bool",
                    ParamKind::Enum(_) => "enum",
                    ParamKind::Text => "text",
                }
                .into(),
                value: ui::display_value(&raw).into(),
                pos: ui::to_pos(p, number),
                on: raw == "on",
                choices: match &p.kind {
                    ParamKind::Enum(c) => choices
                        .entry(format!("{}/{}", desc.name, p.name))
                        .or_insert_with(|| strings(c))
                        .clone(),
                    _ => choices
                        .entry(String::new())
                        .or_insert_with(|| strings(&[]))
                        .clone(),
                },
                choice_index: match &p.kind {
                    ParamKind::Enum(c) => c.iter().position(|x| *x == raw).unwrap_or(0) as i32,
                    _ => 0,
                },
            }
        })
        .collect()
}

fn plural(n: usize) -> &'static str {
    if n == 1 {
        ""
    } else {
        "s"
    }
}

fn chain_detail(c: &Chain, latency_ms: Option<f64>) -> String {
    let width = match c.width {
        1 => "mono".to_string(),
        2 => "stereo".to_string(),
        n => format!("{n} ch"),
    };
    let mut s = format!(
        "{width} · {} filter{}",
        c.stages.len(),
        plural(c.stages.len())
    );
    if let Some(l) = ui::chain_latency_short(latency_ms) {
        s += &format!(" · {l}");
    }
    s
}

fn bus_detail(b: &Bus) -> String {
    let width = match b.width {
        1 => "mono".to_string(),
        2 => "stereo".to_string(),
        n => format!("{n} ch"),
    };
    let kind = match b.kind {
        BusKind::Source => "source",
        BusKind::Sink => "sink",
    };
    format!("{width} · {kind}")
}

/// Whether the bus is actually there, in the terms that tell the user what to
/// do about it if it is not.
/// What the bus pane says under its list of applications.
fn bus_apps_hint(b: &Bus) -> &'static str {
    match (b.kind == BusKind::Source, b.apps.is_empty()) {
        (false, true) => {
            "Send an application's sound here instead of to its own output: pick it \
             below while it is playing. It goes back when it is removed here or when \
             pwfx stops."
        }
        (true, true) => {
            "Have an application record from this bus instead of its own microphone: \
             pick it below while it is recording. It goes back when it is removed here \
             or when pwfx stops."
        }
        _ => {
            "Sent here each time it starts. Moving it somewhere else yourself is left \
             alone until it next starts."
        }
    }
}

/// The "add an application" list for a bus: the ones going its way right
/// now that it does not already have, and the names behind the entries.
/// One that another bus of the same kind has is offered with a note,
/// because adding it here moves it.
fn bus_app_choices(e: &Engine, b: &Bus) -> (Vec<String>, Vec<String>) {
    let record = b.kind == BusKind::Source;
    let verb = if record { "recording" } else { "playing" };
    let running: Vec<String> = e
        .graph
        .apps(record)
        .into_iter()
        .filter(|a| !b.apps.contains(a))
        .collect();
    let mut options = vec![if running.is_empty() {
        format!("— no application is {verb} right now —")
    } else {
        format!("— add an application that is {verb} —")
    }];
    let mut names = vec![String::new()];
    for app in running {
        let elsewhere = e
            .state
            .buses
            .iter()
            .find(|o| o.id != b.id && o.kind == b.kind && o.apps.contains(&app));
        options.push(match elsewhere {
            Some(o) => format!("{app}  ·  now on {}", o.description()),
            None => app.clone(),
        });
        names.push(app);
    }
    (options, names)
}

fn bus_status(e: &Engine, b: &Bus) -> String {
    if !b.enabled {
        return "off — no other application can see it".into();
    }
    if !e.graph.connected {
        return "waiting for PipeWire".into();
    }
    match e.graph.node(&b.node_name()) {
        Some(n) => format!(
            "live · {} input{}, {} output{}",
            n.inputs().len(),
            plural(n.inputs().len()),
            n.outputs().len(),
            plural(n.outputs().len())
        ),
        None => "asking the daemon for it…".into(),
    }
}

fn latency_text(rt: &engine::Runtime) -> String {
    if rt.latency == 0 {
        return String::new();
    }
    let rate: f64 = rt
        .reported
        .get("rate")
        .and_then(|r| r.parse().ok())
        .unwrap_or(48000.0);
    format!("+{:.1} ms", rt.latency as f64 * 1000.0 / rate)
}

/// The picker's strings, the node names behind them, and which one is
/// currently chosen.
///
/// A saved endpoint whose device is not plugged in right now keeps its place
/// in the list rather than being dropped -- otherwise opening the app with the
/// interface unplugged would quietly forget where the chain was connected.
fn endpoint_model(
    e: &Engine,
    dir: Dir,
    endpoint: Option<&Endpoint>,
) -> (Vec<String>, Vec<String>, i32) {
    let mut options: Vec<String> = vec![match dir {
        Dir::Out => "— no input —".into(),
        Dir::In => "— no output —".into(),
    }];
    let mut nodes: Vec<String> = vec![String::new()];

    // Then the default, which is not a node but whichever one the session
    // manager currently names. It says which, so picking it is not a guess.
    let default = default_endpoint(dir);
    let now = match e.graph.default_for(default) {
        Some(Some(_)) => match e.graph.lookup(default) {
            Some(n) => n.description.clone(),
            None => "not followed".into(),
        },
        _ => "none".into(),
    };
    options.push(format!("{}  ·  now {now}", default_label(dir)));
    nodes.push(default.into());

    for n in e.graph.endpoints(dir, |n| state::is_stage_node(&n.name)) {
        let count = n.ports_for(dir).len();
        options.push(format!(
            "{}  ·  {count} ch  ·  {}",
            n.description,
            n.kind(dir).label()
        ));
        nodes.push(n.name.clone());
    }

    let index = match endpoint {
        None => 0,
        Some(ep) => match nodes.iter().position(|n| *n == ep.node) {
            Some(i) => i as i32,
            None => {
                options.push(format!("{}  ·  not connected", ep.label));
                nodes.push(ep.node.clone());
                (nodes.len() - 1) as i32
            }
        },
    };
    (options, nodes, index)
}

/// The sidechain pickers' strings and node names, and each stage's entry.
///
/// One list for every card in the chain: what a chain's input could take,
/// with "none" first, and after it any saved sidechain whose source is not
/// in the graph right now -- kept in its place, as `endpoint_model` keeps an
/// unplugged input, rather than forgotten. A stage without a sidechain
/// input reads 0 and shows no picker.
fn sidechain_model(e: &Engine, chain: &Chain) -> (Vec<String>, Vec<String>, Vec<i32>) {
    let (mut options, mut nodes, _) = endpoint_model(e, Dir::Out, None);
    options[0] = "— none —".into();
    let index = chain
        .stages
        .iter()
        .map(|s| match s.sidechain.as_ref() {
            None => 0,
            Some(ep) => match nodes.iter().position(|n| *n == ep.node) {
                Some(i) => i as i32,
                None => {
                    options.push(format!("{}  ·  not connected", ep.label));
                    nodes.push(ep.node.clone());
                    (nodes.len() - 1) as i32
                }
            },
        })
        .collect();
    (options, nodes, index)
}

/// The default endpoint for one end of a chain: the default microphone to
/// take from, the default output to send to.
fn default_endpoint(dir: Dir) -> &'static str {
    match dir {
        Dir::Out => graph::DEFAULT_SOURCE,
        Dir::In => graph::DEFAULT_SINK,
    }
}

fn default_label(dir: Dir) -> &'static str {
    match dir {
        Dir::Out => "Default input",
        Dir::In => "Default output",
    }
}

fn endpoint_detail(
    e: &Engine,
    endpoint: Option<&Endpoint>,
    pairs: &[(String, usize)],
    width: usize,
) -> String {
    let Some(ep) = endpoint else {
        return String::new();
    };
    if let Some(target) = e.graph.default_for(&ep.node) {
        let Some(target) = target else {
            return "there is no default right now".into();
        };
        let Some(node) = e.graph.lookup(&ep.node) else {
            return if target.starts_with(state::PREFIX) {
                // Following it would feed this app from its own output.
                "the default is one of this app's own nodes; not followed".into()
            } else {
                "waiting for the default device".into()
            };
        };
        return format!(
            "following {} · {}",
            node.description,
            plural_links(pairs.len())
        );
    }
    if e.graph.node(&ep.node).is_none() {
        // A bus is something the user can turn back on, which is worth
        // saying rather than leaving them waiting for a device that is
        // never coming.
        return match e.state.bus(&ep.node) {
            Some(b) if !b.enabled => "the bus is switched off".into(),
            Some(_) => "waiting for the bus".into(),
            None => "waiting for the device".into(),
        };
    }
    let covered = (0..width)
        .filter(|ch| pairs.iter().any(|(_, c)| c == ch))
        .count();
    if covered == 0 {
        "no channels mapped".into()
    } else if covered < width {
        format!("{covered} of {width} channels mapped")
    } else {
        plural_links(pairs.len())
    }
}

fn plural_links(n: usize) -> String {
    format!("{n} link{}", plural(n))
}

// ---------------------------------------------------------------------------
// clicks

impl App {
    /// Change only what is being looked at -- no state edit, no save.
    fn look(&self, f: impl FnOnce(&mut View)) {
        f(&mut self.view.borrow_mut());
        self.refresh();
    }

    fn wire(&self, win: &AppWindow) {
        macro_rules! on {
            ($setter:ident, |$app:ident $(, $arg:ident)*| $body:block) => {
                let $app = self.clone();
                win.$setter(move |$($arg),*| $body);
            };
        }

        // --- chains -----------------------------------------------------
        on!(on_select_chain, |app, i| {
            app.look(|v| {
                v.selected = i.max(0) as usize;
                v.focus = Focus::Chain;
            });
        });

        on!(on_add_chain, |app| {
            app.edit(|e, v| {
                let name = format!("Chain {}", e.state.chains.len() + 1);
                let id = e.state.fresh_chain_id(&name);
                e.state.chains.push(Chain {
                    id,
                    name,
                    enabled: true,
                    width: 2,
                    stages: Vec::new(),
                    input: None,
                    output: None,
                });
                v.selected = e.state.chains.len() - 1;
                v.focus = Focus::Chain;
                v.map = None;
            });
        });

        on!(on_remove_chain, |app| {
            app.edit(|e, v| {
                if v.selected < e.state.chains.len() {
                    e.state.chains.remove(v.selected);
                }
                v.selected = v.selected.saturating_sub(1);
                v.map = None;
            });
        });

        on!(on_rename_chain, |app, name| {
            app.edit(move |e, v| {
                // The id, and with it every node name in the graph, stays as
                // it was: renaming a chain must not restart its filters.
                if let Some(c) = e.state.chains.get_mut(v.selected) {
                    c.name = name.to_string();
                }
            });
        });

        on!(on_set_chain_enabled, |app, on| {
            app.edit(move |e, v| {
                if let Some(c) = e.state.chains.get_mut(v.selected) {
                    c.enabled = on;
                }
            });
        });

        on!(on_set_width, |app, index| {
            app.edit(move |e, v| {
                if let Some(c) = e.state.chains.get_mut(v.selected) {
                    // Channel count is a command-line parameter in the
                    // filters, so this one restarts the chain's processes.
                    c.width = (index.max(0) as usize + 1).clamp(1, 64);
                }
            });
        });

        // --- buses ------------------------------------------------------
        on!(on_select_bus, |app, i| {
            app.look(|v| {
                v.selected_bus = i.max(0) as usize;
                v.focus = Focus::Bus;
                v.map = None;
                v.bus_app_pick = None;
            });
        });

        on!(on_pick_bus_app, |app, i| {
            app.look(move |v| {
                v.bus_app_pick = v.bus_app_names.get(i.max(0) as usize).filter(|n| !n.is_empty()).cloned();
            });
        });
        on!(on_add_bus_app, |app| {
            app.edit(|e, v| {
                let Some(name) = v.bus_app_pick.take() else { return };
                let Some(kind) = e.state.buses.get(v.selected_bus).map(|b| b.kind) else {
                    return;
                };
                // One bus per application and direction, so the state can
                // never send one stream two ways: adding it here takes it
                // off any other bus of the same kind.
                let mut moved_from = Vec::new();
                for (i, b) in e.state.buses.iter_mut().enumerate() {
                    if i != v.selected_bus && b.kind == kind && b.apps.contains(&name) {
                        b.apps.retain(|a| *a != name);
                        moved_from.push(b.description());
                    }
                }
                let b = &mut e.state.buses[v.selected_bus];
                if !b.apps.contains(&name) {
                    b.apps.push(name.clone());
                }
                let here = b.description();
                for from in moved_from {
                    e.note(format!("{name} moved from {from} to {here}"), false);
                }
            });
        });
        on!(on_remove_bus_app, |app, i| {
            app.edit(move |e, v| {
                if let Some(b) = e.state.buses.get_mut(v.selected_bus) {
                    if (i as usize) < b.apps.len() {
                        b.apps.remove(i as usize);
                    }
                }
            });
        });

        on!(on_add_bus, |app| {
            app.edit(|e, v| {
                let name = format!("Bus {}", e.state.buses.len() + 1);
                let id = e.state.fresh_bus_id(&name);
                e.state.buses.push(Bus {
                    id,
                    name,
                    enabled: true,
                    width: 2,
                    kind: BusKind::Source,
                    apps: Vec::new(),
                });
                v.selected_bus = e.state.buses.len() - 1;
                v.focus = Focus::Bus;
                v.map = None;
            });
        });

        on!(on_remove_bus, |app| {
            app.edit(|e, v| {
                if v.selected_bus >= e.state.buses.len() {
                    return;
                }
                let gone = e.state.buses.remove(v.selected_bus);
                let node = gone.node_name();
                // Unplug the chains that were using it rather than leaving
                // them pointing at a name that is no longer anything. Ids are
                // handed out again once they are free, so a later bus with
                // the same name would otherwise inherit these chains without
                // anyone having asked it to.
                let mut unplugged = 0;
                for c in &mut e.state.chains {
                    for end in [&mut c.input, &mut c.output] {
                        if end.as_ref().is_some_and(|ep| ep.node == node) {
                            *end = None;
                            unplugged += 1;
                        }
                    }
                }
                if unplugged > 0 {
                    e.note(
                        format!(
                            "removed the bus “{}”; {unplugged} chain end{} now unplugged",
                            gone.name,
                            plural(unplugged)
                        ),
                        false,
                    );
                }
                v.selected_bus = v.selected_bus.saturating_sub(1);
            });
        });

        on!(on_rename_bus, |app, name| {
            app.edit(move |e, v| {
                // Unlike a chain, this one is visible outside the app: it is
                // the description other applications show in their device
                // list, so the node is made again under the new one. The
                // `node.name` behind it does not move, so the chains wired to
                // the bus are undisturbed.
                if let Some(b) = e.state.buses.get_mut(v.selected_bus) {
                    b.name = name.to_string();
                }
            });
        });

        on!(on_set_bus_enabled, |app, on| {
            app.edit(move |e, v| {
                if let Some(b) = e.state.buses.get_mut(v.selected_bus) {
                    b.enabled = on;
                }
            });
        });

        on!(on_set_bus_kind, |app, index| {
            app.edit(move |e, v| {
                let Some(kind) = BusKind::ALL.get(index.max(0) as usize).copied() else {
                    return;
                };
                if let Some(b) = e.state.buses.get_mut(v.selected_bus) {
                    b.kind = kind;
                }
            });
        });

        on!(on_set_bus_width, |app, index| {
            app.edit(move |e, v| {
                if let Some(b) = e.state.buses.get_mut(v.selected_bus) {
                    // Channel count is fixed when a null sink is created, so
                    // this is the edit that makes the node again -- and any
                    // application streaming through it is moved off it while
                    // that happens.
                    b.width = (index.max(0) as usize + 1).clamp(1, 64);
                }
            });
        });

        // --- stages -----------------------------------------------------
        let weak = win.as_weak();
        on!(on_add_stage, |app| {
            let Some(w) = weak.upgrade() else { return };
            let pick = w.get_plugin_index().max(0) as usize;
            app.edit(move |e, v| {
                let Some(desc) = e.plugins.get(pick).cloned() else {
                    return;
                };
                let Some(c) = e.state.chains.get_mut(v.selected) else {
                    return;
                };
                let id = c.fresh_stage_id(&desc.name);
                c.stages.push(Stage {
                    id,
                    plugin: desc.name.clone(),
                    bypass: false,
                    sidechain: None,
                    params: desc.defaults(),
                });
            });
        });

        on!(on_remove_stage, |app, i| {
            app.edit(move |e, v| {
                if let Some(c) = e.state.chains.get_mut(v.selected) {
                    let i = i.max(0) as usize;
                    if i < c.stages.len() {
                        c.stages.remove(i);
                    }
                }
            });
        });

        on!(on_move_stage, |app, i, d| {
            app.edit(move |e, v| {
                let Some(c) = e.state.chains.get_mut(v.selected) else {
                    return;
                };
                let (i, j) = (i.max(0) as usize, i + d);
                if j >= 0 && (j as usize) < c.stages.len() && i < c.stages.len() {
                    c.stages.swap(i, j as usize);
                }
            });
        });

        on!(on_set_bypass, |app, i, on| {
            app.edit(move |e, v| {
                if let Some(s) = e
                    .state
                    .chains
                    .get_mut(v.selected)
                    .and_then(|c| c.stages.get_mut(i.max(0) as usize))
                {
                    s.bypass = on;
                }
            });
        });

        // --- parameters -------------------------------------------------
        on!(on_param_slide, |app, i, name, pos| {
            app.set_param(i, &name, |p| Some(ui::fmt_g(ui::from_pos(p, pos))));
        });

        on!(on_param_commit, |app, i, name, text| {
            app.set_param(i, &name, move |p| match &p.kind {
                ParamKind::Number { min, max, integer } => {
                    // A number the user typed is clamped here so the slider
                    // agrees with it immediately; the filter would clamp it
                    // too, but only after a round trip.
                    let v: f64 = text.trim().parse().ok()?;
                    let v = if *integer { v.round() } else { v };
                    Some(ui::fmt_g(v.clamp(*min, *max)))
                }
                _ => Some(text.to_string()),
            });
        });

        on!(on_param_pick, |app, i, name, choice| {
            app.set_param(i, &name, move |p| match &p.kind {
                ParamKind::Enum(c) => c.get(choice.max(0) as usize).cloned(),
                _ => None,
            });
        });

        on!(on_param_toggle, |app, i, name, on| {
            app.set_param(i, &name, move |_| {
                Some(if on { "on".into() } else { "off".into() })
            });
        });

        // --- endpoints --------------------------------------------------
        on!(on_set_source, |app, i| {
            app.set_endpoint(Dir::Out, i);
        });
        on!(on_set_sink, |app, i| {
            app.set_endpoint(Dir::In, i);
        });
        on!(on_set_sidechain, |app, stage, i| {
            app.set_sidechain(stage, i);
        });

        on!(on_open_map, |app, is_input| {
            app.look(move |v| v.map = Some(if is_input { Dir::Out } else { Dir::In }));
        });
        on!(on_close_map, |app| {
            app.look(|v| v.map = None);
        });

        on!(on_map_set_auto, |app, auto| {
            app.edit(move |e, v| {
                let Some(dir) = v.map else { return };
                // Switching to manual starts from whatever automatic had
                // worked out, so the dialog opens on the wiring that is
                // actually in place rather than on an empty grid.
                let pairs = e
                    .state
                    .chains
                    .get(v.selected)
                    .map(|c| e.endpoint_pairs(c, dir))
                    .unwrap_or_default();
                if let Some(ep) = endpoint_mut(e, v.selected, dir) {
                    ep.mapping = if auto {
                        Mapping::Auto
                    } else {
                        Mapping::Manual(pairs)
                    };
                }
            });
        });

        on!(on_map_toggle, |app, row, col| {
            app.edit(move |e, v| {
                let Some(dir) = v.map else { return };
                let Some(port) = v.map_ports.get(row.max(0) as usize).cloned() else {
                    return;
                };
                let col = col.max(0) as usize;
                let pairs = e
                    .state
                    .chains
                    .get(v.selected)
                    .map(|c| e.endpoint_pairs(c, dir))
                    .unwrap_or_default();

                let Some(ep) = endpoint_mut(e, v.selected, dir) else {
                    return;
                };
                let mut list = match &ep.mapping {
                    Mapping::Manual(l) => l.clone(),
                    Mapping::Auto => pairs,
                };
                let key = (port, col);
                match list.iter().position(|x| *x == key) {
                    Some(at) => {
                        list.remove(at);
                    }
                    None => list.push(key),
                }
                ep.mapping = Mapping::Manual(list);
            });
        });

        // --- app-wide ---------------------------------------------------
        on!(on_set_metering, |app, on| {
            app.edit(move |e, _| e.state.meters = on);
        });

        on!(on_undo, |app| {
            app.step(false);
        });
        on!(on_redo, |app| {
            app.step(true);
        });

        on!(on_rescan, |app| {
            app.edit(|e, _| {
                let dirs = plugin::search_path();
                let (plugins, errors) = plugin::discover(&dirs);
                let n = plugins.len();
                e.plugins = plugins;
                for err in errors {
                    e.note(err, true);
                }
                e.note(format!("rescanned: {n} filter{} found", plural(n)), false);
            });
        });

        on!(on_save_default, |app| {
            let st = app.engine.borrow().state.clone();
            match state::write(&state::default_path(), &st) {
                Ok(()) => app
                    .engine
                    .borrow_mut()
                    .note("saved as the default state", false),
                Err(e) => app.engine.borrow_mut().note(e, true),
            }
            app.refresh();
        });

        on!(on_restore_default, |app| {
            app.load_from(&state::default_path());
        });

        on!(on_save_preset, |app, name| {
            let name = name.trim().to_string();
            if name.is_empty() {
                return;
            }
            let st = app.engine.borrow().state.clone();
            match state::write(&state::preset_path(&name), &st) {
                Ok(()) => app
                    .engine
                    .borrow_mut()
                    .note(format!("saved preset “{name}”"), false),
                Err(e) => app.engine.borrow_mut().note(e, true),
            }
            app.look(|v| v.presets = state::list_presets());
        });

        on!(on_load_preset, |app, i| {
            let name = app.view.borrow().presets.get(i.max(0) as usize).cloned();
            if let Some(name) = name {
                app.load_from(&state::preset_path(&name));
            }
        });

        on!(on_delete_preset, |app, i| {
            let name = app.view.borrow().presets.get(i.max(0) as usize).cloned();
            if let Some(name) = name {
                if let Err(e) = std::fs::remove_file(state::preset_path(&name)) {
                    app.engine.borrow_mut().note(format!("{name}: {e}"), true);
                }
            }
            app.look(|v| v.presets = state::list_presets());
        });
    }

    /// Set one parameter of one stage of the selected chain.
    ///
    /// The value is written into the state and the engine sends the
    /// difference; what the GUI shows afterwards is whatever the filter
    /// echoes back, which is how a rejected or clamped value corrects itself
    /// on screen without this path knowing anything about it.
    fn set_param(
        &self,
        stage: i32,
        name: &str,
        value: impl FnOnce(&plugin::ParamDesc) -> Option<String>,
    ) {
        let (stage, name) = (stage.max(0) as usize, name.to_string());
        self.edit(move |e, v| {
            let Some(chain) = e.state.chains.get(v.selected) else {
                return;
            };
            let Some(s) = chain.stages.get(stage) else {
                return;
            };
            let Some(new) = e
                .plugin(&s.plugin)
                .and_then(|d| d.param(&name))
                .and_then(value)
            else {
                return;
            };
            e.state.chains[v.selected].stages[stage]
                .params
                .insert(name, new);
        });
    }

    fn set_endpoint(&self, dir: Dir, index: i32) {
        let index = index.max(0) as usize;
        self.edit(move |e, v| {
            let nodes = match dir {
                Dir::Out => &v.source_nodes,
                Dir::In => &v.sink_nodes,
            };
            let Some(node) = nodes.get(index).cloned() else {
                return;
            };
            // A default's label is what it is, not what it points at today.
            let label = if node == default_endpoint(dir) {
                default_label(dir).to_string()
            } else {
                e.graph
                    .node(&node)
                    .map(|n| n.description.clone())
                    .unwrap_or_else(|| node.clone())
            };
            let Some(c) = e.state.chains.get_mut(v.selected) else {
                return;
            };
            let slot = match dir {
                Dir::Out => &mut c.input,
                Dir::In => &mut c.output,
            };
            if node.is_empty() {
                *slot = None;
                return;
            }
            // Picking the same node again must not throw away a channel map
            // the user set up on it.
            let mapping = match slot.as_ref() {
                Some(ep) if ep.node == node => ep.mapping.clone(),
                _ => Mapping::Auto,
            };
            *slot = Some(Endpoint {
                node,
                label,
                mapping,
            });
        });
    }

    /// Point a stage's sidechain at entry `index` of the shared list, or at
    /// nothing for entry 0. Labelled like a chain's input, so a source that
    /// is unplugged later can still be named.
    fn set_sidechain(&self, stage: i32, index: i32) {
        let (stage, index) = (stage.max(0) as usize, index.max(0) as usize);
        self.edit(move |e, v| {
            let Some(node) = v.sidechain_nodes.get(index).cloned() else {
                return;
            };
            let label = if node == default_endpoint(Dir::Out) {
                default_label(Dir::Out).to_string()
            } else {
                e.graph
                    .node(&node)
                    .map(|n| n.description.clone())
                    .unwrap_or_else(|| node.clone())
            };
            let Some(s) = e
                .state
                .chains
                .get_mut(v.selected)
                .and_then(|c| c.stages.get_mut(stage))
            else {
                return;
            };
            s.sidechain = match s.sidechain.take() {
                _ if node.is_empty() => None,
                // the same source again keeps what it was
                Some(ep) if ep.node == node => Some(ep),
                _ => Some(Endpoint {
                    node,
                    label,
                    mapping: Mapping::Auto,
                }),
            };
        });
    }

    fn load_from(&self, path: &std::path::Path) {
        match state::read(path) {
            Ok(loaded) => {
                let label = if path == state::default_path() {
                    "restore the default".to_string()
                } else {
                    format!(
                        "load {}",
                        path.file_stem().map(|s| s.to_string_lossy()).unwrap_or_default()
                    )
                };
                self.edit_as(Some(label), move |e, v| {
                    e.state = loaded;
                    v.selected = 0;
                    v.selected_bus = 0;
                    v.focus = Focus::Chain;
                    v.map = None;
                });
                self.engine
                    .borrow_mut()
                    .note(format!("loaded {}", path.display()), false);
                self.refresh();
            }
            Err(e) => {
                self.engine.borrow_mut().note(e, true);
                self.refresh();
            }
        }
    }
}

fn endpoint_mut(e: &mut Engine, chain: usize, dir: Dir) -> Option<&mut Endpoint> {
    let c = e.state.chains.get_mut(chain)?;
    match dir {
        Dir::Out => c.input.as_mut(),
        Dir::In => c.output.as_mut(),
    }
}

#[cfg(test)]
mod arg_tests {
    use super::*;

    fn parse(args: &[&str]) -> Result<Mode, String> {
        parse_args(&args.iter().map(|s| s.to_string()).collect::<Vec<_>>())
    }

    #[test]
    fn the_refusal_says_what_to_do_about_what_is_running() {
        let (h, b) = refusal(Some(42), |_| Some(true));
        assert!(h.contains("without a window"), "{h}");
        assert!(b.contains("systemctl --user stop pwfx") && b.contains("kill 42"), "{b}");

        let (h, b) = refusal(Some(42), |_| Some(false));
        assert!(h.contains("already open"), "{h}");
        assert!(b.contains("42") && !b.contains("systemctl"), "{b}");

        // Gone between the lock and the look, or no pid to look at.
        let (_, b) = refusal(Some(42), |_| None);
        assert!(b.contains("42"), "{b}");
        let (h, _) = refusal(None, |_| unreachable!());
        assert_eq!(h, "pwfx is already running");
    }

    #[test]
    fn the_command_line() {
        assert!(matches!(parse(&[]), Ok(Mode::Window)));
        assert!(matches!(parse(&["--help"]), Ok(Mode::Help)));
        assert!(matches!(parse(&["--headless"]), Ok(Mode::Headless(None))));
        assert!(matches!(parse(&["--headless", "mic"]),
                         Ok(Mode::Headless(Some(f))) if f == PathBuf::from("mic")));
        assert!(matches!(parse(&["--install-desktop"]), Ok(Mode::InstallDesktop)));
        assert!(parse(&["--install-desktop", "x"]).is_err());
        assert!(matches!(parse(&["--install-service"]), Ok(Mode::InstallService)));
        assert!(parse(&["--headless", "--verbose"]).is_err());
        assert!(parse(&["mic.json"]).is_err());
    }
}

/// What the two endpoint pickers offer, which is the one place a bus has to
/// behave unlike the app's other nodes.
#[cfg(test)]
mod picker_tests {
    use super::*;
    use graph::{Graph, Node, Port};

    fn node(name: &str, class: &str, dir: Dir) -> Node {
        Node {
            id: 1,
            name: name.into(),
            description: name.into(),
            media_class: class.into(),
            app: None,
            ports: vec![Port {
                id: 2,
                name: "p".into(),
                channel: Some("MONO".into()),
                dir,
                index: 0,
            }],
        }
    }

    fn engine_with(nodes: Vec<Node>) -> Engine {
        let mut e = Engine::new(Vec::new(), state::AppState::default(), Arc::new(|_| {}));
        e.graph = Graph {
            nodes,
            connected: true,
            ..Default::default()
        };
        e
    }

    #[test]
    fn a_bus_is_offered_and_a_filter_stage_is_not() {
        let e = engine_with(vec![
            node("pwfx.voice.hpf", "", Dir::Out),
            node("pwfx.bus.mic", "Audio/Source/Virtual", Dir::Out),
            node("alsa_input.card", "Audio/Source", Dir::Out),
        ]);
        let (options, nodes, index) = endpoint_model(&e, Dir::Out, None);

        assert_eq!(index, 0, "nothing chosen is the first entry");
        assert_eq!(nodes[0], "", "and it stands for no endpoint at all");
        assert!(!nodes.iter().any(|n| n == "pwfx.voice.hpf"));
        // The default comes straight after "none"; then buses, so the
        // thing the user just made is at the top of the list rather than
        // filed under whatever media.class it happens to carry.
        assert_eq!(nodes[1], graph::DEFAULT_SOURCE);
        assert_eq!(nodes[2], "pwfx.bus.mic");
        assert!(options[2].contains("Buses"));
        assert_eq!(nodes[3], "alsa_input.card");
    }

    #[test]
    fn the_default_entry_says_what_it_points_at() {
        let mut card = node("alsa_input.card", "Audio/Source", Dir::Out);
        card.description = "Scarlett 2i2".into();
        let mut e = engine_with(vec![card]);
        e.graph.default_source = Some("alsa_input.card".into());
        let (options, nodes, _) = endpoint_model(&e, Dir::Out, None);
        assert_eq!(nodes[1], graph::DEFAULT_SOURCE);
        assert!(options[1].contains("Scarlett 2i2"), "{}", options[1]);

        // Chosen, it is found in the list as itself: never "not connected",
        // whatever the default happens to be.
        let saved = Endpoint {
            node: graph::DEFAULT_SOURCE.into(),
            label: "Default input".into(),
            mapping: Mapping::Auto,
        };
        e.graph.default_source = None;
        let (options, _, index) = endpoint_model(&e, Dir::Out, Some(&saved));
        assert_eq!(index, 1);
        assert!(options[1].contains("none"), "{}", options[1]);
    }

    #[test]
    fn a_bus_that_is_not_in_the_graph_keeps_its_place_in_the_picker() {
        // Switched off, or the daemon has not made it yet: a chain pointed at
        // it must not quietly lose where it was plugged in.
        let e = engine_with(vec![node("alsa_input.card", "Audio/Source", Dir::Out)]);
        let saved = Endpoint {
            node: "pwfx.bus.mic".into(),
            label: "Podcast Mic".into(),
            mapping: Mapping::Auto,
        };
        let (options, nodes, index) = endpoint_model(&e, Dir::Out, Some(&saved));
        assert_eq!(nodes[index as usize], "pwfx.bus.mic");
        assert!(options[index as usize].contains("Podcast Mic"));
    }
}

/// Driving the real window, headlessly.
///
/// These pin the behaviour of the std widgets, which own their own value: the
/// moment one is typed into or dragged, the binding feeding it from the model
/// is gone. Getting that wrong does not fail to compile and does not look
/// wrong on screen -- the field shows what was typed either way. It only
/// shows up later, as a setting that was never saved.
#[cfg(test)]
mod window_tests {
    use super::*;
    use i_slint_backend_testing as testing;
    use slint::platform::PointerEventButton;

    fn number_param(name: &str, value: &str, pos: f32) -> ParamRow {
        ParamRow {
            name: name.into(),
            label: name.into(),
            unit: "Hz".into(),
            help: "".into(),
            kind: "number".into(),
            value: value.into(),
            pos,
            on: false,
            choices: ModelRc::new(VecModel::from(Vec::<SharedString>::new())),
            choice_index: 0,
        }
    }

    fn one_stage_window(params: Vec<ParamRow>) -> AppWindow {
        let win = AppWindow::new().unwrap();
        win.set_has_chain(true);
        win.set_chain_name("Voice".into());
        win.set_chain_enabled(true);
        win.set_width_options(ModelRc::new(VecModel::from(vec![SharedString::from("Stereo")])));
        win.set_chains(ModelRc::new(VecModel::from(vec![ChainRow {
            id: "voice".into(),
            name: "Voice".into(),
            enabled: true,
            detail: "stereo".into(),
            problem: false,
        }])));
        win.set_selected(0);
        win.set_stages(ModelRc::new(VecModel::from(vec![StageRow {
            node: "pwfx.voice.eq".into(),
            plugin: "pw_eq".into(),
            title: "pw_eq".into(),
            summary: "5-band parametric EQ".into(),
            bypass: false,
            running: true,
            latency: "".into(),
            error: "".into(),
            params: ModelRc::new(VecModel::from(params)),
            ..Default::default()
        }])));
        win.set_levels(ModelRc::new(VecModel::from(vec![level_row(-240.0, -240.0, None, None)])));
        win
    }

    /// Bring the window up far enough that clicks land on elements. Nothing
    /// has a position until it has been shown and laid out, and a click into
    /// a window that is not active moves no focus.
    fn show(win: &AppWindow) {
        win.show().unwrap();
        win.window()
            .dispatch_event(slint::platform::WindowEvent::WindowActiveChanged(true));
    }

    fn field_of(win: &AppWindow, id: &str) -> testing::ElementHandle {
        testing::ElementHandle::find_by_element_id(win, id)
            .next()
            .unwrap_or_else(|| panic!("no element {id}"))
    }

    /// Give the loop a turn.
    ///
    /// Slint runs a `changed` handler as part of the property cycle that a
    /// rendered frame drives, so a model write needs a frame before it shows
    /// up in a widget. A click is the only thing the testing API offers that
    /// waits for one; this one lands on a field that is already focused, or
    /// on nothing in particular, so it changes no state of its own.
    async fn settle(win: &AppWindow) {
        field_of(win, "NameField::field")
            .single_click(PointerEventButton::Left)
            .await;
    }

    type Commits = Rc<RefCell<Vec<(String, String)>>>;

    fn record_commits(win: &AppWindow) -> Commits {
        let commits: Commits = Rc::new(RefCell::new(Vec::new()));
        win.on_param_commit({
            let commits = commits.clone();
            move |_stage, name, text| {
                commits.borrow_mut().push((name.to_string(), text.to_string()));
            }
        });
        commits
    }

    /// One test, four scenarios: the Slint test platform can only be
    /// installed once in a process, and its event loop wants a single thread.
    #[test]
    fn the_widgets_and_the_model_stay_in_step() {
        testing::init_integration_test_with_system_time();
        slint::spawn_local(async move {
            typed_then_clicked_away_is_committed().await;
            a_typed_field_still_follows_the_model().await;
            a_dropdown_follows_the_model().await;
            an_endpoint_picker_survives_its_list_changing().await;
            the_bus_pane_comes_up_and_commits_its_name().await;
            a_picker_never_runs_into_the_control_beside_it().await;
            the_window_fits_its_narrowest_size().await;
            the_refusal_dialog_wraps_and_closes().await;
            a_gr_strip_shows_only_where_there_is_a_reduction().await;
            a_refresh_leaves_lists_that_did_not_change_alone().await;
            undo_and_redo_from_the_keyboard().await;
            slint::quit_event_loop().unwrap();
        })
        .unwrap();
        slint::run_event_loop().unwrap();
    }

    /// The bug this was written for: `LineEdit::accepted` only fires on
    /// Enter, so a figure typed and then clicked away from was shown but
    /// never committed -- and the setting looked saved until the next start.
    async fn typed_then_clicked_away_is_committed() {
        let win = one_stage_window(vec![number_param("b3-freq", "1000", 0.5)]);
        show(&win);
        let commits = record_commits(&win);

        let field = field_of(&win, "NumberEditor::field");
        let elsewhere = field_of(&win, "NameField::field");

        // What a person does to sixteen EQ fields in a row.
        field.single_click(PointerEventButton::Left).await;
        field.set_accessible_value("2500");
        elsewhere.single_click(PointerEventButton::Left).await;

        assert_eq!(
            *commits.borrow(),
            [("b3-freq".to_string(), "2500".to_string())],
            "leaving a field must commit what was typed in it"
        );
    }

    /// Typing into a `LineEdit` writes its `text`, which removes the binding
    /// that was feeding it from the model. Everything after that has to be
    /// pushed in by hand, or the field shows a figure that is not real.
    async fn a_typed_field_still_follows_the_model() {
        let win = one_stage_window(vec![number_param("b3-freq", "1000", 0.5)]);
        show(&win);
        record_commits(&win);

        let field = field_of(&win, "NumberEditor::field");
        let elsewhere = field_of(&win, "NameField::field");

        field.single_click(PointerEventButton::Left).await;
        field.set_accessible_value("99999");
        elsewhere.single_click(PointerEventButton::Left).await;

        // Nothing here updates the model, which is what a filter refusing a
        // value looks like: the field must go back to what is real rather
        // than leave the rejected figure sitting there.
        assert_eq!(
            field.accessible_value().unwrap(),
            "1000",
            "a value the model did not take must not stay on screen"
        );

        // And a later model change -- a preset being loaded, a filter
        // clamping -- still has to reach it. Written the way `refresh()`
        // writes it: into the row of the model that is already there, never
        // by handing over a new model, which would rebuild the widget and
        // paper over the whole problem.
        let params = win.get_stages().row_data(0).unwrap().params;
        params.set_row_data(0, number_param("b3-freq", "440", 0.3));
        settle(&win).await;
        assert_eq!(
            field.accessible_value().unwrap(),
            "440",
            "the field must still follow the model after being typed into"
        );
    }

    /// The endpoint lists move on their own: every device that appears and
    /// every application that starts playing is a new entry, sorted into a
    /// list the chain's own index is read against. A daemon restart empties
    /// the list altogether and then fills it again.
    ///
    /// `ComboBoxBase::reset-current()` clamps and *writes* its `current-index`
    /// whenever the model changes, and in Slint a write removes the binding
    /// feeding that property. The write is a no-op while the clamp does not
    /// move the value, which is why this is intermittent -- but the first
    /// time the list shrinks past the selection the dropdown comes off its
    /// model for good and shows whatever index it kept, read against a list
    /// that has since grown back. `Picker` pushes the index in again.
    async fn an_endpoint_picker_survives_its_list_changing() {
        let win = one_stage_window(vec![number_param("b3-freq", "1000", 0.5)]);
        // Only the source list carries device names, so whichever Picker
        // reports one is the one under test and the order the three of them
        // are built in does not matter.
        win.set_width_options(strings(&["Stereo".into()]));
        win.set_width_index(0);
        win.set_sink_options(strings(&["\u{2014} no output \u{2014}".into()]));
        win.set_sink_index(0);

        let full = [
            "\u{2014} no input \u{2014}".to_string(),
            "Line In".to_string(),
            "Webcam".to_string(),
            "Mic".to_string(),
        ];
        win.set_source_options(strings(&full));
        win.set_source_index(3);
        show(&win);
        settle(&win).await;

        let shown = || -> Vec<String> {
            testing::ElementHandle::find_by_element_type_name(&win, "Picker")
                .filter_map(|e| e.accessible_value().map(|v| v.to_string()))
                .collect()
        };
        assert!(shown().contains(&"Mic".to_string()), "got {:?}", shown());

        // The daemon goes away: everything but the saved endpoint leaves the
        // list, and the clamp drags the selection down with it.
        win.set_source_options(strings(&[
            "\u{2014} no input \u{2014}".to_string(),
            "Mic".to_string(),
        ]));
        win.set_source_index(1);
        settle(&win).await;
        assert!(shown().contains(&"Mic".to_string()), "got {:?}", shown());

        // And it comes back. This is the one the widget gets wrong on its
        // own: it kept the index the clamp left it with and now reads it
        // against the full list again.
        win.set_source_options(strings(&full));
        win.set_source_index(3);
        settle(&win).await;
        assert!(
            shown().contains(&"Mic".to_string()),
            "the picker must follow its model after the list shrank and grew \
             back; got {:?}",
            shown()
        );
    }

    /// A `Picker` has to take the width it is given.
    ///
    /// It inherits `ComboBox`, which asks for at least 160px. While it was a
    /// layout wrapping one, a narrower width on the wrapper could not be
    /// passed on -- a layout cannot give a child less than its minimum -- so
    /// the box hung 10px out of its slot and over the button beside it. The
    /// preset dropdown is the one narrow enough for that to bite, and the
    /// gap between it and "Load" simply vanished.
    ///
    /// Nothing about that fails to compile or looks wrong in the markup, so
    /// this measures the real window instead: every dropdown keeps a row's
    /// worth of space between itself and whatever button follows it.
    /// The dialog a refused window shows instead. Its text is long and has
    /// to wrap inside the dialog's width rather than widen the window off
    /// the screen, and OK has to take it away -- which is what lets `main`
    /// return and the process end.
    async fn the_refusal_dialog_wraps_and_closes() {
        let (heading, body) = refusal(Some(4242), |_| Some(true));
        let win = RefusedWindow::new().unwrap();
        win.set_heading(heading.into());
        win.set_body(body.into());
        let dismissed = Rc::new(Cell::new(false));
        win.on_dismiss({
            let dismissed = dismissed.clone();
            move || dismissed.set(true)
        });
        win.show().unwrap();

        let width = win.window().size().to_logical(win.window().scale_factor()).width;
        let text = testing::ElementHandle::find_by_element_id(&win, "RefusedWindow::body")
            .next()
            .expect("the dialog has its body text");
        assert!(text.size().width <= width, "the text fits the dialog");
        assert!(
            text.size().height > 3.0 * 13.0,
            "a body this long wraps onto several lines, got {}",
            text.size().height
        );

        testing::ElementHandle::find_by_element_id(&win, "RefusedWindow::ok")
            .next()
            .expect("the dialog has an OK button")
            .single_click(PointerEventButton::Left)
            .await;
        assert!(dismissed.get(), "OK dismisses the dialog");
        win.hide().unwrap();
    }

    /// The bug this was written for: the bus pane handed its dropdown a new
    /// list on every refresh, and a `gr` line counted as a reason to refresh
    /// everything, so with a compressor running the application list was
    /// replaced a dozen times a second -- it flickered, and no entry could be
    /// picked. Driven through the real `App::refresh`, which is where it was.
    async fn a_refresh_leaves_lists_that_did_not_change_alone() {
        assert!(is_meter_reading(&ProcEvent::Param {
            node: "pwfx.voice.comp".into(),
            name: "gr".into(),
            value: "6.0".into(),
        }));
        assert!(is_meter_reading(&ProcEvent::Param {
            node: "pwfx.voice.duck".into(),
            name: "sc-level".into(),
            value: "-12.0".into(),
        }));

        let mut st = state::AppState::default();
        st.buses.push(Bus {
            id: "music".into(),
            name: "Music".into(),
            enabled: true,
            width: 2,
            kind: BusKind::Sink,
            apps: Vec::new(),
        });
        st.chains.push(Chain {
            id: "voice".into(),
            name: "Voice".into(),
            enabled: false,
            width: 2,
            stages: Vec::new(),
            input: None,
            output: None,
        });
        let mut engine = Engine::new(Vec::new(), st, Arc::new(|_| {}));
        let stream = |name: &str, app: &str| graph::Node {
            id: 1,
            name: name.into(),
            description: name.into(),
            media_class: "Stream/Output/Audio".into(),
            app: Some(app.into()),
            ports: Vec::new(),
        };
        engine.graph = graph::Graph {
            nodes: vec![stream("ff", "Firefox"), stream("mpv", "mpv")],
            connected: true,
            ..Default::default()
        };

        let win = AppWindow::new().unwrap();
        let app = App {
            engine: Rc::new(RefCell::new(engine)),
            view: Rc::new(RefCell::new(View {
                focus: Focus::Bus,
                ..Default::default()
            })),
            win: win.as_weak(),
            pw: Rc::new(pwthread::start(|_| {})),
            dirty: Rc::new(Cell::new(false)),
            models: Rc::new(RefCell::new(Models::default())),
            links: Rc::new(Cell::new((0, 0))),
            history: Rc::new(RefCell::new(history::History::default())),
        };

        app.refresh();
        let options = win.get_bus_app_options();
        assert_eq!(options.row_count(), 3, "the prompt, Firefox and mpv");
        let (apps, uses, widths) = (win.get_bus_apps(), win.get_bus_uses(), win.get_bus_width_options());
        app.refresh();
        assert!(win.get_bus_app_options() == options, "the application list was replaced");
        assert!(win.get_bus_apps() == apps, "the application rows were replaced");
        assert!(win.get_bus_uses() == uses, "the chain rows were replaced");
        assert!(win.get_bus_width_options() == widths, "the width list was replaced");

        // A real change still gets through.
        app.engine.borrow_mut().graph.nodes.pop();
        app.refresh();
        assert!(win.get_bus_app_options() != options);
        assert_eq!(win.get_bus_app_options().row_count(), 2);

        // And the chain view's width list, which had the same fault.
        app.view.borrow_mut().focus = Focus::Chain;
        app.refresh();
        let widths = win.get_width_options();
        app.refresh();
        assert!(win.get_width_options() == widths, "the chain width list was replaced");
    }

    fn keys(win: &AppWindow, combo: &[&str]) {
        use slint::platform::{Key, WindowEvent};
        let name = |k: &str| -> SharedString {
            match k {
                "ctrl" => Key::Control.into(),
                "shift" => Key::Shift.into(),
                other => other.into(),
            }
        };
        for k in combo {
            win.window().dispatch_event(WindowEvent::KeyPressed { text: name(k) });
        }
        for k in combo.iter().rev() {
            win.window().dispatch_event(WindowEvent::KeyReleased { text: name(k) });
        }
    }

    /// Undo and redo through the window as a user drives it: an edit made
    /// by the window's own callback, Ctrl+Z and Ctrl+Shift+Z as key events,
    /// a slider dragged through forty positions that comes back in one step,
    /// and a text field that keeps Ctrl+Z for itself while it has focus.
    async fn undo_and_redo_from_the_keyboard() {
        let (plugins, _) = plugin::discover(&[std::path::PathBuf::from("../bin")]);
        if !plugins.iter().any(|p| p.name == "pw_comp") {
            eprintln!("no pw_comp built; skipping the undo window test");
            return;
        }
        let mut st = state::AppState::default();
        st.chains.push(Chain {
            id: "voice".into(),
            name: "Voice".into(),
            enabled: true,
            width: 2,
            stages: vec![Stage {
                id: "comp".into(),
                plugin: "pw_comp".into(),
                bypass: false,
                sidechain: None,
                params: [("ratio".to_string(), "3".to_string())].into_iter().collect(),
            }],
            input: None,
            output: None,
        });
        let win = AppWindow::new().unwrap();
        let app = App {
            engine: Rc::new(RefCell::new(Engine::new(plugins, st.clone(), Arc::new(|_| {})))),
            view: Rc::new(RefCell::new(View::default())),
            win: win.as_weak(),
            pw: Rc::new(pwthread::start(|_| {})),
            dirty: Rc::new(Cell::new(false)),
            models: Rc::new(RefCell::new(Models::default())),
            links: Rc::new(Cell::new((0, 0))),
            history: Rc::new(RefCell::new(history::History::default())),
        };
        app.wire(&win);
        app.refresh();
        show(&win);
        assert!(!win.get_can_undo() && !win.get_can_redo());

        let enabled = || app.engine.borrow().state.chains[0].enabled;
        win.invoke_set_chain_enabled(false);
        assert!(!enabled() && win.get_can_undo());

        keys(&win, &["ctrl", "z"]);
        assert!(enabled(), "Ctrl+Z undid the edit");
        assert!(win.get_can_redo() && !win.get_can_undo());
        keys(&win, &["ctrl", "shift", "Z"]);
        assert!(!enabled(), "Ctrl+Shift+Z redid it");
        keys(&win, &["ctrl", "z"]);
        keys(&win, &["ctrl", "y"]);
        assert!(!enabled(), "and so does Ctrl+Y");
        keys(&win, &["ctrl", "z"]);
        assert!(enabled());

        // A drag is one step, however many positions it went through.
        let before = app.engine.borrow().state.clone();
        for i in 0..40 {
            win.invoke_param_slide(0, "ratio".into(), 0.2 + 0.015 * i as f32);
        }
        let dragged = app.engine.borrow().state.clone();
        assert_ne!(dragged, before);
        keys(&win, &["ctrl", "z"]);
        assert_eq!(app.engine.borrow().state, before, "one Ctrl+Z took the whole drag back");
        keys(&win, &["ctrl", "shift", "Z"]);
        assert_eq!(app.engine.borrow().state, dragged, "and one Ctrl+Shift+Z put it back");

        // Typing in a field: Ctrl+Z is the field's.
        let field = field_of(&win, "NameField::field");
        field.single_click(PointerEventButton::Left).await;
        let state = app.engine.borrow().state.clone();
        keys(&win, &["ctrl", "z"]);
        assert_eq!(app.engine.borrow().state, state, "a focused text field keeps Ctrl+Z");
        win.hide().unwrap();
    }

    /// The third meter strip is there for a dynamics filter and absent for
    /// the rest, rather than an always-empty strip on every stage.
    async fn a_gr_strip_shows_only_where_there_is_a_reduction() {
        let captions = |win: &AppWindow| -> Vec<String> {
            testing::ElementHandle::find_by_element_id(win, "LevelStrip::caption")
                .filter_map(|e| e.accessible_label().map(|s| s.to_string()))
                .collect()
        };

        let win = one_stage_window(vec![number_param("b3-freq", "1000", 0.5)]);
        win.set_metering(true);
        show(&win);
        settle(&win).await;
        assert_eq!(captions(&win), ["in", "out"]);

        win.set_levels(ModelRc::new(VecModel::from(vec![level_row(-12.0, -20.0, Some(8.0), None)])));
        settle(&win).await;
        assert_eq!(captions(&win), ["in", "out", "gr"]);
        let figures: Vec<String> = testing::ElementHandle::find_by_element_id(&win, "LevelStrip::figure")
            .filter_map(|e| e.accessible_label().map(|s| s.to_string()))
            .collect();
        assert!(figures.contains(&"-8.0".to_string()), "{figures:?}");

        // Below in and out, on a row of its own, with its figure in the same
        // right-hand column as out's.
        let by_caption = |name: &str| {
            testing::ElementHandle::find_by_element_id(&win, "LevelStrip::caption")
                .find(|e| e.accessible_label().as_deref() == Some(name))
                .unwrap()
        };
        let (inn, out, gr) = (by_caption("in"), by_caption("out"), by_caption("gr"));
        assert_eq!(inn.absolute_position().y, out.absolute_position().y);
        assert!(gr.absolute_position().y >= inn.absolute_position().y + inn.size().height,
                "gr sits below in and out");
        assert_eq!(gr.absolute_position().x, inn.absolute_position().x,
                   "and starts in the same column as in");
        let figure = |text: &str| {
            testing::ElementHandle::find_by_element_id(&win, "LevelStrip::figure")
                .find(|e| e.accessible_label().as_deref() == Some(text))
                .unwrap()
        };
        let (out_fig, gr_fig) = (figure("-20.0"), figure("-8.0"));
        let right = |e: &testing::ElementHandle| e.absolute_position().x + e.size().width;
        assert!((right(&out_fig) - right(&gr_fig)).abs() < 0.5,
                "the gr figure lines up under the out figure");

        // A sidechain filter adds the key's level, between the levels and
        // the reduction it causes, and its figure in the same column.
        win.set_levels(ModelRc::new(VecModel::from(vec![level_row(
            -12.0,
            -20.0,
            Some(8.0),
            Some(-6.5),
        )])));
        settle(&win).await;
        assert_eq!(captions(&win), ["in", "out", "sc", "gr"]);
        let (sc, gr) = (by_caption("sc"), by_caption("gr"));
        assert!(sc.absolute_position().y < gr.absolute_position().y, "sc sits above gr");
        assert!((right(&figure("-6.5")) - right(&figure("-20.0"))).abs() < 0.5,
                "the sc figure lines up under the out figure");
        win.hide().unwrap();
    }

    /// At the window's minimum width every button and every dropdown is
    /// inside the window. They were not: "Restore default" hung off the
    /// toolbar, and the chain header's width picker and "Delete chain" off
    /// its row once the chain's latency was added to it -- each found by
    /// looking, at a width no test had looked at.
    async fn the_window_fits_its_narrowest_size() {
        let win = one_stage_window(vec![number_param("b3-freq", "1000", 0.5)]);
        win.set_presets(strings(&["voice".into()]));
        win.set_chain_latency("+10.0 ms latency".into());
        // A sidechain row too, with the longest things it says: its picker
        // shares the card's width with a label and a detail.
        let rows = win.get_stages();
        let mut row = rows.row_data(0).unwrap();
        row.has_sidechain = true;
        row.sc_detail = "following Scarlett 2i2 4th Gen Headphones / Line 1-2 · 2 links".into();
        rows.set_row_data(0, row);
        win.set_sidechain_options(strings(&[
            "— none —".into(),
            "Scarlett 2i2 4th Gen Headphones / Line 1-2  ·  2 ch  ·  monitor".into(),
        ]));
        show(&win);
        let min_w = 900.0;
        win.window().set_size(slint::LogicalSize::new(min_w, 700.0));
        settle(&win).await;

        let mut seen: Vec<(String, f32)> =
            testing::ElementHandle::find_by_element_id(&win, "Btn::label")
                .map(|e| {
                    let right = e.absolute_position().x + e.size().width;
                    (e.accessible_label().map(|s| s.to_string()).unwrap_or_default(), right)
                })
                .collect();
        seen.extend(
            testing::ElementQuery::from_root(&win)
                .match_inherits("ComboBox")
                .find_all()
                .iter()
                .map(|e| ("a dropdown".to_string(), e.absolute_position().x + e.size().width)),
        );
        for want in ["Restore default", "Delete chain"] {
            assert!(seen.iter().any(|(t, _)| t == want), "no {want:?} in {seen:?}");
        }
        for (text, right) in &seen {
            assert!(*right <= min_w - 8.0, "{text:?} ends at {right}, past the {min_w} window");
        }
        win.hide().unwrap();
    }

    async fn a_picker_never_runs_into_the_control_beside_it() {
        let win = one_stage_window(vec![number_param("b3-freq", "1000", 0.5)]);
        win.set_presets(strings(&["voice".into(), "guitar".into()]));
        win.set_preset_index(0);
        win.set_plugin_options(strings(&["pw_eq \u{2014} five bands".into()]));
        win.set_source_options(strings(&["\u{2014} no input \u{2014}".into()]));
        win.set_sink_options(strings(&["\u{2014} no output \u{2014}".into()]));
        show(&win);
        settle(&win).await;

        // (left, right, top, bottom) of everything that shares a toolbar or
        // a card row with a dropdown.
        let rect = |e: &testing::ElementHandle| {
            let (p, s) = (e.absolute_position(), e.size());
            (p.x, p.x + s.width, p.y, p.y + s.height)
        };
        // By what it inherits rather than by name, so this measures the box
        // that is actually drawn however `Picker` comes to be one. Measuring
        // a wrapper instead would report the slot rather than what is
        // sticking out of it, which is the whole fault.
        let pickers: Vec<_> = testing::ElementQuery::from_root(&win)
            .match_inherits("ComboBox")
            .find_all()
            .iter()
            .map(rect)
            .collect();
        let buttons: Vec<_> = testing::ElementHandle::find_by_element_id(&win, "Btn::label")
            .map(|e| rect(&e))
            .collect();
        assert!(pickers.len() >= 4, "expected the window to build its dropdowns");

        // The one the bug was reported on, stated as what it should be
        // rather than as "not overlapping": `width: 130px` means 130px.
        let toolbar = pickers
            .iter()
            .find(|p| p.2 < 60.0)
            .expect("no dropdown in the toolbar");
        assert_eq!(
            toolbar.1 - toolbar.0,
            130.0,
            "the preset dropdown must take the width the toolbar gives it"
        );

        const GAP: f32 = 8.0; // Theme.gap
        for p in &pickers {
            for b in &buttons {
                let same_row = b.2 < p.3 && b.3 > p.2;
                if same_row && b.0 >= p.0 {
                    assert!(
                        b.0 >= p.1 + GAP,
                        "a dropdown ending at {} leaves no room before the \
                         button at {}",
                        p.1,
                        b.0
                    );
                }
            }
        }
    }

    /// The bus pane is the other half of the window and has its own name
    /// field, so it gets the same two questions: does it lay out at all, and
    /// does what was typed into it survive being clicked away from.
    async fn the_bus_pane_comes_up_and_commits_its_name() {
        let win = AppWindow::new().unwrap();
        win.set_has_chain(false);
        win.set_has_bus(true);
        win.set_bus_name("Podcast Mic".into());
        win.set_bus_enabled(true);
        win.set_bus_node_name("pwfx.bus.mic".into());
        win.set_bus_status("live · 2 inputs, 2 outputs".into());
        win.set_bus_live(true);
        win.set_bus_explain("Chains send audio into this bus.".into());
        win.set_bus_kind_options(ModelRc::new(VecModel::from(vec![
            SharedString::from("Source"),
            SharedString::from("Sink"),
        ])));
        win.set_bus_width_options(ModelRc::new(VecModel::from(vec![SharedString::from("Stereo")])));
        win.set_bus_uses(ModelRc::new(VecModel::from(vec![BusUseRow {
            chain: "Voice".into(),
            direction: "sends into this bus".into(),
        }])));
        show(&win);

        let renames: Rc<RefCell<Vec<String>>> = Rc::new(RefCell::new(Vec::new()));
        win.on_rename_bus({
            let renames = renames.clone();
            move |name| renames.borrow_mut().push(name.to_string())
        });

        // The pane laid out far enough for its field to have a position.
        let field = field_of(&win, "NameField::field");
        assert_eq!(field.accessible_value().unwrap(), "Podcast Mic");

        field.single_click(PointerEventButton::Left).await;
        field.set_accessible_value("Studio Mic");
        // Focus leaves without an Enter, which is how most renames end.
        win.window()
            .dispatch_event(slint::platform::WindowEvent::WindowActiveChanged(false));
        settle(&win).await;

        assert_eq!(
            *renames.borrow(),
            ["Studio Mic".to_string()],
            "leaving the bus name field must commit what was typed in it"
        );
        // --- the applications card ---
        win.window()
            .dispatch_event(slint::platform::WindowEvent::WindowActiveChanged(true));
        win.set_bus_apps_heading("APPLICATIONS RECORDING FROM THIS BUS".into());
        win.set_bus_apps(ModelRc::new(VecModel::from(vec![BusAppRow {
            name: "Zoom".into(),
            detail: "recording · 1 stream".into(),
            running: true,
        }])));
        win.set_bus_app_options(strings(&[
            "— add an application that is recording —".into(),
            "OBS".into(),
        ]));
        win.set_bus_app_index(0);
        let calls: Rc<RefCell<Vec<String>>> = Rc::new(RefCell::new(Vec::new()));
        win.on_add_bus_app({
            let calls = calls.clone();
            move || calls.borrow_mut().push("add".into())
        });
        win.on_remove_bus_app({
            let calls = calls.clone();
            move |i| calls.borrow_mut().push(format!("remove {i}"))
        });
        settle(&win).await;

        let button = |text: &str| {
            testing::ElementHandle::find_by_element_id(&win, "Btn::label")
                .filter(|e| e.accessible_label().as_deref() == Some(text))
                .last()
                .unwrap_or_else(|| panic!("no {text} button"))
        };
        // Nothing picked: Add does nothing rather than adding the prompt.
        button("Add").single_click(PointerEventButton::Left).await;
        assert!(calls.borrow().is_empty(), "{:?}", calls.borrow());
        win.set_bus_app_index(1);
        button("Add").single_click(PointerEventButton::Left).await;
        button("\u{2715}").single_click(PointerEventButton::Left).await;
        assert_eq!(*calls.borrow(), ["add", "remove 0"]);

        // The picker stops short of the button beside it, the same rule the
        // chain view is measured against.
        let picker = testing::ElementQuery::from_root(&win)
            .match_inherits("ComboBox")
            .find_all()
            .into_iter()
            .max_by(|a, b| a.absolute_position().y.total_cmp(&b.absolute_position().y))
            .expect("the applications card has its picker");
        let add = button("Add");
        let picker_right = picker.absolute_position().x + picker.size().width;
        assert!(
            picker_right <= add.absolute_position().x,
            "the app picker ({picker_right}) runs into Add ({})",
            add.absolute_position().x
        );
        win.hide().unwrap();
    }

    /// Same again for the one enum the filters have, `pw_eq`'s bell design.
    async fn a_dropdown_follows_the_model() {
        let mut p = number_param("bells", "orfanidis", 0.0);
        p.kind = "enum".into();
        p.choices = ModelRc::new(VecModel::from(vec![
            SharedString::from("orfanidis"),
            SharedString::from("rbj"),
        ]));
        p.choice_index = 0;

        let win = one_stage_window(vec![p.clone()]);
        show(&win);

        let mut rbj = p.clone();
        rbj.value = "rbj".into();
        rbj.choice_index = 1;
        win.get_stages()
            .row_data(0)
            .unwrap()
            .params
            .set_row_data(0, rbj);
        settle(&win).await;

        assert_eq!(
            field_of(&win, "EnumEditor::box").accessible_value().unwrap(),
            "rbj",
            "the dropdown must show what the model says"
        );
    }
}

/// Not a test: a way to look at the window without a screen. Renders a chain
/// of real filters, with the curves the real binaries report, through the
/// real `refresh()` and Slint's software renderer, and writes the pixels to
/// `$PWFX_RENDER_OUT` as `width height` and then raw RGBA. Run it alone --
/// it installs its own platform, which can be done once per process:
///
///     PWFX_RENDER_OUT=/tmp/w.rgba cargo test render_the_window -- --ignored
#[cfg(test)]
mod render {
    use super::*;
    use slint::platform::software_renderer::{
        MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType,
    };
    use std::collections::BTreeMap;

    struct Offscreen(Rc<MinimalSoftwareWindow>);
    impl slint::platform::Platform for Offscreen {
        fn create_window_adapter(
            &self,
        ) -> Result<Rc<dyn slint::platform::WindowAdapter>, slint::PlatformError> {
            Ok(self.0.clone())
        }
    }

    fn curve_of(bin: &str, args: &[&str]) -> String {
        let out = std::process::Command::new(format!("../bin/{bin}"))
            .args(args)
            .arg("--curve")
            .output()
            .expect("the filters are built");
        let line = String::from_utf8(out.stdout).unwrap();
        line.trim().strip_prefix("curve ").unwrap().to_string()
    }

    #[test]
    #[ignore]
    fn render_the_window() {
        let Ok(out) = std::env::var("PWFX_RENDER_OUT") else { return };
        let (w, h) = (1180u32, 2400u32);
        let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
        slint::platform::set_platform(Box::new(Offscreen(window.clone()))).unwrap();
        window.set_size(slint::PhysicalSize::new(w, h));

        let (plugins, _) = plugin::discover(&[std::path::PathBuf::from("../bin")]);
        let stage = |id: &str, plugin: &str, params: &[(&str, &str)]| Stage {
            id: id.into(),
            plugin: plugin.into(),
            bypass: false,
            sidechain: None,
            params: params.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect::<BTreeMap<_, _>>(),
        };
        let mut st = state::AppState::default();
        st.meters = false;
        st.chains.push(Chain {
            id: "voice".into(),
            name: "Voice".into(),
            enabled: true,
            width: 2,
            stages: vec![
                stage("gain", "pw_gain", &[("gain", "-6"), ("balance", "20")]),
                stage("hpf", "pw_hpf", &[("cutoff", "80"), ("order", "2")]),
                stage("eq", "pw_eq", &[("b3-freq", "2500"), ("b3-gain", "4"), ("b3-q", "1.2"),
                                        ("ls-freq", "200"), ("ls-gain", "-3")]),
                stage("comp", "pw_comp", &[("threshold", "-24"), ("ratio", "3"), ("knee", "6"),
                                           ("makeup", "6")]),
                stage("gate", "pw_gate", &[("threshold", "-45"), ("hysteresis", "6"), ("range", "30")]),
            ],
            input: None,
            output: None,
        });
        let mut engine = Engine::new(plugins, st, Arc::new(|_| {}));
        for (node, bin, args) in [
            ("pwfx.voice.gain", "pw_gain", &["-g", "-6", "-b", "20"][..]),
            ("pwfx.voice.hpf", "pw_hpf", &["-f", "80", "-n", "2"][..]),
            ("pwfx.voice.eq", "pw_eq", &["--b3-freq", "2500", "--b3-gain", "4", "--b3-q", "1.2",
                                         "--ls-freq", "200", "--ls-gain", "-3"][..]),
            ("pwfx.voice.comp", "pw_comp", &["-t", "-24", "-R", "3", "-w", "6", "-m", "6"][..]),
            ("pwfx.voice.gate", "pw_gate", &["-t", "-45", "-T", "6", "-g", "30"][..]),
        ] {
            let rt = engine.runtime_mut(node);
            rt.running = true;
            rt.curve = engine::Curve::parse(&curve_of(bin, args));
            // the lookahead filters' default 5 ms, as they report it
            if bin == "pw_comp" || bin == "pw_gate" {
                rt.latency = 240;
                rt.reported.insert("rate".into(), "48000".into());
            }
        }

        let win = AppWindow::new().unwrap();
        let app = App {
            engine: Rc::new(RefCell::new(engine)),
            view: Rc::new(RefCell::new(View::default())),
            win: win.as_weak(),
            pw: Rc::new(pwthread::start(|_| {})),
            dirty: Rc::new(Cell::new(false)),
            models: Rc::new(RefCell::new(Models::default())),
            links: Rc::new(Cell::new((0, 0))),
            history: Rc::new(RefCell::new(history::History::default())),
        };
        app.refresh();
        win.show().unwrap();

        let mut buf = vec![PremultipliedRgbaColor::default(); (w * h) as usize];
        window.draw_if_needed(|r| {
            r.render(&mut buf, w as usize);
        });
        let mut bytes = format!("{w} {h}\n").into_bytes();
        for p in &buf {
            bytes.extend_from_slice(&[p.red, p.green, p.blue, 255]);
        }
        std::fs::write(out, bytes).unwrap();
    }
}
