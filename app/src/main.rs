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
mod plugin;
mod process;
mod pwthread;
mod state;
mod ui;

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::mpsc::{channel, Receiver};
use std::sync::Arc;
use std::time::Duration;

use slint::{ComponentHandle, Model, ModelRc, SharedString, VecModel};

use engine::Engine;
use graph::{Dir, Mapping};
use plugin::ParamKind;
use process::ProcEvent;
use pwthread::{NodeSpec, PwCmd, PwEvent};
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
    /// Which end the channel matrix is open on, if any.
    map: Option<Dir>,
    /// Port names in the matrix's row order.
    map_ports: Vec<String>,
    presets: Vec<String>,
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
    last_links: Rc<RefCell<Vec<pwthread::LinkSpec>>>,
    last_nodes: Rc<RefCell<Vec<NodeSpec>>>,
}

fn main() {
    let dirs = plugin::search_path();
    let (plugins, errors) = plugin::discover(&dirs);

    let (state, load_error) = state::startup();

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

    let win = AppWindow::new().expect("cannot create the window");
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
        last_links: Rc::new(RefCell::new(Vec::new())),
        last_nodes: Rc::new(RefCell::new(Vec::new())),
    };

    app.wire(&win);
    app.apply();
    app.refresh();

    let _ticker = app.start_ticker(rx);
    let _keeper = app.start_housekeeping();

    win.run().expect("the event loop failed");

    // Leaving: stop the filters before the links, so the graph never sees a
    // node disappear out from under a link it is still carrying. The buses go
    // last, for the same reason and one level up -- and they would go anyway
    // when the PipeWire thread drops its proxies, so this is only about the
    // order it happens in.
    app.save_session();
    app.last_links.borrow_mut().clear();
    app.pw.send(PwCmd::SetLinks(Vec::new()));
    app.engine.borrow_mut().shutdown();
    app.last_nodes.borrow_mut().clear();
    app.pw.send(PwCmd::SetNodes(Vec::new()));
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
                        let level = matches!(&p, ProcEvent::Param { name, .. }
                                             if name == "in-level" || name == "out-level");
                        app.engine.borrow_mut().on_proc_event(p);
                        if level {
                            meters = true;
                        } else {
                            full = true;
                        }
                    }
                    AppEvent::Pw(PwEvent::Graph(g)) => {
                        let mut e = app.engine.borrow_mut();
                        if g.connected && !e.graph.connected {
                            // The daemon is back. Whatever was stopping the
                            // filters from connecting is gone with it.
                            e.clear_backoff();
                        }
                        e.graph = g;
                        drop(e);
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

    /// Reconcile the processes, and publish the buses and the links the state
    /// implies.
    ///
    /// Each set is only sent when it has actually moved: dragging a slider
    /// reconciles processes twenty times a second and would otherwise wake
    /// the PipeWire thread just as often to be told nothing changed. The
    /// buses go first, so that a chain and the bus it was just pointed at
    /// cannot arrive in the wrong order.
    fn apply(&self) {
        let (nodes, links) = self.engine.borrow_mut().apply();
        if *self.last_nodes.borrow() != nodes {
            *self.last_nodes.borrow_mut() = nodes.clone();
            self.pw.send(PwCmd::SetNodes(nodes));
        }
        if *self.last_links.borrow() != links {
            *self.last_links.borrow_mut() = links.clone();
            self.pw.send(PwCmd::SetLinks(links));
        }
    }

    /// Every edit goes through here: change the state, then let the engine
    /// work out what that means.
    fn edit(&self, f: impl FnOnce(&mut Engine, &mut View)) {
        f(&mut self.engine.borrow_mut(), &mut self.view.borrow_mut());
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
                    .flatten()
                    .any(|ep| e.graph.lookup(&ep.node).is_none());
                ChainRow {
                    id: c.id.as_str().into(),
                    name: c.name.as_str().into(),
                    enabled: c.enabled,
                    detail: chain_detail(c).into(),
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
            win.set_bus_width_options(strings(&widths));
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
            win.set_bus_uses(ModelRc::new(VecModel::from(uses)));
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
        let widths: Vec<String> = (1..=chain.width.max(8)).map(ui::width_label).collect();
        win.set_width_options(strings(&widths));
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
                .map(|(s, p)| stage_row(&e, chain, s, ModelRc::from(p.clone())))
                .collect();
            let n = rows.len();
            let stages = Rc::new(VecModel::from(rows));
            let levels = Rc::new(VecModel::from(vec![level_row(-240.0, -240.0); n]));

            win.set_stages(ModelRc::from(stages.clone()));
            win.set_levels(ModelRc::from(levels.clone()));
            m.stages = Some(stages);
            m.levels = Some(levels);
            m.shape = shape;
        } else if let Some(stages) = m.stages.clone() {
            for (i, stage) in chain.stages.iter().enumerate() {
                if let Some(desc) = e.plugin(&stage.plugin) {
                    let pm = &m.params[i];
                    for (j, row) in param_rows(desc, stage, &mut choices).into_iter().enumerate() {
                        if pm.row_data(j).as_ref() != Some(&row) {
                            pm.set_row_data(j, row);
                        }
                    }
                }
                let row = stage_row(&e, chain, stage, ModelRc::from(m.params[i].clone()));
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
            let (a, b) = e
                .runtime(&chain.node_name(s))
                .map(|r| (r.in_level, r.out_level))
                .unwrap_or((-240.0, -240.0));
            let row = level_row(a, b);
            if levels.row_data(i).as_ref() != Some(&row) {
                levels.set_row_data(i, row);
            }
        }
    }
}

/// The models the window is bound to, kept alive between refreshes.
#[derive(Default)]
struct Models {
    /// One entry per stage, identifying it. A change here, and only a change
    /// here, rebuilds the stage models.
    shape: Vec<String>,
    stages: Option<Rc<VecModel<StageRow>>>,
    params: Vec<Rc<VecModel<ParamRow>>>,
    levels: Option<Rc<VecModel<LevelRow>>>,
    chains_key: Vec<String>,
    buses_key: Vec<String>,
    bus_kinds_key: Vec<String>,
    source_key: Vec<String>,
    sink_key: Vec<String>,
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

fn level_row(in_db: f32, out_db: f32) -> LevelRow {
    LevelRow {
        in_db,
        in_text: ui::level_text(in_db).into(),
        out_db,
        out_text: ui::level_text(out_db).into(),
    }
}

fn strings(v: &[String]) -> ModelRc<SharedString> {
    ModelRc::new(VecModel::from(
        v.iter().map(|s| SharedString::from(s.as_str())).collect::<Vec<_>>(),
    ))
}

fn stage_row(e: &Engine, chain: &Chain, stage: &Stage, params: ModelRc<ParamRow>) -> StageRow {
    let node = chain.node_name(stage);
    let rt = e.runtime(&node).cloned().unwrap_or_default();
    match e.plugin(&stage.plugin) {
        Some(desc) => StageRow {
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

fn chain_detail(c: &Chain) -> String {
    let width = match c.width {
        1 => "mono".to_string(),
        2 => "stereo".to_string(),
        n => format!("{n} ch"),
    };
    format!(
        "{width} · {} filter{}",
        c.stages.len(),
        plural(c.stages.len())
    )
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

    fn load_from(&self, path: &std::path::Path) {
        match state::read(path) {
            Ok(loaded) => {
                self.edit(move |e, v| {
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
        }])));
        win.set_levels(ModelRc::new(VecModel::from(vec![level_row(-240.0, -240.0)])));
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
        // rather than as "not overlapping": `width: 150px` means 150px.
        let toolbar = pickers
            .iter()
            .find(|p| p.2 < 60.0)
            .expect("no dropdown in the toolbar");
        assert_eq!(
            toolbar.1 - toolbar.0,
            150.0,
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
