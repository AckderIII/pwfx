//! Turning the saved state into running processes and wanted links.
//!
//! The engine holds no commands, only a comparison. `apply()` looks at what
//! `AppState` says should be running, looks at what is running, and closes the
//! gap; then it derives the set of links that state implies and hands it to
//! the PipeWire thread, which does the same trick one level down. Every user
//! action is therefore "edit the state, then apply", and there is no path
//! where the GUI and the graph can disagree about what was asked for.
//!
//! That is also what makes loading a preset the same operation as changing one
//! knob: the new state is compared against the running one, the stages that
//! did not move are left alone -- including their audio -- and only the
//! differences are spent.

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::graph::{self, Dir, Graph};
use crate::plugin::PluginDesc;
use crate::process::{EventSink, FilterProc, ProcEvent};
use crate::pwthread::LinkSpec;
use crate::state::{AppState, Chain};

/// How often the meters report, in milliseconds, when they are on.
const METER_MS: u32 = 80;

const LOG_MAX: usize = 400;

/// How long a filter has to survive before starting it counts as having
/// worked. Anything shorter is a failure to start rather than a crash.
const SETTLED: Duration = Duration::from_secs(5);

/// A filter that will not start -- a missing daemon, a binary that segfaults,
/// a setting the graph cannot honour -- would otherwise be respawned on every
/// pass, for as long as the app is open. Back off instead, and say so.
fn backoff(attempts: u32) -> Duration {
    Duration::from_secs(match attempts {
        0 | 1 => 0,
        2 => 1,
        3 => 2,
        4 => 5,
        5 => 15,
        _ => 30,
    })
}

struct Restart {
    attempts: u32,
    next: Instant,
}

#[derive(Clone, Debug)]
pub struct LogLine {
    pub text: String,
    pub error: bool,
}

/// What a stage's process is currently saying about itself.
#[derive(Clone, Debug, Default)]
pub struct Runtime {
    pub running: bool,
    /// Every `name value` the filter has reported, which is the authority on
    /// what its parameters actually are.
    pub reported: BTreeMap<String, String>,
    pub in_level: f32,
    pub out_level: f32,
    pub latency: u32,
    pub last_error: String,
}

pub struct Engine {
    pub state: AppState,
    pub plugins: Vec<PluginDesc>,
    pub graph: Graph,
    pub log: VecDeque<LogLine>,
    /// Keyed by node name, which is unique across the whole app.
    procs: HashMap<String, FilterProc>,
    runtime: HashMap<String, Runtime>,
    restarts: HashMap<String, Restart>,
    sink: EventSink,
    meters_on: bool,
}

impl Engine {
    pub fn new(plugins: Vec<PluginDesc>, state: AppState, sink: EventSink) -> Self {
        Self {
            meters_on: state.meters,
            state,
            plugins,
            graph: Graph::default(),
            log: VecDeque::new(),
            procs: HashMap::new(),
            runtime: HashMap::new(),
            restarts: HashMap::new(),
            sink,
        }
    }

    pub fn plugin(&self, name: &str) -> Option<&PluginDesc> {
        self.plugins.iter().find(|p| p.name == name)
    }

    pub fn runtime(&self, node: &str) -> Option<&Runtime> {
        self.runtime.get(node)
    }

    pub fn note(&mut self, text: impl Into<String>, error: bool) {
        let text = text.into();
        if self.log.back().is_some_and(|l| l.text == text) {
            return; // the same complaint every tick is not new information
        }
        // Also on stderr: a window that will not come up, or a save file that
        // will not load, has to be diagnosable from the terminal the app was
        // started in, where the log panel is of no help at all.
        eprintln!("{}{text}", if error { "error: " } else { "" });
        self.log.push_back(LogLine { text, error });
        while self.log.len() > LOG_MAX {
            self.log.pop_front();
        }
    }

    // -----------------------------------------------------------------
    // reconciliation

    /// Bring the running processes in line with the state, and return the
    /// links that state implies.
    pub fn apply(&mut self) -> Vec<LinkSpec> {
        self.reconcile_processes();
        self.desired_links()
    }

    fn reconcile_processes(&mut self) {
        // What should be running, and with what.
        let mut wanted: Vec<Want> = Vec::new();
        for chain in &self.state.chains {
            if !chain.enabled {
                continue;
            }
            for stage in &chain.stages {
                wanted.push(Want {
                    node: chain.node_name(stage),
                    plugin: stage.plugin.clone(),
                    channels: chain.width,
                    params: stage.params.clone(),
                    bypass: stage.bypass,
                });
            }
        }

        // Stop anything that is no longer wanted, or that no longer matches
        // the shape it was started with. Channel count is a command-line
        // parameter in the filters, so changing a chain's width is the one
        // edit that has to restart its processes.
        let keep: Vec<String> = wanted.iter().map(|w| w.node.clone()).collect();
        let mut stale: Vec<(String, bool, Duration)> = Vec::new();
        for (node, proc) in self.procs.iter_mut() {
            let died = !proc.alive();
            let reshaped = match wanted.iter().find(|w| w.node == *node) {
                None => true,
                Some(w) => w.plugin != proc.plugin || w.channels != proc.channels,
            };
            if died || reshaped {
                stale.push((node.clone(), died, proc.uptime()));
            }
        }
        for (node, died, uptime) in stale {
            if let Some(p) = self.procs.remove(&node) {
                p.shutdown();
            }
            if died {
                let now = Instant::now();
                let r = self.restarts.entry(node.clone()).or_insert(Restart {
                    attempts: 0,
                    next: now,
                });
                // One that had been running is given a free retry; one that
                // never got going is not.
                r.attempts = if uptime < SETTLED { r.attempts + 1 } else { 1 };
                let (attempts, wait) = (r.attempts, backoff(r.attempts));
                r.next = now + wait;
                if !wait.is_zero() {
                    self.note(
                        format!(
                            "{node}: will not start (attempt {attempts}); trying again in {} s",
                            wait.as_secs()
                        ),
                        true,
                    );
                }
            }
            self.runtime.entry(node).or_default().running = false;
        }
        self.runtime.retain(|node, _| keep.contains(node));
        self.restarts.retain(|node, _| keep.contains(node));

        let meter_ms = if self.state.meters { METER_MS } else { 0 };

        for Want {
            node,
            plugin,
            channels,
            params,
            bypass,
        } in wanted
        {
            let Some(desc) = self.plugin(&plugin).cloned() else {
                self.note(
                    format!("{node}: no filter called {plugin} is installed"),
                    true,
                );
                continue;
            };

            if !self.procs.contains_key(&node) {
                if self
                    .restarts
                    .get(&node)
                    .is_some_and(|r| Instant::now() < r.next)
                {
                    continue;
                }
                let mut full = params.clone();
                full.insert("bypass".into(), bool_word(bypass).into());
                match FilterProc::spawn(
                    &desc,
                    &node,
                    channels,
                    &full,
                    meter_ms,
                    Arc::clone(&self.sink),
                ) {
                    Ok(p) => {
                        self.procs.insert(node.clone(), p);
                        self.runtime.entry(node).or_default().running = true;
                    }
                    Err(e) => self.note(format!("{node}: cannot start {plugin}: {e}"), true),
                }
                continue;
            }

            // Running already: send only what actually differs from what the
            // filter last reported. After a preset load that is a handful of
            // lines; after a slider move it is one.
            let rt = self.runtime.entry(node.clone()).or_default();
            let mut changes: Vec<(String, String)> = Vec::new();
            for (name, value) in params.iter().chain(std::iter::once((
                &"bypass".to_string(),
                &bool_word(bypass).to_string(),
            ))) {
                match rt.reported.get(name) {
                    Some(have) if same_value(have, value) => {}
                    _ => changes.push((name.clone(), value.clone())),
                }
            }
            if self.meters_on != self.state.meters {
                changes.push(("meter".into(), meter_ms.to_string()));
            }
            if let Some(p) = self.procs.get_mut(&node) {
                for (name, value) in changes {
                    if let Err(e) = p.set(&name, &value) {
                        self.note(format!("{node}: cannot set {name}: {e}"), true);
                        break;
                    }
                }
            }
        }
        self.meters_on = self.state.meters;
    }

    /// Every link the current state asks for.
    ///
    /// Three kinds, and they are all the same shape once the channel mapping
    /// has been resolved: source into the first stage, stage into stage, last
    /// stage into the sink. A chain with no stages at all is not a special
    /// case either -- it links its source straight to its sink, which is a
    /// useful thing to be able to ask for.
    fn desired_links(&self) -> Vec<LinkSpec> {
        let mut links = Vec::new();

        for chain in &self.state.chains {
            if !chain.enabled {
                continue;
            }
            let nodes: Vec<String> = chain
                .stages
                .iter()
                .filter(|s| self.plugin(&s.plugin).is_some())
                .map(|s| chain.node_name(s))
                .collect();

            let inputs = self.endpoint_pairs(chain, Dir::Out);
            let outputs = self.endpoint_pairs(chain, Dir::In);

            match nodes.first() {
                Some(first) => {
                    for (port, ch) in &inputs {
                        links.push(LinkSpec {
                            out_node: chain.input.as_ref().unwrap().node.clone(),
                            out_port: port.clone(),
                            in_node: first.clone(),
                            in_port: format!("in_{}", ch + 1),
                        });
                    }
                    for pair in nodes.windows(2) {
                        for ch in 0..chain.width {
                            links.push(LinkSpec {
                                out_node: pair[0].clone(),
                                out_port: format!("out_{}", ch + 1),
                                in_node: pair[1].clone(),
                                in_port: format!("in_{}", ch + 1),
                            });
                        }
                    }
                    let last = nodes.last().unwrap();
                    for (port, ch) in &outputs {
                        links.push(LinkSpec {
                            out_node: last.clone(),
                            out_port: format!("out_{}", ch + 1),
                            in_node: chain.output.as_ref().unwrap().node.clone(),
                            in_port: port.clone(),
                        });
                    }
                }
                None => {
                    // No filters: wire the two ends together channel by
                    // channel, so the chain is a plain patch cable.
                    let (Some(src), Some(dst)) = (chain.input.as_ref(), chain.output.as_ref())
                    else {
                        continue;
                    };
                    for (out_port, ch) in &inputs {
                        for (in_port, ch2) in &outputs {
                            if ch == ch2 {
                                links.push(LinkSpec {
                                    out_node: src.node.clone(),
                                    out_port: out_port.clone(),
                                    in_node: dst.node.clone(),
                                    in_port: in_port.clone(),
                                });
                            }
                        }
                    }
                }
            }
        }

        links.sort_by(|a, b| {
            (&a.out_node, &a.out_port, &a.in_node, &a.in_port).cmp(&(
                &b.out_node,
                &b.out_port,
                &b.in_node,
                &b.in_port,
            ))
        });
        links.dedup();
        links
    }

    /// The `(port name, chain channel)` pairs for one end of a chain, or
    /// nothing if that end is unset or its node is not currently present.
    pub fn endpoint_pairs(&self, chain: &Chain, dir: Dir) -> Vec<(String, usize)> {
        let endpoint = match dir {
            Dir::Out => chain.input.as_ref(),
            Dir::In => chain.output.as_ref(),
        };
        let Some(endpoint) = endpoint else {
            return Vec::new();
        };
        let Some(node) = self.graph.node(&endpoint.node) else {
            return Vec::new();
        };
        graph::resolve(
            &endpoint.mapping,
            node,
            dir,
            chain.width,
            dir == Dir::Out, // taking audio from the node: the chain is the destination
        )
    }

    // -----------------------------------------------------------------
    // events

    /// Take in one line from a filter process.
    ///
    /// Events for a stage the app no longer has are dropped rather than
    /// resurrecting it: the reader threads of a process that has just been
    /// stopped are still draining its last few lines, and a stage removed
    /// while that happens must stay removed.
    pub fn on_proc_event(&mut self, e: ProcEvent) {
        match e {
            ProcEvent::Param { node, name, value } => {
                let Some(rt) = self.runtime.get_mut(&node) else {
                    return;
                };
                match name.as_str() {
                    "in-level" => rt.in_level = value.parse().unwrap_or(-240.0),
                    "out-level" => rt.out_level = value.parse().unwrap_or(-240.0),
                    "latency" => {
                        rt.latency = value.parse().unwrap_or(0);
                        rt.reported.insert(name, value);
                    }
                    _ => {
                        rt.reported.insert(name, value);
                    }
                }
                rt.running = true;
            }
            ProcEvent::Note { node, text, error } => {
                if error {
                    if let Some(rt) = self.runtime.get_mut(&node) {
                        rt.last_error = text.clone();
                    }
                }
                self.note(format!("{node}: {text}"), error);
            }
            ProcEvent::Exited { node, text } => {
                // A stage that was deliberately stopped has already been
                // taken out of `procs`; that its process then ends is not
                // news and certainly not an error.
                if let Some(rt) = self.runtime.get_mut(&node) {
                    rt.running = false;
                }
                if self.procs.contains_key(&node) {
                    self.note(format!("{node}: {text}"), true);
                }
            }
        }
    }

    /// Adopt what the filter reported back into the saved state, so that what
    /// gets written out is what the filter actually settled on rather than
    /// what the GUI asked for.
    pub fn absorb_reports(&mut self) -> bool {
        let mut changed = false;
        let snapshot: Vec<(String, BTreeMap<String, String>)> = self
            .runtime
            .iter()
            .map(|(k, v)| (k.clone(), v.reported.clone()))
            .collect();

        for chain in &mut self.state.chains {
            let chain_id = chain.id.clone();
            for stage in &mut chain.stages {
                let node = crate::state::node_name(&chain_id, &stage.id);
                let Some((_, reported)) = snapshot.iter().find(|(n, _)| *n == node) else {
                    continue;
                };
                for (name, value) in stage.params.iter_mut() {
                    if let Some(have) = reported.get(name) {
                        if !same_value(have, value) {
                            *value = have.clone();
                            changed = true;
                        }
                    }
                }
            }
        }
        changed
    }

    /// Forget how often things have failed to start.
    ///
    /// Called when the daemon comes back: whatever was stopping the filters
    /// from connecting has just changed, so the accumulated backoff is stale
    /// and waiting out another thirty seconds of it would be nonsense.
    pub fn clear_backoff(&mut self) {
        self.restarts.clear();
    }

    /// Stop everything, in the orderly way.
    pub fn shutdown(&mut self) {
        for (_, p) in self.procs.drain() {
            p.shutdown();
        }
    }
}

/// One filter that ought to be running, and the shape it ought to have.
struct Want {
    node: String,
    plugin: String,
    channels: usize,
    params: BTreeMap<String, String>,
    bypass: bool,
}

fn bool_word(b: bool) -> &'static str {
    if b {
        "on"
    } else {
        "off"
    }
}

/// Compare two protocol values the way the filter would.
///
/// The filter prints numbers with `%.6g`, so "80.0" comes back as "80" and a
/// textual comparison would see a difference that is not there and resend the
/// same value on every pass.
fn same_value(a: &str, b: &str) -> bool {
    if a == b {
        return true;
    }
    match (a.parse::<f64>(), b.parse::<f64>()) {
        (Ok(x), Ok(y)) => (x - y).abs() <= 1e-9 * x.abs().max(y.abs()).max(1.0),
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::graph::{Mapping, Node, Port};
    use crate::plugin::parse_help;
    use crate::state::{Endpoint, Stage};

    fn plugins() -> Vec<PluginDesc> {
        ["pw_hpf", "pw_comp"]
            .iter()
            .map(|n| {
                parse_help(&format!(
                    "{n} -- a filter\n\nfilter parameters (live, settable on stdin)\n  -f, --cutoff HZ            cutoff\n                             (1..20000 Hz, default 80)\n\nstream and node\n      --bypass on|off        bypass\n                             (default off)\n\ncontrol protocol\n"
                ))
                .unwrap()
            })
            .collect()
    }

    fn device(name: &str, dir: Dir, channels: &[&str]) -> Node {
        Node {
            id: 1,
            name: name.into(),
            description: name.into(),
            media_class: "Audio/Source".into(),
            ports: channels
                .iter()
                .enumerate()
                .map(|(i, c)| Port {
                    id: 10 + i as u32,
                    name: format!("p_{c}"),
                    channel: Some(c.to_string()),
                    dir,
                    index: i as u32,
                })
                .collect(),
        }
    }

    fn engine(chains: Vec<Chain>, nodes: Vec<Node>) -> Engine {
        let mut e = Engine::new(
            plugins(),
            AppState {
                chains,
                ..Default::default()
            },
            Arc::new(|_| {}),
        );
        e.graph = Graph {
            nodes,
            connected: true,
        };
        e
    }

    fn chain(width: usize, stages: &[&str]) -> Chain {
        let mut c = Chain {
            id: "voice".into(),
            name: "Voice".into(),
            enabled: true,
            width,
            stages: vec![],
            input: Some(Endpoint {
                node: "mic".into(),
                label: "Mic".into(),
                mapping: Mapping::Auto,
            }),
            output: Some(Endpoint {
                node: "spk".into(),
                label: "Speakers".into(),
                mapping: Mapping::Auto,
            }),
        };
        for p in stages {
            let id = c.fresh_stage_id(p);
            c.stages.push(Stage {
                id,
                plugin: p.to_string(),
                bypass: false,
                params: BTreeMap::new(),
            });
        }
        c
    }

    fn show(links: &[LinkSpec]) -> Vec<String> {
        links
            .iter()
            .map(|l| {
                format!(
                    "{}:{} -> {}:{}",
                    l.out_node, l.out_port, l.in_node, l.in_port
                )
            })
            .collect()
    }

    #[test]
    fn a_stereo_chain_wires_source_stages_and_sink() {
        let e = engine(
            vec![chain(2, &["pw_hpf", "pw_comp"])],
            vec![
                device("mic", Dir::Out, &["FL", "FR"]),
                device("spk", Dir::In, &["FL", "FR"]),
            ],
        );
        assert_eq!(
            show(&e.desired_links()),
            [
                "mic:p_FL -> pwfx.voice.hpf:in_1",
                "mic:p_FR -> pwfx.voice.hpf:in_2",
                "pwfx.voice.comp:out_1 -> spk:p_FL",
                "pwfx.voice.comp:out_2 -> spk:p_FR",
                "pwfx.voice.hpf:out_1 -> pwfx.voice.comp:in_1",
                "pwfx.voice.hpf:out_2 -> pwfx.voice.comp:in_2",
            ]
        );
    }

    #[test]
    fn a_mono_source_feeding_a_stereo_chain_fans_out() {
        let e = engine(
            vec![chain(2, &["pw_hpf"])],
            vec![
                device("mic", Dir::Out, &["MONO"]),
                device("spk", Dir::In, &["FL", "FR"]),
            ],
        );
        let l = show(&e.desired_links());
        assert!(l.contains(&"mic:p_MONO -> pwfx.voice.hpf:in_1".to_string()));
        assert!(l.contains(&"mic:p_MONO -> pwfx.voice.hpf:in_2".to_string()));
    }

    #[test]
    fn a_stereo_source_feeding_a_mono_chain_sums_into_one_port() {
        let e = engine(
            vec![chain(1, &["pw_hpf"])],
            vec![
                device("mic", Dir::Out, &["FL", "FR"]),
                device("spk", Dir::In, &["FL", "FR"]),
            ],
        );
        let l = show(&e.desired_links());
        assert!(l.contains(&"mic:p_FL -> pwfx.voice.hpf:in_1".to_string()));
        assert!(l.contains(&"mic:p_FR -> pwfx.voice.hpf:in_1".to_string()));
        // and the one output fans back out to both speakers
        assert!(l.contains(&"pwfx.voice.hpf:out_1 -> spk:p_FL".to_string()));
        assert!(l.contains(&"pwfx.voice.hpf:out_1 -> spk:p_FR".to_string()));
    }

    #[test]
    fn a_chain_with_no_stages_is_a_patch_cable() {
        let e = engine(
            vec![chain(2, &[])],
            vec![
                device("mic", Dir::Out, &["FL", "FR"]),
                device("spk", Dir::In, &["FL", "FR"]),
            ],
        );
        assert_eq!(
            show(&e.desired_links()),
            ["mic:p_FL -> spk:p_FL", "mic:p_FR -> spk:p_FR"]
        );
    }

    #[test]
    fn a_disabled_chain_asks_for_nothing() {
        let mut c = chain(2, &["pw_hpf"]);
        c.enabled = false;
        let e = engine(
            vec![c],
            vec![
                device("mic", Dir::Out, &["FL", "FR"]),
                device("spk", Dir::In, &["FL", "FR"]),
            ],
        );
        assert!(e.desired_links().is_empty());
    }

    #[test]
    fn an_absent_device_drops_only_its_own_end() {
        // The sink is unplugged: the chain still takes its input and runs.
        let e = engine(
            vec![chain(2, &["pw_hpf", "pw_comp"])],
            vec![device("mic", Dir::Out, &["FL", "FR"])],
        );
        let l = show(&e.desired_links());
        assert!(l.iter().any(|s| s.starts_with("mic:")));
        assert!(!l.iter().any(|s| s.contains("spk")));
    }

    #[test]
    fn an_uninstalled_filter_does_not_break_the_chain_around_it() {
        let mut c = chain(2, &["pw_hpf"]);
        c.stages.insert(
            1,
            Stage {
                id: "ghost".into(),
                plugin: "pw_nonexistent".into(),
                bypass: false,
                params: BTreeMap::new(),
            },
        );
        let e = engine(
            vec![c],
            vec![
                device("mic", Dir::Out, &["FL", "FR"]),
                device("spk", Dir::In, &["FL", "FR"]),
            ],
        );
        let l = show(&e.desired_links());
        assert!(l.contains(&"pwfx.voice.hpf:out_1 -> spk:p_FL".to_string()));
        assert!(!l.iter().any(|s| s.contains("ghost")));
    }

    #[test]
    fn values_the_filter_reprints_differently_are_not_resent() {
        assert!(same_value("80", "80.0"));
        assert!(same_value("-1", "-1.000"));
        assert!(!same_value("80", "81"));
        assert!(same_value("orfanidis", "orfanidis"));
        assert!(!same_value("max", "avg"));
    }
}

/// The other half of the save/restore contract: what a saved chain asks for
/// is what the filter ends up running with.
///
/// `plugin::against_the_real_filters` pins the parameter *table* read out of
/// `--help`; this pins the round trip through an actual process -- the
/// command line the engine builds, the filter's own parse and validate, and
/// the dump that comes back. It needs a PipeWire daemon, because a filter
/// that cannot connect exits, and skips when there is not one.
#[cfg(test)]
mod round_trip {
    use super::*;
    use crate::state::{AppState, Stage};
    use std::sync::mpsc;

    fn settings() -> BTreeMap<String, String> {
        // Every shape pw_eq has: frequencies, signed gains, unitless slopes
        // and Qs, and the one enum in the whole tree.
        [
            ("ls-freq", "90"), ("ls-gain", "-4"), ("ls-slope", "0.8"),
            ("b2-freq", "300"), ("b2-gain", "-3"), ("b2-q", "1.4"),
            ("b3-freq", "2500"), ("b3-gain", "3"), ("b3-q", "0.9"),
            ("b4-freq", "6000"), ("b4-gain", "-2"), ("b4-q", "2"),
            ("hs-freq", "9000"), ("hs-gain", "2.5"), ("hs-slope", "0.6"),
            ("bells", "rbj"),
        ]
        .iter()
        .map(|(k, v)| (k.to_string(), v.to_string()))
        .collect()
    }

    #[test]
    fn a_saved_eq_comes_back_exactly() {
        let (plugins, _) = crate::plugin::discover(&crate::plugin::search_path());
        if !plugins.iter().any(|p| p.name == "pw_eq") {
            eprintln!("no pw_eq built; skipping");
            return;
        }

        let (tx, rx) = mpsc::channel();
        let sink: crate::process::EventSink = Arc::new(move |e| {
            let _ = tx.send(e);
        });

        let wanted = settings();
        let mut chain = Chain {
            id: "e".into(),
            name: "E".into(),
            enabled: true,
            width: 2,
            stages: Vec::new(),
            input: None,
            output: None,
        };
        chain.stages.push(Stage {
            id: "eq".into(),
            plugin: "pw_eq".into(),
            bypass: false,
            params: wanted.clone(),
        });
        let node = chain.node_name(&chain.stages[0]);

        let mut e = Engine::new(
            plugins,
            AppState {
                chains: vec![chain],
                ..Default::default()
            },
            sink,
        );
        e.apply();

        // Wait for the filter's startup dump, or give up and skip.
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline {
            while let Ok(ev) = rx.try_recv() {
                e.on_proc_event(ev);
            }
            if e.runtime(&node).is_some_and(|r| r.reported.contains_key("bells")) {
                break;
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        let Some(rt) = e.runtime(&node).cloned() else {
            e.shutdown();
            eprintln!("pw_eq never reported; skipping (no PipeWire daemon?)");
            return;
        };
        if rt.reported.is_empty() {
            e.shutdown();
            eprintln!("pw_eq never reported; skipping (no PipeWire daemon?)");
            return;
        }

        for (name, value) in &wanted {
            assert_eq!(
                rt.reported.get(name).map(String::as_str),
                Some(value.as_str()),
                "{name}: the filter did not take what the saved chain asked for"
            );
        }

        // Adopting the filter's own report must leave a correct state alone,
        // and a save/load cycle must not move it either.
        let before = e.state.clone();
        e.absorb_reports();
        assert_eq!(e.state, before, "absorb_reports changed a settled state");

        let json = serde_json::to_string(&e.state).unwrap();
        let back: AppState = serde_json::from_str(&json).unwrap();
        assert_eq!(back, e.state, "the state did not survive JSON");

        e.shutdown();
    }
}
