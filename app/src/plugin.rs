//! Discovering the filter binaries and learning what knobs they have.
//!
//! A plugin here is just one of the `pw_*` programs: it is asked for its
//! `--help` and the reply is parsed back into a parameter table. That text is
//! not prose, it is printed by `print_param_help()` in `src/pwf.c` from the
//! same table the program validates against, so it has exactly one shape:
//!
//! ```text
//!   -f, --cutoff HZ            cutoff frequency, the -3 dB point
//!                              (1..384000 Hz, default 12000)
//! ```
//!
//! a header line of `"  %-26s %s"` (option, help) and a range line of
//! `"  %-26s (%s)%s"` (blank, range, an optional `[read-only]` /
//! `[command line only]` tag). Parsing that is what lets the GUI build itself
//! from whatever binaries are on disk rather than from a table compiled in
//! here, so a seventh filter shows up without this file changing.
//!
//! What it deliberately does *not* try to recover is the C type: the range
//! line cannot tell `PWF_INT` from `PWF_DOUBLE` for a parameter that carries
//! a unit. It does not need to. Values are sent as text and the filter echoes
//! back the value it actually took, so the filter stays the authority on
//! rounding and clamping and the GUI only ever displays what came back.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Which half of the help text a parameter came from.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Section {
    /// "filter parameters (live, settable on stdin)" -- the DSP knobs.
    Filter,
    /// "stream and node" -- channels, bypass, meter, the PipeWire plumbing.
    Core,
}

#[derive(Clone, Debug)]
pub enum ParamKind {
    Number {
        min: f64,
        max: f64,
        /// The help placeholder was `N`, so whole numbers only.
        integer: bool,
    },
    Bool,
    Enum(Vec<String>),
    Text,
}

#[derive(Clone, Debug)]
pub struct ParamDesc {
    pub name: String,
    pub kind: ParamKind,
    pub unit: Option<String>,
    pub help: String,
    pub default: String,
    pub readonly: bool,
    pub cli_only: bool,
    pub section: Section,
}

impl ParamDesc {
    /// Live means the GUI may set it on stdin while audio is flowing.
    pub fn live(&self) -> bool {
        !self.readonly && !self.cli_only
    }

    /// Whether the slider for this parameter should be logarithmic.
    ///
    /// A quantity spanning two decades or more reads as nothing at all on a
    /// linear slider: an attack of 10 ms would sit at 1 % of travel on a
    /// 0..1000 ms parameter, and every value anyone actually uses would be
    /// crowded into the first few pixels. Frequencies, times and ratios are
    /// all like that.
    ///
    /// Decibels are the exception, and not because of their range: a dB scale
    /// *is* a logarithmic scale already, so a threshold from -80 to 0 dB is
    /// linear in exactly the sense that matters and putting it on a log
    /// slider would bend it twice.
    pub fn logarithmic(&self) -> bool {
        match &self.kind {
            ParamKind::Number { min, max, .. } => {
                self.unit.as_deref() != Some("dB")
                    && *min >= 0.0
                    && *max / self.log_floor(*min) >= 100.0
            }
            _ => false,
        }
    }

    /// The bottom of the logarithmic part of the scale.
    ///
    /// Several frequency parameters use 0 to mean "off" -- a sidechain
    /// high-pass, say. Zero has no logarithm, so the scale runs from 1 Hz and
    /// the very bottom of the travel is that off value, which is both
    /// reachable and the only place it can be.
    pub fn log_floor(&self, min: f64) -> f64 {
        if min > 0.0 {
            min
        } else {
            1.0
        }
    }

    pub fn default_f64(&self) -> f64 {
        match &self.kind {
            ParamKind::Bool => {
                if self.default == "on" {
                    1.0
                } else {
                    0.0
                }
            }
            ParamKind::Enum(c) => c.iter().position(|x| *x == self.default).unwrap_or(0) as f64,
            _ => self.default.parse().unwrap_or(0.0),
        }
    }
}

#[derive(Clone, Debug)]
pub struct PluginDesc {
    /// `pw_lpf` -- the program name as it calls itself, not the file name.
    pub name: String,
    pub summary: String,
    pub path: PathBuf,
    pub params: Vec<ParamDesc>,
    /// Whether it answers "curve" with its response. A filter lists
    /// `--curve` in its help exactly when it does.
    pub curve: bool,
    /// Whether it has a mono sidechain input, `sc_in`, which a stage can be
    /// given a source for. Its help lists the port, under "ports", exactly
    /// when it does.
    pub sidechain: bool,
}

impl PluginDesc {
    pub fn param(&self, name: &str) -> Option<&ParamDesc> {
        self.params.iter().find(|p| p.name == name)
    }

    /// The knobs the GUI puts on a stage card: the filter's own live
    /// parameters, in the order the program lists them.
    pub fn live_params(&self) -> impl Iterator<Item = &ParamDesc> {
        self.params
            .iter()
            .filter(|p| p.section == Section::Filter && p.live())
    }

    /// Every live parameter at its declared default, which is the starting
    /// point for a freshly added stage.
    pub fn defaults(&self) -> BTreeMap<String, String> {
        self.live_params()
            .map(|p| (p.name.clone(), p.default.clone()))
            .collect()
    }
}

/// Where to look for filter binaries, most specific first.
///
/// `PWFX_FILTER_PATH` wins outright so a packaged build can point at
/// `/usr/libexec`; otherwise the guesses cover running from `bin/` and
/// running straight out of `cargo build` inside the source tree.
pub fn search_path() -> Vec<PathBuf> {
    let mut dirs: Vec<PathBuf> = Vec::new();
    let mut push = |p: PathBuf| {
        if p.is_dir() && !dirs.contains(&p) {
            dirs.push(p);
        }
    };

    if let Ok(env) = std::env::var("PWFX_FILTER_PATH") {
        for d in env.split(':').filter(|s| !s.is_empty()) {
            push(PathBuf::from(d));
        }
        return dirs;
    }

    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            push(dir.to_path_buf());
            // app/target/{debug,release}/pwfx -> the tree's own bin/
            for up in [2usize, 3, 4] {
                let mut p = dir.to_path_buf();
                for _ in 0..up {
                    p.pop();
                }
                push(p.join("bin"));
            }
        }
    }
    push(PathBuf::from("bin"));
    dirs
}

/// Every `pw_*` program found on the search path, deduplicated by name so an
/// earlier directory shadows a later one.
pub fn discover(dirs: &[PathBuf]) -> (Vec<PluginDesc>, Vec<String>) {
    let mut out: Vec<PluginDesc> = Vec::new();
    let mut errors = Vec::new();

    for dir in dirs {
        let entries = match std::fs::read_dir(dir) {
            Ok(e) => e,
            Err(_) => continue,
        };
        let mut paths: Vec<PathBuf> = entries
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| {
                p.file_name()
                    .and_then(|n| n.to_str())
                    .is_some_and(|n| n.starts_with("pw_") && !n.contains('.'))
                    && is_executable(p)
            })
            .collect();
        paths.sort();

        for path in paths {
            match probe(&path) {
                Ok(d) => {
                    if !out.iter().any(|p| p.name == d.name) {
                        out.push(d);
                    }
                }
                Err(e) => errors.push(format!("{}: {e}", path.display())),
            }
        }
    }
    (out, errors)
}

fn is_executable(p: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(p)
        .map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}

/// Run one binary's `--help` and parse it.
pub fn probe(path: &Path) -> Result<PluginDesc, String> {
    let out = Command::new(path)
        .arg("--help")
        .output()
        .map_err(|e| e.to_string())?;
    let text = String::from_utf8_lossy(&out.stdout);
    let mut d = parse_help(&text)?;
    d.path = path.to_path_buf();
    Ok(d)
}

// ---------------------------------------------------------------------------
// the parser

const FILTER_HEAD: &str = "filter parameters (live, settable on stdin)";
const CORE_HEAD: &str = "stream and node";
const END_HEAD: &str = "control protocol";

pub fn parse_help(text: &str) -> Result<PluginDesc, String> {
    let mut lines = text.lines();
    let first = lines.next().unwrap_or("");
    let (name, summary) = first
        .split_once(" -- ")
        .ok_or_else(|| format!("first line is not 'name -- summary': {first:?}"))?;

    let mut params: Vec<ParamDesc> = Vec::new();
    let mut section: Option<Section> = None;
    // The header line of the entry whose range line we are still waiting for.
    let mut pending: Option<(String, String)> = None;

    for line in text.lines() {
        let flat = line.trim_end();
        match flat.trim() {
            FILTER_HEAD if flat.starts_with(FILTER_HEAD) => {
                section = Some(Section::Filter);
                pending = None;
                continue;
            }
            CORE_HEAD if flat.starts_with(CORE_HEAD) => {
                section = Some(Section::Core);
                pending = None;
                continue;
            }
            END_HEAD if flat.starts_with(END_HEAD) => break,
            _ => {}
        }
        let Some(section) = section else { continue };

        if let Some(rhs) = range_line(flat) {
            // Closes the entry opened by the last header line.
            if let Some((lhs, help)) = pending.take() {
                match build(&lhs, &help, rhs, section) {
                    Some(p) => params.push(p),
                    None => return Err(format!("cannot parse parameter {lhs:?} ({rhs:?})")),
                }
            }
            continue;
        }

        if is_header(flat) {
            let (lhs, help) = split_header(flat);
            // `-h, --help` and `--check` are the program's own options, not
            // parameters, and have no range line; they simply never close.
            pending = Some((lhs, help));
        } else if !flat.trim().is_empty() {
            // Continuation of a multi-line help text.
            if let Some((_, help)) = pending.as_mut() {
                if !help.is_empty() {
                    help.push(' ');
                }
                help.push_str(flat.trim());
            }
        }
    }

    if params.is_empty() {
        return Err("no parameters found in --help".into());
    }

    Ok(PluginDesc {
        name: name.trim().to_string(),
        summary: summary.trim().to_string(),
        path: PathBuf::new(),
        params,
        curve: text.lines().any(|l| l.trim_start().starts_with("--curve ")),
        sidechain: text.lines().any(|l| l.trim_start().starts_with("sc_in ")),
    })
}

/// `"  %-26s (%s)%s"` -- 2 + 26 + 1 spaces, then the parenthesised range.
fn range_line(line: &str) -> Option<&str> {
    let rest = line.strip_prefix("  ")?;
    if rest.len() < FIELD + 2 || !rest[..FIELD + 1].bytes().all(|b| b == b' ') {
        return None;
    }
    let rest = rest[FIELD + 1..].trim_start();
    rest.strip_prefix('(')
}

/// A header line indents by two spaces and then has something in the option
/// field. That field is where the two kinds of line differ: a header puts the
/// option in it, a range line leaves it blank. The option itself is indented
/// further when it has no short form (`      --bypass`), so the leading
/// whitespace alone does not tell them apart.
fn is_header(line: &str) -> bool {
    let Some(body) = line.strip_prefix("  ") else {
        return false;
    };
    !body.chars().take(FIELD).collect::<String>().trim().is_empty()
}

/// The option field of `"  %-26s %s"`.
const FIELD: usize = 26;

/// `"  %-26s %s"`: the option field is 26 columns wide, then one space, then
/// the help. A field that overflows 26 columns still ends at the first run of
/// two spaces, which is the fallback.
fn split_header(line: &str) -> (String, String) {
    let body = &line[2..];
    if body.len() > FIELD + 1 && body.is_char_boundary(FIELD) && body.as_bytes()[FIELD] == b' ' {
        return (
            body[..FIELD].trim().to_string(),
            body[FIELD + 1..].trim().to_string(),
        );
    }
    match body.find("  ") {
        Some(i) => (body[..i].trim().to_string(), body[i..].trim().to_string()),
        None => (body.trim().to_string(), String::new()),
    }
}

fn build(lhs: &str, help: &str, rhs_and_tag: &str, section: Section) -> Option<ParamDesc> {
    let close = rhs_and_tag.rfind(')')?;
    let rhs = rhs_and_tag[..close].trim();
    let tag = rhs_and_tag[close + 1..].trim();

    // A read-only parameter is printed as its bare name; everything settable
    // carries `--name PLACEHOLDER`.
    let (name, placeholder) = match lhs.split_once("--") {
        Some((_, rest)) => {
            let mut it = rest.split_whitespace();
            (it.next()?.to_string(), it.next().unwrap_or("").to_string())
        }
        None => (lhs.split_whitespace().next()?.to_string(), String::new()),
    };
    if name.is_empty() || name == "help" || name == "check" {
        return None;
    }

    let (kind, unit, default) = parse_range(rhs, &placeholder)?;

    Some(ParamDesc {
        name,
        kind,
        unit,
        help: help.to_string(),
        default,
        readonly: tag.contains("read-only"),
        cli_only: tag.contains("command line only"),
        section,
    })
}

/// The four shapes `print_param_help()` can produce inside the parentheses.
fn parse_range(rhs: &str, placeholder: &str) -> Option<(ParamKind, Option<String>, String)> {
    if let Some((head, default)) = rhs.rsplit_once(", default ") {
        let default = default.trim().to_string();

        if let Some((lo, hi_unit)) = head.split_once("..") {
            let min: f64 = lo.trim().parse().ok()?;
            let mut it = hi_unit.split_whitespace();
            let max: f64 = it.next()?.parse().ok()?;
            let unit = it.next().map(|u| u.to_string());
            return Some((
                ParamKind::Number {
                    min,
                    max,
                    integer: placeholder == "N",
                },
                unit,
                default,
            ));
        }
        if head.contains('|') {
            let choices: Vec<String> = head.split('|').map(|c| c.trim().to_string()).collect();
            return Some((ParamKind::Enum(choices), None, default));
        }
        return None;
    }

    if let Some(v) = rhs.strip_prefix("default ") {
        // Only PWF_BOOL prints a bare default, and only as on/off.
        return Some((ParamKind::Bool, None, v.trim().to_string()));
    }
    if rhs == "unset" {
        return Some((ParamKind::Text, None, String::new()));
    }
    Some((ParamKind::Text, None, rhs.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    const HELP: &str = "\
pw_gate -- noise gate with lookahead, hysteresis and hold

usage: pw_gate [options]

filter parameters (live, settable on stdin)
  -t, --threshold DB         level that opens the gate, dBFS
                             (-120..0 dB, default -35)
  -k, --lookahead MS         audio delay feeding the detector; keep >= attack
                             (0..200 ms, default 5)

stream and node
  -c, --channels N           number of mono port pairs to create
                             (1..64, default 2)  [command line only]
  latency                    latency this filter adds, reported to the graph
                             (0..1.67772e+07 samples, default 0)  [read-only]
      --bypass on|off        pass the input through, crossfaded over 10 ms
                             (default off)
      --node-name STR        PipeWire node.name (default: the program name)
                             (unset)  [command line only]
  -l, --link MODE            channel linking; off compresses each channel alone
                             (max|avg|off, default max)

  -h, --help                 this text

control protocol
  On start every parameter is printed to stdout.
";

    #[test]
    fn parses_every_shape() {
        let d = parse_help(HELP).unwrap();
        assert_eq!(d.name, "pw_gate");
        assert_eq!(d.summary, "noise gate with lookahead, hysteresis and hold");

        let live: Vec<&str> = d.live_params().map(|p| p.name.as_str()).collect();
        assert_eq!(live, ["threshold", "lookahead"]);

        let t = d.param("threshold").unwrap();
        assert!(matches!(t.kind, ParamKind::Number { min, max, integer: false }
                         if min == -120.0 && max == 0.0));
        assert_eq!(t.unit.as_deref(), Some("dB"));
        assert_eq!(t.default, "-35");
        assert_eq!(t.help, "level that opens the gate, dBFS");

        // `>= attack` in the help must not be mistaken for anything.
        assert_eq!(d.param("lookahead").unwrap().default, "5");

        let c = d.param("channels").unwrap();
        assert!(c.cli_only && !c.live());
        assert!(matches!(c.kind, ParamKind::Number { integer: true, .. }));

        let l = d.param("latency").unwrap();
        assert!(l.readonly && !l.live());
        assert!(matches!(l.kind, ParamKind::Number { max, .. } if max > 1.6e7));

        assert!(matches!(d.param("bypass").unwrap().kind, ParamKind::Bool));
        assert_eq!(d.param("bypass").unwrap().default, "off");

        let n = d.param("node-name").unwrap();
        assert!(matches!(n.kind, ParamKind::Text) && n.default.is_empty());

        match &d.param("link").unwrap().kind {
            ParamKind::Enum(c) => assert_eq!(c, &["max", "avg", "off"]),
            k => panic!("expected an enum, got {k:?}"),
        }

        // `-h, --help` has no range line and must not become a parameter.
        assert!(d.param("help").is_none());
    }

    /// Swap the threshold's range line for another and ask what kind of
    /// slider the result wants.
    fn is_log(range: &str) -> bool {
        parse_help(&HELP.replace("(-120..0 dB, default -35)", range))
            .unwrap()
            .param("threshold")
            .unwrap()
            .logarithmic()
    }

    #[test]
    fn wide_ranges_get_a_logarithmic_slider() {
        assert!(is_log("(1..384000 Hz, default 12000)"));   // a cutoff
        assert!(is_log("(0..1000 ms, default 10)"));        // an attack
        assert!(is_log("(1..100, default 3)"));             // a ratio
        assert!(is_log("(0.05..40, default 1)"));           // a bell Q
        // 0 Hz means "off" on a sidechain filter, and must not cost it one.
        assert!(is_log("(0..384000 Hz, default 0)"));
    }

    #[test]
    fn decibels_and_narrow_ranges_stay_linear() {
        // A dB scale is already logarithmic; bending it again would be wrong.
        assert!(!is_log("(-120..0 dB, default -35)"));
        assert!(!is_log("(0..144 dB, default 120)"));
        assert!(!is_log("(0..40 dB, default 6)"));
        // Under two decades there is nothing to spread out.
        assert!(!is_log("(1..16, default 4)"));
        assert!(!is_log("(1..64, default 2)"));
    }
}

/// Parsing the real binaries, when they are built.
///
/// The unit tests above pin the shapes; this one pins the agreement with
/// whatever `src/pwf.c` currently prints. If the help formatter changes, this
/// is what notices.
#[cfg(test)]
mod against_the_real_filters {
    use super::*;

    fn filters() -> Vec<PluginDesc> {
        let (found, errors) = discover(&search_path());
        assert!(errors.is_empty(), "some pw_* binaries did not parse: {errors:?}");
        found
    }

    #[test]
    fn every_installed_filter_parses() {
        let found = filters();
        if found.is_empty() {
            eprintln!("no pw_* binaries on the search path; run `make` first");
            return;
        }
        for d in &found {
            assert!(d.name.starts_with("pw_"), "{:?}", d.name);
            assert!(!d.summary.is_empty(), "{} has no summary", d.name);
            assert!(
                d.live_params().count() > 0,
                "{} came back with no live parameters",
                d.name
            );
            // Every filter has these three, and the GUI relies on all of
            // them: bypass for the stage toggle, channels for the chain
            // width, latency for the stage header.
            for name in ["bypass", "channels", "latency", "meter"] {
                assert!(d.param(name).is_some(), "{} has no {name}", d.name);
            }
            assert!(d.param("channels").unwrap().cli_only);
            assert!(d.param("latency").unwrap().readonly);

            // Every filter in the tree draws a curve but the delay, whose
            // response is a comb far finer than any plot, and the noise
            // suppressor, whose response adapts to what it hears -- and each
            // says so.
            let curveless = ["pw_delay", "pw_denoise"].contains(&d.name.as_str());
            assert_eq!(d.curve, !curveless, "{}: --curve in its help", d.name);

            // A sidechain input on the sidechain compressor and nothing
            // else: the stage card offers a source for it off this.
            assert_eq!(d.sidechain, d.name == "pw_sccomp", "{}: sc_in in its help", d.name);
            // and a meter for it there, read-only, and nowhere else
            match d.param("sc-level") {
                Some(sc) => {
                    assert!(d.sidechain, "{} has an sc-level meter", d.name);
                    assert!(sc.readonly && !sc.live());
                }
                None => assert!(!d.sidechain, "{} has no sc-level meter", d.name),
            }

            // A signed gain meter on the leveler, the one filter that turns
            // up as well as down, read-only, and on nothing else.
            match d.param("gain-applied") {
                Some(g) => {
                    assert_eq!(d.name, "pw_level", "{} has a gain-applied meter", d.name);
                    assert!(g.readonly && !g.live());
                }
                None => assert_ne!(d.name, "pw_level", "pw_level has no gain-applied meter"),
            }

            // A gain-reduction meter on exactly the filters that reduce
            // gain, read-only and so never a knob. The window draws the
            // strip off this, so it must not appear on a low-pass.
            let dynamics = [
                "pw_gate", "pw_comp", "pw_sccomp", "pw_limit", "pw_deess", "pw_denoise",
            ]
                .contains(&d.name.as_str());
            match d.param("gr") {
                Some(gr) => {
                    assert!(dynamics, "{} has a gr meter", d.name);
                    assert!(gr.readonly && !gr.live());
                }
                None => assert!(!dynamics, "{} has no gr meter", d.name),
            }
            assert!(matches!(d.param("bypass").unwrap().kind, ParamKind::Bool));

            // Nothing may come back with a range a slider cannot sit on.
            for p in d.live_params() {
                if let ParamKind::Number { min, max, .. } = p.kind {
                    assert!(max > min, "{}: {} has an empty range", d.name, p.name);
                    let def = p.default_f64();
                    assert!(
                        (min..=max).contains(&def),
                        "{}: {} defaults to {def}, outside {min}..{max}",
                        d.name,
                        p.name
                    );
                }
            }
        }
    }

    #[test]
    fn the_filters_this_tree_ships_look_like_themselves() {
        let found = filters();
        if found.is_empty() {
            return;
        }
        let by_name = |n: &str| found.iter().find(|d| d.name == n).cloned();

        if let Some(lpf) = by_name("pw_lpf") {
            let cutoff = lpf.param("cutoff").unwrap();
            assert!(cutoff.logarithmic(), "a cutoff needs a logarithmic slider");
            assert_eq!(cutoff.unit.as_deref(), Some("Hz"));
            let order = lpf.param("order").unwrap();
            assert!(matches!(order.kind, ParamKind::Number { integer: true, .. }));
        }
        if let Some(comp) = by_name("pw_comp") {
            // The knobs whose useful values live in the bottom few percent of
            // a linear slider.
            for n in ["ratio", "attack", "release", "lookahead", "rms"] {
                assert!(comp.param(n).unwrap().logarithmic(), "{n} needs a log slider");
            }
            for n in ["threshold", "knee", "makeup"] {
                assert!(!comp.param(n).unwrap().logarithmic(), "{n} is in dB already");
            }
            match &comp.param("link").unwrap().kind {
                ParamKind::Enum(c) => assert_eq!(c, &["max", "avg", "off"]),
                k => panic!("comp link should be an enum, got {k:?}"),
            }
            assert!(matches!(
                comp.param("auto-makeup").unwrap().kind,
                ParamKind::Bool
            ));
        }
        if let Some(level) = by_name("pw_level") {
            // Loudness in LUFS is a dB scale with another name: linear
            // sliders, and the unit carried through to the knob.
            for n in ["loudness", "gate"] {
                let p = level.param(n).unwrap();
                assert_eq!(p.unit.as_deref(), Some("LUFS"));
                assert!(!p.logarithmic(), "{n} is a level already");
            }
            // Its times span three decades, from a test's 10 ms to a
            // broadcast ride's half a minute.
            for n in ["attack", "release"] {
                assert!(level.param(n).unwrap().logarithmic(), "{n} needs a log slider");
            }
            assert!(level.curve);
        }
        if let Some(limit) = by_name("pw_limit") {
            match &limit.param("oversample").unwrap().kind {
                ParamKind::Enum(c) => assert_eq!(c, &["1", "2", "4", "8", "16"]),
                k => panic!("limit oversample should be an enum, got {k:?}"),
            }
        }
        if let Some(eq) = by_name("pw_eq") {
            // Five bands, split into the one-value-per-line form.
            for b in ["ls", "b2", "b3", "b4", "hs"] {
                assert!(eq.param(&format!("{b}-freq")).is_some(), "{b}-freq");
                assert!(eq.param(&format!("{b}-gain")).is_some(), "{b}-gain");
            }
        }
        if let Some(gate) = by_name("pw_gate") {
            // The deliberate divergence from the reference CLI.
            assert!(gate.param("hysteresis").is_some());
            assert!(gate.param("close").is_none());
        }
    }
}
