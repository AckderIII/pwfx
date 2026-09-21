//! What the app remembers: the chains, and where they are plugged in.
//!
//! This is the whole user-visible model and the whole save file. Everything
//! else in the program is derived from it -- which processes run, which links
//! exist, what the window shows -- so saving and restoring is just writing
//! this out and reading it back, and there is no second place a setting could
//! hide.
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

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct AppState {
    pub version: u32,
    #[serde(default)]
    pub chains: Vec<Chain>,
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
            meters: true,
        }
    }
}

impl AppState {
    /// A chain id that is not in use, derived from the name so that node
    /// names in the graph are recognisable.
    pub fn fresh_chain_id(&self, name: &str) -> String {
        unique(&slug(name), |c| self.chains.iter().any(|x| x.id == c))
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
                    params: [("cutoff".to_string(), "80".to_string())].into(),
                }],
                input: Some(Endpoint {
                    node: "alsa_input.x".into(),
                    label: "Mic".into(),
                    mapping: Mapping::Manual(vec![("capture_FL".into(), 0)]),
                }),
                output: None,
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
    }
}
