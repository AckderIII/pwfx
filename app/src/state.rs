//! What the app remembers: the chains, the buses, and where they are plugged
//! in.
//!
//! This is the whole user-visible model and the whole save file. Everything
//! else in the program is derived from it -- which processes run, which nodes
//! the app asks the daemon for, which links exist, what the window shows --
//! so saving and restoring is just writing this out and reading it back, and
//! there is no second place a setting could hide.
//!
//! Endpoints are stored by `node.name` rather than by id because ids are
//! reassigned every time a device is replugged or a profile changes, whereas
//! the name of a card's sink is the same next week. A saved chain therefore
//! reconnects itself when the interface is plugged back in, without the app
//! having to do anything about it: the name resolves again and the links come
//! back.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::graph::Mapping;

pub const FORMAT_VERSION: u32 = 1;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Endpoint {
    /// `node.name` of the device, application or virtual node.
    pub node: String,
    /// Its description at the time it was chosen, so the UI can still name
    /// something that is currently unplugged.
    #[serde(default)]
    pub label: String,
    #[serde(default)]
    pub mapping: Mapping,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Stage {
    pub id: String,
    /// The program name, `pw_lpf`. A saved chain naming a filter that is not
    /// installed is kept and reported rather than silently dropped.
    pub plugin: String,
    #[serde(default)]
    pub bypass: bool,
    /// Live parameters only, as the text the filter echoed back.
    #[serde(default)]
    pub params: BTreeMap<String, String>,
    /// What the filter's sidechain input, `sc_in`, listens to -- for a
    /// filter that has one (`PluginDesc::sidechain`). Any source a chain's
    /// input could take, stored the same way, by name; a wider one is
    /// folded down onto the one mono port as a chain folds onto one
    /// channel. Kept when the filter is not installed, like `params`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sidechain: Option<Endpoint>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Chain {
    pub id: String,
    pub name: String,
    #[serde(default = "yes")]
    pub enabled: bool,
    /// How many channels run through the chain. Every stage is built this
    /// wide; the mapping at each end is what copes with the world not being.
    pub width: usize,
    #[serde(default)]
    pub stages: Vec<Stage>,
    #[serde(default)]
    pub input: Option<Endpoint>,
    #[serde(default)]
    pub output: Option<Endpoint>,
}

/// What a bus is to everything outside this app.
///
/// Both kinds are the same object underneath -- a null audio sink, whose
/// input ports turn up again on its monitor -- so both have ports at each
/// end and either can be picked at either end of a chain. What `media.class`
/// decides is which side the session manager offers to *other* applications,
/// and that is the whole difference between them.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum BusKind {
    /// A virtual microphone. Chains feed its input; other applications
    /// record from it.
    #[default]
    Source,
    /// A virtual speaker. Other applications play into it; chains take the
    /// audio off its monitor.
    Sink,
}

impl BusKind {
    pub const ALL: [BusKind; 2] = [BusKind::Source, BusKind::Sink];

    pub fn media_class(self) -> &'static str {
        match self {
            // `/Virtual` is what keeps it out of the list of real capture
            // devices while still offering it as one to record from.
            BusKind::Source => "Audio/Source/Virtual",
            BusKind::Sink => "Audio/Sink",
        }
    }

    /// Whether the node is allowed to drive the graph.
    ///
    /// A source bus never is: something always feeds it -- a chain, and
    /// behind that a real capture device -- and that is what should be
    /// setting the clock. A sink bus is the other way round: an application
    /// playing into it has no clock of its own, so the bus has to be the one
    /// providing it.
    pub fn passive(self) -> bool {
        self == BusKind::Source
    }

    pub fn label(self) -> &'static str {
        match self {
            BusKind::Source => "Source — applications record from it",
            BusKind::Sink => "Sink — applications play into it",
        }
    }

    /// The sentence shown under the picker, in the bus's own terms.
    pub fn explain(self) -> &'static str {
        match self {
            BusKind::Source => "Chains send audio into this bus. \
                Other applications see it as a microphone and record from it.",
            BusKind::Sink => "Other applications see this bus as an output and play into it. \
                Chains take the audio back off it.",
        }
    }
}

/// A virtual device the app asks the daemon to create for it.
///
/// It is not a process and it is not a filter: it is one node with N inputs
/// and N outputs, so several chains can feed one bus and several chains can
/// take from it. What that is *for* is giving third-party applications a
/// fixed name to record from or play into, whatever the chains behind it are
/// doing this week.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Bus {
    pub id: String,
    pub name: String,
    #[serde(default = "yes")]
    pub enabled: bool,
    pub width: usize,
    #[serde(default)]
    pub kind: BusKind,
    /// Applications routed to this bus, by `application.name`: their
    /// playback streams into a sink bus, their recording streams out of a
    /// source bus. The kind decides which, because that is the only
    /// direction the session manager will link a stream to it in.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub apps: Vec<String>,
}

impl Bus {
    /// The `node.name` the bus appears in the graph under.
    ///
    /// Built from the id, never from the display name, for the same reason a
    /// stage's is: it is what every chain's endpoint and every hand-made
    /// `pw-link` refers to, and renaming a bus must not break them.
    pub fn node_name(&self) -> String {
        format!("{BUS_PREFIX}{}", self.id)
    }

    /// What other applications put in their device list. The name is the
    /// whole point of a bus, so unlike the node name this one does follow
    /// what the user typed.
    pub fn description(&self) -> String {
        match self.name.trim() {
            "" => self.node_name(),
            n => n.to_string(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct AppState {
    pub version: u32,
    #[serde(default)]
    pub chains: Vec<Chain>,
    #[serde(default)]
    pub buses: Vec<Bus>,
    /// Metering costs a line pair per stage per interval on stdout and a scan
    /// per block in the filter, so it is switchable.
    #[serde(default = "yes")]
    pub meters: bool,
}

fn yes() -> bool {
    true
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            version: FORMAT_VERSION,
            chains: Vec::new(),
            buses: Vec::new(),
            meters: true,
        }
    }
}

impl AppState {
    /// A chain id that is not in use, derived from the name so that node
    /// names in the graph are recognisable.
    ///
    /// `bus` is never handed out, because a chain by that name would put its
    /// stages at `pwfx.bus.<stage>` -- exactly where the buses live.
    pub fn fresh_chain_id(&self, name: &str) -> String {
        unique(&slug(name), |c| {
            c == BUS_SEGMENT || self.chains.iter().any(|x| x.id == c)
        })
    }

    pub fn fresh_bus_id(&self, name: &str) -> String {
        unique(&slug(name), |c| self.buses.iter().any(|x| x.id == c))
    }

    pub fn bus(&self, node: &str) -> Option<&Bus> {
        self.buses.iter().find(|b| b.node_name() == node)
    }
}

impl Chain {
    pub fn fresh_stage_id(&self, plugin: &str) -> String {
        let base = slug(plugin.strip_prefix("pw_").unwrap_or(plugin));
        unique(&base, |c| self.stages.iter().any(|s| s.id == c))
    }

    /// The `node.name` the stage's process runs under.
    pub fn node_name(&self, stage: &Stage) -> String {
        node_name(&self.id, &stage.id)
    }
}

/// The `node.name` a stage's process runs under, and the one prefix the app
/// recognises as its own.
///
/// Built from the two ids and never from a display name or a position, so
/// that renaming a chain does not restart it and reordering one moves links
/// around rather than renaming -- and therefore restarting -- every process
/// after the one that moved.
pub fn node_name(chain_id: &str, stage_id: &str) -> String {
    format!("{PREFIX}{chain_id}.{stage_id}")
}

/// What marks a node in the graph as belonging to this app.
pub const PREFIX: &str = "pwfx.";

/// And what marks one of those as a bus rather than a filter stage. Buses
/// are the app's own nodes but they are also endpoints, so this is the line
/// the pickers draw: stages are hidden, buses are offered.
pub const BUS_PREFIX: &str = "pwfx.bus.";

const BUS_SEGMENT: &str = "bus";

/// Whether a node in the graph is one of this app's filter stages.
///
/// That is the one class of node the endpoint pickers hide: a chain fed from
/// another chain's stage would be wiring round the app's back. A bus carries
/// the same prefix and is the exact opposite -- it exists to be picked, at
/// either end, by this app and by everything else on the machine.
pub fn is_stage_node(name: &str) -> bool {
    name.starts_with(PREFIX) && !name.starts_with(BUS_PREFIX)
}

/// Node names travel through PipeWire properties and end up in `pw-link`
/// output, so keep them to something that can be typed.
pub fn slug(s: &str) -> String {
    let out: String = s
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() {
                c.to_ascii_lowercase()
            } else {
                '-'
            }
        })
        .collect();
    let out = out.trim_matches('-').to_string();
    let mut collapsed = String::with_capacity(out.len());
    let mut dash = false;
    for c in out.chars() {
        if c == '-' {
            if !dash {
                collapsed.push(c);
            }
            dash = true;
        } else {
            collapsed.push(c);
            dash = false;
        }
    }
    if collapsed.is_empty() {
        "x".into()
    } else {
        collapsed
    }
}

fn unique(base: &str, taken: impl Fn(&str) -> bool) -> String {
    if !taken(base) {
        return base.to_string();
    }
    (2..).map(|n| format!("{base}-{n}")).find(|c| !taken(c)).unwrap()
}

// ---------------------------------------------------------------------------
// on disk

/// Where the save files live: `$XDG_CONFIG_HOME/pwfx`, or `~/.config/pwfx`.
pub fn config_dir() -> PathBuf {
    if let Ok(d) = std::env::var("PWFX_CONFIG_DIR") {
        return PathBuf::from(d);
    }
    if let Ok(d) = std::env::var("XDG_CONFIG_HOME") {
        if !d.is_empty() {
            return PathBuf::from(d).join("pwfx");
        }
    }
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    PathBuf::from(home).join(".config").join("pwfx")
}

/// The state the app was in when it was last closed.
pub fn session_path() -> PathBuf {
    config_dir().join("session.json")
}

/// The state "Restore default" goes back to, and what a first run starts from.
pub fn default_path() -> PathBuf {
    config_dir().join("default.json")
}

pub fn presets_dir() -> PathBuf {
    config_dir().join("presets")
}

pub fn preset_path(name: &str) -> PathBuf {
    presets_dir().join(format!("{}.json", slug(name)))
}

pub fn read(path: &Path) -> Result<AppState, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let state: AppState =
        serde_json::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))?;
    if state.version > FORMAT_VERSION {
        return Err(format!(
            "{}: saved by a newer version of pwfx (format {} > {FORMAT_VERSION})",
            path.display(),
            state.version
        ));
    }
    Ok(state)
}

/// Write through a temporary file, so an interrupted save cannot leave a
/// half-written session behind for the next start to choke on.
pub fn write(path: &Path, state: &AppState) -> Result<(), String> {
    let dir = path.parent().unwrap_or(Path::new("."));
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let text = serde_json::to_string_pretty(state).map_err(|e| e.to_string())?;

    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, text.as_bytes()).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
}

/// Hold the one-per-user lock for as long as the returned file is kept open.
///
/// Two copies of the app would ask for the same node names -- the same
/// `pwfx.voice.hpf`, the same `pwfx.bus.mic` -- and fight over them, so the
/// window and the headless mode both take this and the second one to start
/// refuses. It sits in `$XDG_RUNTIME_DIR` because what it guards is the
/// user's PipeWire session, not a config directory: two `PWFX_CONFIG_DIR`s
/// still share one graph. The kernel drops an `flock` with the process,
/// however it ends, so there is nothing stale to clean up after a crash.
/// Why the lock was not taken.
#[derive(Debug)]
pub enum LockError {
    /// Another pwfx has it; the pid it wrote, if it could be read.
    Held(Option<u32>),
    /// The lock file itself could not be made or opened.
    Io(String),
}

impl std::fmt::Display for LockError {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            LockError::Held(Some(pid)) => write!(f, "pwfx is already running (pid {pid})"),
            LockError::Held(None) => write!(f, "pwfx is already running"),
            LockError::Io(e) => f.write_str(e),
        }
    }
}

pub fn lock_instance() -> Result<std::fs::File, LockError> {
    let dir = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(config_dir);
    std::fs::create_dir_all(&dir).map_err(|e| LockError::Io(format!("{}: {e}", dir.display())))?;
    lock_at(&dir.join("pwfx.lock"))
}

fn lock_at(path: &Path) -> Result<std::fs::File, LockError> {
    use std::io::{Read, Seek, Write};
    use std::os::fd::AsRawFd;

    let mut file = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(path)
        .map_err(|e| LockError::Io(format!("{}: {e}", path.display())))?;

    if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        let mut pid = String::new();
        let _ = file.read_to_string(&mut pid);
        return Err(LockError::Held(pid.trim().parse().ok()));
    }
    // Only for the message above; the lock is the flock, not the contents.
    let _ = file.set_len(0);
    let _ = file.rewind();
    let _ = writeln!(file, "{}", std::process::id());
    Ok(file)
}

/// What to start with: the last session if there is one, otherwise the saved
/// default, otherwise nothing.
pub fn startup() -> (AppState, Option<String>) {
    for path in [session_path(), default_path()] {
        if !path.exists() {
            continue;
        }
        match read(&path) {
            Ok(s) => return (s, None),
            Err(e) => return (AppState::default(), Some(e)),
        }
    }
    (AppState::default(), None)
}

pub fn list_presets() -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(presets_dir())
        .into_iter()
        .flatten()
        .filter_map(|e| e.ok())
        .filter_map(|e| {
            let p = e.path();
            (p.extension()? == "json")
                .then(|| p.file_stem()?.to_str().map(|s| s.to_string()))
                .flatten()
        })
        .collect();
    v.sort();
    v
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_one_instance_holds_the_lock() {
        let path = std::env::temp_dir().join(format!("pwfx-lock-test-{}", std::process::id()));
        let first = lock_at(&path).expect("the first one gets it");
        // flock belongs to the open file, not the process, so a second open
        // in the same process is refused just as another process would be.
        let err = lock_at(&path).unwrap_err();
        assert!(matches!(err, LockError::Held(Some(pid)) if pid == std::process::id()), "{err}");
        drop(first);
        let again = lock_at(&path).expect("free again once the holder lets go");
        drop(again);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn slugs_are_safe_for_node_names() {
        assert_eq!(slug("Voice Chain"), "voice-chain");
        assert_eq!(slug("  Mic #1 !! "), "mic-1");
        assert_eq!(slug("Ünïcödé"), "n-c-d");
        assert_eq!(slug("---"), "x");
        assert_eq!(slug(""), "x");
    }

    #[test]
    fn ids_do_not_collide() {
        let mut st = AppState::default();
        for _ in 0..3 {
            let id = st.fresh_chain_id("Voice");
            st.chains.push(Chain {
                id,
                name: "Voice".into(),
                enabled: true,
                width: 2,
                stages: vec![],
                input: None,
                output: None,
            });
        }
        let ids: Vec<&str> = st.chains.iter().map(|c| c.id.as_str()).collect();
        assert_eq!(ids, ["voice", "voice-2", "voice-3"]);
    }

    #[test]
    fn bus_ids_do_not_collide_and_stay_out_of_the_chains_way() {
        let mut st = AppState::default();
        for _ in 0..2 {
            let id = st.fresh_bus_id("Mic Bus");
            st.buses.push(Bus {
                id,
                name: "Mic Bus".into(),
                enabled: true,
                width: 2,
                kind: BusKind::Source,
                apps: Vec::new(),
            });
        }
        let ids: Vec<&str> = st.buses.iter().map(|b| b.id.as_str()).collect();
        assert_eq!(ids, ["mic-bus", "mic-bus-2"]);
        assert_eq!(st.buses[0].node_name(), "pwfx.bus.mic-bus");
        assert_eq!(st.bus("pwfx.bus.mic-bus-2").map(|b| b.name.as_str()), Some("Mic Bus"));

        // A chain called "Bus" would otherwise name its stages into the same
        // space the buses live in.
        assert_eq!(st.fresh_chain_id("Bus"), "bus-2");
    }

    #[test]
    fn the_pickers_hide_stages_and_offer_buses() {
        assert!(is_stage_node("pwfx.voice.hpf"));
        assert!(is_stage_node("pwfx.bus-2.hpf")); // a chain called "Bus 2"
        assert!(!is_stage_node("pwfx.bus.mic"));
        assert!(!is_stage_node("alsa_output.pci-0000_03_00.1.hdmi-stereo"));
        assert!(!is_stage_node("MyMic"));
    }

    #[test]
    fn a_bus_keeps_its_node_name_when_it_is_renamed() {
        let mut b = Bus {
            id: "talk".into(),
            name: "Talkback".into(),
            enabled: true,
            width: 1,
            kind: BusKind::Source,
            apps: Vec::new(),
        };
        let node = b.node_name();
        b.name = "Studio talkback".into();
        assert_eq!(b.node_name(), node);
        // The description does follow, because that is what other
        // applications show in their device list.
        assert_eq!(b.description(), "Studio talkback");
        b.name = "  ".into();
        assert_eq!(b.description(), "pwfx.bus.talk");
    }

    #[test]
    fn node_names_follow_the_stage_not_its_position() {
        let mut c = Chain {
            id: "voice".into(),
            name: "Voice".into(),
            enabled: true,
            width: 2,
            stages: vec![],
            input: None,
            output: None,
        };
        for p in ["pw_hpf", "pw_comp", "pw_hpf"] {
            let id = c.fresh_stage_id(p);
            c.stages.push(Stage {
                id,
                plugin: p.into(),
                bypass: false,
                sidechain: None,
                params: BTreeMap::new(),
            });
        }
        let before: Vec<String> = c.stages.iter().map(|s| c.node_name(s)).collect();
        assert_eq!(before, ["pwfx.voice.hpf", "pwfx.voice.comp", "pwfx.voice.hpf-2"]);

        c.stages.swap(0, 2);
        let after: Vec<String> = c.stages.iter().map(|s| c.node_name(s)).collect();
        assert_eq!(after, ["pwfx.voice.hpf-2", "pwfx.voice.comp", "pwfx.voice.hpf"]);
    }

    #[test]
    fn a_round_trip_through_json_changes_nothing() {
        let st = AppState {
            version: FORMAT_VERSION,
            meters: false,
            chains: vec![Chain {
                id: "voice".into(),
                name: "Voice".into(),
                enabled: true,
                width: 2,
                stages: vec![Stage {
                    id: "hpf".into(),
                    plugin: "pw_hpf".into(),
                    bypass: true,
                    sidechain: None,
                    params: [("cutoff".to_string(), "80".to_string())].into(),
                }],
                input: Some(Endpoint {
                    node: "alsa_input.x".into(),
                    label: "Mic".into(),
                    mapping: Mapping::Manual(vec![("capture_FL".into(), 0)]),
                }),
                output: None,
            }],
            buses: vec![Bus {
                id: "mic".into(),
                name: "Mic Bus".into(),
                enabled: true,
                width: 2,
                kind: BusKind::Sink,
                apps: Vec::new(),
            }],
        };
        let text = serde_json::to_string(&st).unwrap();
        assert_eq!(serde_json::from_str::<AppState>(&text).unwrap(), st);
    }

    #[test]
    fn an_older_save_missing_fields_still_loads() {
        let text = r#"{"version":1,"chains":[{"id":"a","name":"A","width":2}]}"#;
        let st: AppState = serde_json::from_str(text).unwrap();
        assert!(st.meters);
        assert!(st.chains[0].enabled);
        assert!(st.chains[0].stages.is_empty());
        assert!(st.buses.is_empty());

        // And one written before the kind had a name defaults to the one a
        // bus is usually made for.
        let text = r#"{"version":1,"buses":[{"id":"b","name":"B","width":2}]}"#;
        let st: AppState = serde_json::from_str(text).unwrap();
        assert_eq!(st.buses[0].kind, BusKind::Source);
        assert!(st.buses[0].enabled);
    }
}
