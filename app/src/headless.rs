//! `pwfx --headless`: the chains and buses with no window.
//!
//! The same engine and the same PipeWire thread as the window, driven by a
//! plain loop instead of Slint's timers. Nothing here is new behaviour; it
//! is the four reconcile loops with nobody looking at them, which is what a
//! microphone chain wants to be once it has been set up -- running from
//! login, whether or not the window is open.
//!
//! It never writes a save file. The file it was started from is the
//! authority, and a filter's clamped value is not a reason to rewrite
//! someone's preset. To change what runs, edit the file (or close this, use
//! the window, and start this again) and send SIGHUP: the new state goes
//! through the same comparison as a loaded preset, so only what moved is
//! restarted.

use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crate::pwthread::PwEvent;
use crate::state::{self, AppState};
use crate::{apply, boot, stop, AppEvent, HOUSEKEEPING};

/// How long to wait for an event before looking at the signal flags again.
/// Signals do not interrupt the wait, so this is how late a SIGTERM can be.
const POLL: Duration = Duration::from_millis(200);

static QUIT: AtomicBool = AtomicBool::new(false);
static RELOAD: AtomicBool = AtomicBool::new(false);

extern "C" fn on_quit(_: libc::c_int) {
    QUIT.store(true, Ordering::Relaxed);
}

extern "C" fn on_reload(_: libc::c_int) {
    RELOAD.store(true, Ordering::Relaxed);
}

pub fn run(file: Option<PathBuf>) -> ExitCode {
    let state = match load(file.as_deref()) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("pwfx: {e}");
            return ExitCode::FAILURE;
        }
    };
    unsafe {
        libc::signal(libc::SIGTERM, on_quit as *const () as libc::sighandler_t);
        libc::signal(libc::SIGINT, on_quit as *const () as libc::sighandler_t);
        libc::signal(libc::SIGHUP, on_reload as *const () as libc::sighandler_t);
    }

    let (mut engine, pw, rx) = boot(state, None);
    engine.note(summary(&engine.state, file.as_deref()), false);
    apply(&mut engine, &pw);

    let mut links = (0, 0);
    let mut next_housekeeping = Instant::now() + HOUSEKEEPING;
    while !QUIT.load(Ordering::Relaxed) {
        if RELOAD.swap(false, Ordering::Relaxed) {
            match load(file.as_deref()) {
                Ok(s) => {
                    engine.state = s;
                    engine.note(
                        format!("reloaded: {}", summary(&engine.state, file.as_deref())),
                        false,
                    );
                    apply(&mut engine, &pw);
                }
                Err(e) => engine.note(format!("reload failed, keeping what is running: {e}"), true),
            }
        }

        let mut relink = false;
        if let Ok(first) = rx.recv_timeout(POLL) {
            for ev in std::iter::once(first).chain(rx.try_iter()) {
                match ev {
                    AppEvent::Proc(p) => engine.on_proc_event(p),
                    AppEvent::Pw(PwEvent::Graph(g)) => {
                        engine.set_graph(g);
                        relink = true;
                    }
                    AppEvent::Pw(PwEvent::Links { wanted, made }) => {
                        // The one status worth a line in the journal: links
                        // going down and coming back is a device leaving and
                        // returning, seen from here.
                        if (wanted, made) != links {
                            links = (wanted, made);
                            engine.note(format!("{made}/{wanted} links up"), false);
                        }
                    }
                    AppEvent::Pw(PwEvent::Error(e)) => engine.note(e, true),
                }
            }
        }
        if relink {
            apply(&mut engine, &pw);
        }
        if Instant::now() >= next_housekeeping {
            next_housekeeping = Instant::now() + HOUSEKEEPING;
            // A filter that died is restarted by the same comparison that
            // started it. What the filters report is taken in so the state
            // stays theirs, but it is not saved.
            apply(&mut engine, &pw);
            engine.absorb_reports();
        }
    }

    engine.note("stopping", false);
    stop(&mut engine, &pw);
    ExitCode::SUCCESS
}

/// The state to run: the named file, else a preset of that name, else
/// whatever the window would start with.
///
/// Unlike the window, a file that will not read is fatal. The window can
/// start empty and say why; a service that starts empty looks like it is
/// working and does nothing.
fn load(file: Option<&Path>) -> Result<AppState, String> {
    let mut state = match file {
        Some(path) if path.exists() => state::read(path)?,
        Some(name) => {
            let preset = state::preset_path(&name.to_string_lossy());
            if !preset.exists() {
                return Err(format!("{}: no such file or preset", name.display()));
            }
            state::read(&preset)?
        }
        None => match state::startup() {
            (s, None) => s,
            (_, Some(e)) => return Err(e),
        },
    };
    // Nobody is looking at the meters, so the filters need not report them.
    state.meters = false;
    Ok(state)
}

fn summary(st: &AppState, file: Option<&Path>) -> String {
    let chains = st.chains.iter().filter(|c| c.enabled).count();
    let buses = st.buses.iter().filter(|b| b.enabled).count();
    let from = match file {
        Some(f) => f.display().to_string(),
        None => "the last session".into(),
    };
    format!(
        "running {chains} chain{} and {buses} bus{} from {from}",
        if chains == 1 { "" } else { "s" },
        if buses == 1 { "" } else { "es" },
    )
}
