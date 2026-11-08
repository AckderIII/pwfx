//! Undo and redo: the state as it was before each edit.
//!
//! The whole user-visible model is one `AppState`, and the engine turns any
//! state it is handed into processes, buses and links by comparison. So an
//! undo is nothing more than putting an earlier state back and applying it:
//! the reconcile loops restart what has to restart and leave the rest
//! playing, exactly as when a preset is loaded.
//!
//! A step is recorded for every edit that changes the state, labelled and
//! keyed by `describe`, which compares the state before and after rather
//! than trusting the caller to say what it did -- so any new kind of edit
//! is undoable, with a sensible name, without touching this file. Two
//! things are kept out:
//!
//! - `meters`, which is how the window looks rather than what it does.
//!   Toggling it records nothing, and an undo keeps the current setting
//!   rather than reviving whatever it was back then.
//! - Values a filter reported back (`Engine::absorb_reports`): those are the
//!   filter correcting the state, not the user changing it, and they do not
//!   go through an edit at all.
//!
//! A slider drag is one step, not fifty: an edit that changes exactly one
//! parameter carries a key, and an edit with the same key as the step on
//! top, within `MERGE` of that step's last change, folds into it.

use std::time::{Duration, Instant};

use crate::state::{AppState, Bus, Chain};

/// How long after the last change to a parameter the next change to the
/// same one still counts as the same gesture.
pub const MERGE: Duration = Duration::from_secs(1);

/// How many steps are kept. Each is a copy of the state, which is small.
pub const LIMIT: usize = 200;

struct Step {
    /// The state before the edit.
    state: AppState,
    label: String,
    key: Option<String>,
    at: Instant,
}

#[derive(Default)]
pub struct History {
    undo: Vec<Step>,
    redo: Vec<Step>,
}

impl History {
    /// Record an edit that took the state away from `before`. A new edit
    /// ends whatever could have been redone.
    pub fn record(&mut self, before: AppState, label: String, key: Option<String>, now: Instant) {
        self.redo.clear();
        if let (Some(top), Some(k)) = (self.undo.last_mut(), key.as_ref()) {
            if top.key.as_ref() == Some(k) && now.duration_since(top.at) < MERGE {
                // The same gesture, still going: the step keeps the state
                // from before it began, and the clock restarts.
                top.at = now;
                return;
            }
        }
        self.undo.push(Step { state: before, label, key, at: now });
        if self.undo.len() > LIMIT {
            self.undo.remove(0);
        }
    }

    /// The state to go back to, given the one in force now, and what the
    /// step was called.
    pub fn undo(&mut self, current: AppState) -> Option<(AppState, String)> {
        let step = self.undo.pop()?;
        self.redo.push(Step { state: current, label: step.label.clone(), key: None, at: step.at });
        Some((step.state, step.label))
    }

    pub fn redo(&mut self, current: AppState) -> Option<(AppState, String)> {
        let step = self.redo.pop()?;
        // Not mergeable once redone: the gesture it came from is over.
        self.undo.push(Step { state: current, label: step.label.clone(), key: None, at: step.at });
        Some((step.state, step.label))
    }

    pub fn can_undo(&self) -> bool {
        !self.undo.is_empty()
    }

    pub fn can_redo(&self) -> bool {
        !self.redo.is_empty()
    }
}

/// Whether two states differ in anything but `meters`.
pub fn differs(a: &AppState, b: &AppState) -> bool {
    a.chains != b.chains || a.buses != b.buses || a.version != b.version
}

/// What an edit did, in words, and the key that lets a run of them fold
/// into one step -- `Some` only for a change to exactly one parameter.
pub fn describe(before: &AppState, after: &AppState) -> (String, Option<String>) {
    if let Some(d) = list_change(&before.chains, &after.chains, |c| &c.id, |c| &c.name, "chain") {
        return (d, None);
    }
    if let Some(d) = list_change(&before.buses, &after.buses, |b| &b.id, |b| &b.name, "bus") {
        return (d, None);
    }
    for (a, b) in before.chains.iter().zip(&after.chains) {
        if a != b {
            return chain_change(a, b);
        }
    }
    for (a, b) in before.buses.iter().zip(&after.buses) {
        if a != b {
            return (bus_change(a, b), None);
        }
    }
    ("change".into(), None)
}

/// An item added or removed, if that is what happened to the list.
fn list_change<T>(
    a: &[T],
    b: &[T],
    id: impl Fn(&T) -> &String,
    name: impl Fn(&T) -> &String,
    what: &str,
) -> Option<String> {
    if let Some(new) = b.iter().find(|x| !a.iter().any(|y| id(y) == id(x))) {
        return Some(format!("add {what} {}", name(new)));
    }
    if let Some(gone) = a.iter().find(|x| !b.iter().any(|y| id(y) == id(x))) {
        return Some(format!("delete {what} {}", name(gone)));
    }
    None
}

fn chain_change(a: &Chain, b: &Chain) -> (String, Option<String>) {
    let c = &b.name;
    if a.name != b.name {
        return (format!("rename chain {} to {}", a.name, b.name), None);
    }
    if a.enabled != b.enabled {
        let verb = if b.enabled { "start" } else { "stop" };
        return (format!("{verb} chain {c}"), None);
    }
    if a.width != b.width {
        return (format!("set the width of {c}"), None);
    }
    if a.input != b.input {
        return (format!("change the input of {c}"), None);
    }
    if a.output != b.output {
        return (format!("change the output of {c}"), None);
    }
    let ids = |ch: &Chain| ch.stages.iter().map(|s| s.id.clone()).collect::<Vec<_>>();
    if let Some(new) = b.stages.iter().find(|s| !a.stages.iter().any(|t| t.id == s.id)) {
        return (format!("add {} to {c}", new.plugin), None);
    }
    if let Some(gone) = a.stages.iter().find(|s| !b.stages.iter().any(|t| t.id == s.id)) {
        return (format!("remove {} from {c}", gone.plugin), None);
    }
    if ids(a) != ids(b) {
        return (format!("reorder the filters in {c}"), None);
    }
    for (x, y) in a.stages.iter().zip(&b.stages) {
        if x.bypass != y.bypass {
            let verb = if y.bypass { "bypass" } else { "un-bypass" };
            return (format!("{verb} {} in {c}", y.plugin), None);
        }
        if x.sidechain != y.sidechain {
            return (format!("change the sidechain of {} in {c}", y.plugin), None);
        }
        if x.params != y.params {
            let moved: Vec<&String> = y
                .params
                .iter()
                .filter(|(k, v)| x.params.get(*k) != Some(v))
                .map(|(k, _)| k)
                .chain(x.params.keys().filter(|k| !y.params.contains_key(*k)))
                .collect();
            return match moved.as_slice() {
                [one] => (
                    format!("set {} {one} in {c}", y.plugin),
                    Some(format!("param {}.{}.{one}", b.id, y.id)),
                ),
                _ => (format!("change {} in {c}", y.plugin), None),
            };
        }
    }
    (format!("change {c}"), None)
}

fn bus_change(a: &Bus, b: &Bus) -> String {
    let n = &b.name;
    if a.name != b.name {
        format!("rename bus {} to {}", a.name, b.name)
    } else if a.enabled != b.enabled {
        format!("switch bus {n} {}", if b.enabled { "on" } else { "off" })
    } else if a.kind != b.kind {
        format!("change the kind of bus {n}")
    } else if a.width != b.width {
        format!("set the width of bus {n}")
    } else if a.apps != b.apps {
        format!("change the applications on bus {n}")
    } else {
        format!("change bus {n}")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::state::{BusKind, Stage};
    use std::collections::BTreeMap;

    fn state() -> AppState {
        let mut st = AppState::default();
        st.chains.push(Chain {
            id: "voice".into(),
            name: "Voice".into(),
            enabled: true,
            width: 2,
            stages: vec![
                Stage { id: "hpf".into(), plugin: "pw_hpf".into(), bypass: false, sidechain: None,
                        params: BTreeMap::from([("cutoff".into(), "80".into())]) },
                Stage { id: "comp".into(), plugin: "pw_comp".into(), bypass: false, sidechain: None,
                        params: BTreeMap::from([("ratio".into(), "3".into()),
                                                ("threshold".into(), "-18".into())]) },
            ],
            input: None,
            output: None,
        });
        st.buses.push(Bus {
            id: "mic".into(),
            name: "Mic".into(),
            enabled: true,
            width: 2,
            kind: BusKind::Source,
            apps: Vec::new(),
        });
        st
    }

    fn set(st: &mut AppState, stage: usize, k: &str, v: &str) {
        st.chains[0].stages[stage].params.insert(k.into(), v.into());
    }

    #[test]
    fn edits_are_named_by_what_changed() {
        let a = state();
        let name = |f: &dyn Fn(&mut AppState)| {
            let mut b = a.clone();
            f(&mut b);
            describe(&a, &b).0
        };
        assert_eq!(name(&|s| s.chains.clear()), "delete chain Voice");
        assert_eq!(name(&|s| s.chains[0].name = "Talk".into()), "rename chain Voice to Talk");
        assert_eq!(name(&|s| s.chains[0].enabled = false), "stop chain Voice");
        assert_eq!(name(&|s| { s.chains[0].stages.remove(0); }), "remove pw_hpf from Voice");
        assert_eq!(name(&|s| s.chains[0].stages.swap(0, 1)), "reorder the filters in Voice");
        assert_eq!(name(&|s| s.chains[0].stages[1].bypass = true), "bypass pw_comp in Voice");
        assert_eq!(
            name(&|s| s.chains[0].stages[1].sidechain = Some(crate::state::Endpoint {
                node: "music".into(),
                label: "Music".into(),
                mapping: Default::default(),
            })),
            "change the sidechain of pw_comp in Voice"
        );
        assert_eq!(name(&|s| set(s, 1, "ratio", "4")), "set pw_comp ratio in Voice");
        assert_eq!(name(&|s| { set(s, 1, "ratio", "4"); set(s, 1, "threshold", "-20") }),
                   "change pw_comp in Voice");
        assert_eq!(name(&|s| s.buses[0].kind = BusKind::Sink), "change the kind of bus Mic");
        assert_eq!(name(&|s| s.buses[0].apps.push("Zoom".into())),
                   "change the applications on bus Mic");
        let mut b = a.clone();
        b.chains.push(Chain { id: "music".into(), name: "Music".into(), ..a.chains[0].clone() });
        assert_eq!(describe(&a, &b).0, "add chain Music");
    }

    #[test]
    fn only_a_single_parameter_change_can_merge() {
        let a = state();
        let mut b = a.clone();
        set(&mut b, 1, "ratio", "4");
        assert_eq!(describe(&a, &b).1.as_deref(), Some("param voice.comp.ratio"));
        b.chains[0].stages[1].bypass = true;
        assert_eq!(describe(&a, &b).1, None);
    }

    #[test]
    fn a_drag_is_one_step_and_a_pause_starts_another() {
        let mut h = History::default();
        let t0 = Instant::now();
        let mut st = state();
        let start = st.clone();
        // fifty slider positions, 20 ms apart
        for i in 0..50 {
            let before = st.clone();
            set(&mut st, 1, "ratio", &format!("{}", 4 + i));
            let (label, key) = describe(&before, &st);
            h.record(before, label, key, t0 + Duration::from_millis(20 * i));
        }
        // a different parameter right after: a step of its own
        let before = st.clone();
        set(&mut st, 1, "threshold", "-24");
        let (label, key) = describe(&before, &st);
        h.record(before.clone(), label, key, t0 + Duration::from_millis(1100));

        let (back, label) = h.undo(st.clone()).unwrap();
        assert_eq!(label, "set pw_comp threshold in Voice");
        assert_eq!(back, before);
        let (back, label) = h.undo(back).unwrap();
        assert_eq!(label, "set pw_comp ratio in Voice");
        assert_eq!(back, start, "the whole drag comes back in one step");
        assert!(!h.can_undo());

        // the same parameter after a pause is a new gesture
        let mut h = History::default();
        let mut st = state();
        for (i, at) in [0u64, 100, 1500].iter().enumerate() {
            let before = st.clone();
            set(&mut st, 1, "ratio", &format!("{}", 5 + i));
            let (label, key) = describe(&before, &st);
            h.record(before, label, key, t0 + Duration::from_millis(*at));
        }
        assert!(h.undo(st.clone()).is_some());
        assert!(h.undo(state()).is_some());
        assert!(!h.can_undo(), "two gestures, two steps");
    }

    #[test]
    fn redo_walks_forward_again_and_a_new_edit_ends_it() {
        let mut h = History::default();
        let t = Instant::now();
        let a = state();
        let mut b = a.clone();
        b.chains[0].enabled = false;
        h.record(a.clone(), describe(&a, &b).0, None, t);

        let (back, _) = h.undo(b.clone()).unwrap();
        assert_eq!(back, a);
        assert!(h.can_redo());
        let (fwd, label) = h.redo(back.clone()).unwrap();
        assert_eq!((fwd, label.as_str()), (b.clone(), "stop chain Voice"));

        h.undo(b.clone()).unwrap();
        let mut c = a.clone();
        c.chains[0].name = "Talk".into();
        h.record(a.clone(), describe(&a, &c).0, None, t);
        assert!(!h.can_redo(), "an edit after an undo starts a new branch");
    }

    #[test]
    fn the_meters_switch_is_not_an_edit() {
        let a = state();
        let mut b = a.clone();
        b.meters = !a.meters;
        assert!(!differs(&a, &b));
    }

    #[test]
    fn history_is_bounded() {
        let mut h = History::default();
        let t = Instant::now();
        for i in 0..LIMIT + 50 {
            h.record(state(), format!("edit {i}"), None, t);
        }
        let mut n = 0;
        let mut st = state();
        while let Some((s, _)) = h.undo(st) {
            st = s;
            n += 1;
        }
        assert_eq!(n, LIMIT);
    }
}
