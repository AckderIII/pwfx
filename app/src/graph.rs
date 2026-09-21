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
    /// display order: real hardware first, then virtual devices, then the
    /// monitor taps, then applications.
    pub fn kind(&self, dir: Dir) -> NodeKind {
        let c = self.media_class.as_str();
        if c.starts_with("Stream/") {
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
    Device,
    Virtual,
    Monitor,
    Application,
    Other,
}

impl NodeKind {
    pub fn label(self) -> &'static str {
        match self {
            NodeKind::Device => "Devices",
            NodeKind::Virtual => "Virtual devices",
            NodeKind::Monitor => "Monitors",
            NodeKind::Application => "Applications",
            NodeKind::Other => "Other",
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Graph {
    pub nodes: Vec<Node>,
    pub connected: bool,
}

impl Graph {
    pub fn node(&self, name: &str) -> Option<&Node> {
        self.nodes.iter().find(|n| n.name == name)
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
