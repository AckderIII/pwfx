//! The PipeWire graph as the GUI needs to see it, plus the channel matching.
//!
//! This is a snapshot type, not a live object: the PipeWire thread rebuilds
//! one whenever the registry changes and hands it over. Everything here comes
//! straight from the registry's global properties -- `node.name`,
//! `media.class`, `port.direction`, `audio.channel` -- so no proxies have to
//! be bound and no round trip is needed to read it.

use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Dir {
    In,
    Out,
}

#[derive(Clone, Debug)]
pub struct Port {
    pub id: u32,
    pub name: String,
    /// `audio.channel`: FL, FR, MONO, AUX0 ... Absent on ports that do not
    /// declare one, which is most MIDI and some filter ports.
    pub channel: Option<String>,
    pub dir: Dir,
    /// `port.id`, the ordinal within the node and direction. Ports arrive in
    /// registry order, which is not necessarily this one.
    pub index: u32,
}

#[derive(Clone, Debug)]
pub struct Node {
    pub id: u32,
    pub name: String,
    pub description: String,
    pub media_class: String,
    pub ports: Vec<Port>,
}

impl Node {
    pub fn ports_in(&self, dir: Dir) -> impl Iterator<Item = &Port> {
        self.ports.iter().filter(move |p| p.dir == dir)
    }

    /// Output ports, in `port.id` order -- the order the device declares its
    /// channels in, which is the one a by-index mapping has to follow.
    pub fn outputs(&self) -> Vec<&Port> {
        let mut v: Vec<&Port> = self.ports_in(Dir::Out).collect();
        v.sort_by_key(|p| (p.index, p.id));
        v
    }

    pub fn inputs(&self) -> Vec<&Port> {
        let mut v: Vec<&Port> = self.ports_in(Dir::In).collect();
        v.sort_by_key(|p| (p.index, p.id));
        v
    }

    pub fn ports_for(&self, dir: Dir) -> Vec<&Port> {
        match dir {
            Dir::Out => self.outputs(),
            Dir::In => self.inputs(),
        }
    }

    /// Where this node belongs in the endpoint picker. Ordering is the
    /// display order: the app's own buses first, then real hardware, then
    /// other virtual devices, then the monitor taps, then applications.
    pub fn kind(&self, dir: Dir) -> NodeKind {
        let c = self.media_class.as_str();
        if self.name.starts_with(crate::state::BUS_PREFIX) {
            // A bus is a virtual device and a monitor tap and would be
            // classified as either, depending on which end is being asked
            // about. It is neither: it is the thing the user just made, and
            // it belongs at the top of both lists.
            NodeKind::Bus
        } else if c.starts_with("Stream/") {
            NodeKind::Application
        } else if c.contains("Virtual") {
            NodeKind::Virtual
        } else if dir == Dir::Out && c == "Audio/Sink" {
            // A sink's output ports are its monitor: what is being played.
            NodeKind::Monitor
        } else if c.starts_with("Audio/") {
            NodeKind::Device
        } else {
            NodeKind::Other
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum NodeKind {
    Bus,
    Device,
    Virtual,
    Monitor,
    Application,
    Other,
}

impl NodeKind {
    pub fn label(self) -> &'static str {
        match self {
            NodeKind::Bus => "Buses",
            NodeKind::Device => "Devices",
            NodeKind::Virtual => "Virtual devices",
            NodeKind::Monitor => "Monitors",
            NodeKind::Application => "Applications",
            NodeKind::Other => "Other",
        }
    }
}

/// What a chain end saved as "the default input" is called. Not a node name
/// anyone can have -- `node.name` never starts with `@` -- so it can sit in
/// the same field as one and be told apart from it.
pub const DEFAULT_SOURCE: &str = "@default-source";
/// The same for "the default output".
pub const DEFAULT_SINK: &str = "@default-sink";

#[derive(Clone, Debug, Default)]
pub struct Graph {
    pub nodes: Vec<Node>,
    pub connected: bool,
    /// `node.name` of what the session manager currently routes to by
    /// default, from the `default` metadata: `default.audio.source` and
    /// `default.audio.sink`. The effective defaults rather than the
    /// configured ones, so an unplugged favourite gives way to whatever
    /// the session manager fell back to.
    pub default_source: Option<String>,
    pub default_sink: Option<String>,
}

impl Graph {
    pub fn node(&self, name: &str) -> Option<&Node> {
        self.nodes.iter().find(|n| n.name == name)
    }

    /// The node an endpoint name stands for right now.
    ///
    /// A concrete name is itself; a default is whatever the session manager
    /// names at the moment, so a chain on "the default output" moves to the
    /// headphones when they become the default, with nothing stored anywhere
    /// changing. That is the same re-resolve-by-name every link already gets,
    /// one level further up.
    pub fn lookup(&self, name: &str) -> Option<&Node> {
        self.node(self.resolve_name(name)?)
    }

    /// The concrete `node.name` behind an endpoint name, if it may be used.
    ///
    /// A default that names one of this app's own nodes is not followed.
    /// Making a source bus the system microphone is the normal way to use
    /// one, and a chain whose input is "the default microphone" would then
    /// be fed from its own output. Not following it drops that end like an
    /// unplugged device, which is a silence the user can see the reason for
    /// rather than a feedback loop.
    pub fn resolve_name<'a>(&'a self, name: &'a str) -> Option<&'a str> {
        let target = match self.default_for(name) {
            Some(target) => target?,
            None => return Some(name),
        };
        (!target.starts_with(crate::state::PREFIX)).then_some(target)
    }

    /// `None` for a concrete name; `Some(current target)` for a default.
    pub fn default_for(&self, name: &str) -> Option<Option<&str>> {
        match name {
            DEFAULT_SOURCE => Some(self.default_source.as_deref()),
            DEFAULT_SINK => Some(self.default_sink.as_deref()),
            _ => None,
        }
    }

    /// Nodes that can act as an endpoint in the given direction, grouped and
    /// sorted for the picker. `dir` is the direction of the *ports we would
    /// use*: `Out` for something to take audio from, `In` for something to
    /// send it to.
    pub fn endpoints(&self, dir: Dir, exclude: impl Fn(&Node) -> bool) -> Vec<&Node> {
        let mut v: Vec<&Node> = self
            .nodes
            .iter()
            .filter(|n| n.ports_in(dir).next().is_some())
            .filter(|n| !exclude(n))
            .collect();
        v.sort_by(|a, b| {
            a.kind(dir)
                .cmp(&b.kind(dir))
                .then_with(|| a.description.to_lowercase().cmp(&b.description.to_lowercase()))
        });
        v
    }
}

// ---------------------------------------------------------------------------
// channel matching

/// Channel names a chain of the given width uses, matching the names
/// `src/pwf.c` gives the filter ports so that a by-name match lines up.
pub fn chain_channels(width: usize) -> Vec<String> {
    const NAMES: [&str; 8] = ["FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"];
    if width == 1 {
        return vec!["MONO".into()];
    }
    (0..width)
        .map(|i| NAMES.get(i).map(|s| s.to_string()).unwrap_or(format!("CH{}", i + 1)))
        .collect()
}

/// `audio.position` for a bus of the given width.
///
/// Nearly the same list as `chain_channels`, and deliberately so -- a bus and
/// a chain of the same width then match by name and need no mapping at all --
/// but it has to be names SPA actually knows. Past the eight surround
/// positions there is no such name, so the channels become `AUX0..` rather
/// than the `CH9` a chain would call them; an adapter asked for a position it
/// cannot parse comes up with unnamed ports instead of failing, which is the
/// kind of wrong that is only noticed much later.
pub fn bus_channels(width: usize) -> Vec<String> {
    if width > 8 {
        return (0..width).map(|i| format!("AUX{i}")).collect();
    }
    chain_channels(width)
}

/// Work out which source channel feeds which destination channel.
///
/// Returns `(src index, dst index)` pairs. Several sources landing on one
/// destination is not a mistake: PipeWire sums everything linked into an
/// input port, so that is how a stereo source folds down to a mono chain.
///
/// Two rules, tried in order:
///
/// 1. **By name.** If every destination channel has a same-named source
///    channel, use those. This is what makes FL/FR line up with FL/FR even
///    when the device declares them in a different order, and what lets a
///    5.1 source feed a stereo chain with its front pair rather than its
///    first two ports.
/// 2. **By index, wrapping.** Otherwise walk `0..max(src, dst)` and pair
///    `i % src.len()` with `i % dst.len()`. Wrapping on both sides is what
///    makes the two interesting cases fall out of one rule: mono into stereo
///    fans the one port to both, and stereo into mono sums both into the one.
pub fn auto_map(src: &[Option<String>], dst: &[Option<String>]) -> Vec<(usize, usize)> {
    if src.is_empty() || dst.is_empty() {
        return Vec::new();
    }

    let named: Option<Vec<(usize, usize)>> = dst
        .iter()
        .enumerate()
        .map(|(di, name)| {
            let name = name.as_deref()?;
            let si = src.iter().position(|s| {
                s.as_deref()
                    .is_some_and(|s| s.eq_ignore_ascii_case(name))
            })?;
            Some((si, di))
        })
        .collect();
    if let Some(pairs) = named {
        return pairs;
    }

    let n = src.len().max(dst.len());
    let mut pairs: Vec<(usize, usize)> = (0..n).map(|i| (i % src.len(), i % dst.len())).collect();
    pairs.sort_unstable();
    pairs.dedup();
    pairs
}

/// How a chain's end is wired to the node it is connected to.
#[derive(Clone, Debug, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Mapping {
    /// Worked out by `auto_map` every time, so it follows the device.
    #[default]
    Auto,
    /// Explicit `(port name, chain channel)` pairs, kept by name so they
    /// survive the node going away and coming back with new ids.
    Manual(Vec<(String, usize)>),
}

/// Resolve a mapping into the port pairs to link, as
/// `(node port name, chain channel index)`.
pub fn resolve(
    mapping: &Mapping,
    node: &Node,
    dir: Dir,
    chain_width: usize,
    chain_is_dst: bool,
) -> Vec<(String, usize)> {
    let ports = node.ports_for(dir);
    match mapping {
        Mapping::Manual(pairs) => pairs
            .iter()
            .filter(|(name, ch)| *ch < chain_width && ports.iter().any(|p| p.name == *name))
            .cloned()
            .collect(),
        Mapping::Auto => {
            let node_ch: Vec<Option<String>> = ports.iter().map(|p| p.channel.clone()).collect();
            let chain_ch: Vec<Option<String>> =
                chain_channels(chain_width).into_iter().map(Some).collect();
            let pairs = if chain_is_dst {
                auto_map(&node_ch, &chain_ch)
            } else {
                // Chain is the source: swap so the pairs stay (node, chain).
                auto_map(&chain_ch, &node_ch)
                    .into_iter()
                    .map(|(c, n)| (n, c))
                    .collect()
            };
            pairs
                .into_iter()
                .filter_map(|(ni, ci)| Some((ports.get(ni)?.name.clone(), ci)))
                .collect()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn names(v: &[&str]) -> Vec<Option<String>> {
        v.iter().map(|s| Some(s.to_string())).collect()
    }

    #[test]
    fn matching_names_win_over_order() {
        // A device that declares FR before FL still lines up.
        let src = names(&["FR", "FL"]);
        let dst = names(&["FL", "FR"]);
        assert_eq!(auto_map(&src, &dst), [(1, 0), (0, 1)]);
    }

    #[test]
    fn mono_fans_out_and_stereo_folds_down() {
        assert_eq!(auto_map(&names(&["MONO"]), &names(&["FL", "FR"])), [(0, 0), (0, 1)]);
        // Both source ports land on the one destination, which sums them.
        assert_eq!(auto_map(&names(&["FL", "FR"]), &names(&["MONO"])), [(0, 0), (1, 0)]);
    }

    #[test]
    fn a_surround_source_gives_a_stereo_chain_its_front_pair() {
        let src = names(&["FL", "FR", "FC", "LFE", "RL", "RR"]);
        assert_eq!(auto_map(&src, &names(&["FL", "FR"])), [(0, 0), (1, 1)]);
    }

    #[test]
    fn unnameable_channels_fall_back_to_position() {
        // AUX0/AUX1 is what a split device gives; nothing matches by name.
        assert_eq!(auto_map(&names(&["AUX0", "AUX1"]), &names(&["FL", "FR"])), [(0, 0), (1, 1)]);
        // Three into two wraps rather than dropping the third.
        assert_eq!(
            auto_map(&names(&["AUX0", "AUX1", "AUX2"]), &names(&["FL", "FR"])),
            [(0, 0), (1, 1), (2, 0)]
        );
    }

    #[test]
    fn nothing_on_one_side_links_nothing() {
        assert!(auto_map(&[], &names(&["FL"])).is_empty());
        assert!(auto_map(&names(&["FL"]), &[]).is_empty());
    }

    #[test]
    fn chain_channel_names_match_the_filters() {
        assert_eq!(chain_channels(1), ["MONO"]);
        assert_eq!(chain_channels(2), ["FL", "FR"]);
        assert_eq!(chain_channels(6), ["FL", "FR", "FC", "LFE", "RL", "RR"]);
        assert_eq!(chain_channels(9)[8], "CH9");
    }

    fn node(ports: &[(&str, &str, Dir)]) -> Node {
        Node {
            id: 1,
            name: "dev".into(),
            description: "dev".into(),
            media_class: "Audio/Source".into(),
            ports: ports
                .iter()
                .enumerate()
                .map(|(i, (n, c, d))| Port {
                    id: 100 + i as u32,
                    name: n.to_string(),
                    channel: Some(c.to_string()),
                    dir: *d,
                    index: i as u32,
                })
                .collect(),
        }
    }

    #[test]
    fn a_bus_declares_channel_names_spa_knows() {
        assert_eq!(bus_channels(1), ["MONO"]);
        assert_eq!(bus_channels(2), ["FL", "FR"]);
        assert_eq!(bus_channels(8), chain_channels(8));
        // Past the surround positions a chain says CH9; SPA has no such
        // name, so a bus says AUX8.
        assert_eq!(chain_channels(9)[8], "CH9");
        assert_eq!(bus_channels(9)[8], "AUX8");
        assert_eq!(bus_channels(9)[0], "AUX0");
    }

    #[test]
    fn a_bus_is_sorted_and_labelled_as_itself_at_both_ends() {
        let mut n = node(&[
            ("playback_FL", "FL", Dir::In),
            ("monitor_FL", "FL", Dir::Out),
        ]);
        n.name = "pwfx.bus.mix".into();
        n.media_class = "Audio/Sink".into();
        // Without the prefix this is a device at one end and a monitor tap
        // at the other; with it, it is a bus at both.
        assert_eq!(n.kind(Dir::In), NodeKind::Bus);
        assert_eq!(n.kind(Dir::Out), NodeKind::Bus);
        assert!(NodeKind::Bus < NodeKind::Device);

        // A filter stage is not a bus, however close the prefix looks.
        n.name = "pwfx.voice.hpf".into();
        assert_ne!(n.kind(Dir::In), NodeKind::Bus);
    }

    fn with_defaults(source: &str, sink: &str) -> Graph {
        let mut mic = node(&[("capture_MONO", "MONO", Dir::Out)]);
        mic.name = "mic".into();
        let mut spk = node(&[("playback_FL", "FL", Dir::In)]);
        spk.name = "spk".into();
        Graph {
            nodes: vec![mic, spk],
            connected: true,
            default_source: Some(source.into()),
            default_sink: Some(sink.into()),
        }
    }

    #[test]
    fn a_default_resolves_to_whatever_is_the_default_now() {
        let mut g = with_defaults("mic", "spk");
        assert_eq!(g.lookup(DEFAULT_SOURCE).map(|n| n.name.as_str()), Some("mic"));
        assert_eq!(g.lookup(DEFAULT_SINK).map(|n| n.name.as_str()), Some("spk"));
        // A concrete name is untouched.
        assert_eq!(g.resolve_name("spk"), Some("spk"));

        // The default moves to something not in the graph: nothing, the
        // same as an unplugged device.
        g.default_sink = Some("headphones".into());
        assert!(g.lookup(DEFAULT_SINK).is_none());
        g.default_sink = None;
        assert!(g.lookup(DEFAULT_SINK).is_none());
    }

    #[test]
    fn a_default_that_is_one_of_our_own_nodes_is_not_followed() {
        // The source bus made the system microphone: following it would
        // feed a chain from its own output.
        let g = with_defaults("pwfx.bus.mic", "pwfx.voice.hpf");
        assert_eq!(g.resolve_name(DEFAULT_SOURCE), None);
        assert_eq!(g.resolve_name(DEFAULT_SINK), None);
        assert_eq!(g.default_for(DEFAULT_SOURCE), Some(Some("pwfx.bus.mic")));
        // Picking the bus by name is still allowed; only the default is
        // second-guessed.
        assert_eq!(g.resolve_name("pwfx.bus.mic"), Some("pwfx.bus.mic"));
    }

    #[test]
    fn resolving_auto_uses_the_live_port_names() {
        let n = node(&[("capture_FL", "FL", Dir::Out), ("capture_FR", "FR", Dir::Out)]);
        let got = resolve(&Mapping::Auto, &n, Dir::Out, 2, true);
        assert_eq!(got, [("capture_FL".into(), 0), ("capture_FR".into(), 1)]);
    }

    #[test]
    fn resolving_manual_drops_pairs_the_device_no_longer_has() {
        let n = node(&[("capture_FL", "FL", Dir::Out)]);
        let m = Mapping::Manual(vec![
            ("capture_FL".into(), 0),
            ("capture_FR".into(), 1), // gone: the device changed profile
            ("capture_FL".into(), 7), // out of range for a stereo chain
        ]);
        assert_eq!(resolve(&m, &n, Dir::Out, 2, true), [("capture_FL".into(), 0)]);
    }
}
