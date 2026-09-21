//! One running filter binary, and the line protocol on its pipes.
//!
//! Each stage of each chain is its own process. The app speaks the same
//! protocol a person would type at it: settings go in on stdin as
//! `name value`, and every line that comes back on stdout is either
//! `name value` or a `#`-comment. Errors arrive on stderr and leave the
//! parameter as it was.
//!
//! The consequence worth stating is that **the filter, not the GUI, is the
//! authority on a parameter's value.** The GUI sends what the user asked for
//! and then displays whatever came back, so rounding, clamping, a rejected
//! cross-parameter combination and the extra `latency` line that a changed
//! lookahead drags with it all land in the GUI without it having to model any
//! of them. A parameter the filter refuses simply never changes on screen,
//! and the reason appears in the log.

use std::collections::BTreeMap;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::process::CommandExt;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::Arc;

use crate::plugin::{PluginDesc, Section};

/// Anything a filter process says, tagged with the node name that says it --
/// which is unique, and is how the GUI finds the stage it belongs to.
#[derive(Clone, Debug)]
pub enum ProcEvent {
    /// A `name value` line: either the startup dump, an echo of a change, or
    /// a meter reading.
    Param {
        node: String,
        name: String,
        value: String,
    },
    /// A `#`-comment, or a line on stderr.
    Note {
        node: String,
        text: String,
        error: bool,
    },
    Exited {
        node: String,
        text: String,
    },
}

pub type EventSink = Arc<dyn Fn(ProcEvent) + Send + Sync>;

pub struct FilterProc {
    pub plugin: String,
    pub channels: usize,
    child: Child,
    stdin: Option<ChildStdin>,
    spawned: std::time::Instant,
}

impl FilterProc {
    /// Start a filter.
    ///
    /// Everything is passed on the command line rather than pushed in over
    /// stdin afterwards, so the filter is already correct on its very first
    /// quantum instead of sliding into place over the next few. The arguments
    /// go in the order the program lists its parameters, because
    /// `parse_argv()` validates each one as it is applied and a
    /// cross-parameter constraint can be transiently false in another order.
    pub fn spawn(
        desc: &PluginDesc,
        node: &str,
        channels: usize,
        params: &BTreeMap<String, String>,
        meter_ms: u32,
        sink: EventSink,
    ) -> std::io::Result<Self> {
        let mut cmd = Command::new(&desc.path);
        cmd.arg(format!("--channels={channels}"))
            .arg(format!("--node-name={node}"))
            .arg(format!("--meter={meter_ms}"));

        for p in &desc.params {
            if p.section != Section::Filter || !p.live() {
                continue;
            }
            if let Some(v) = params.get(&p.name) {
                cmd.arg(format!("--{}={}", p.name, v));
            }
        }
        if let Some(v) = params.get("bypass") {
            cmd.arg(format!("--bypass={v}"));
        }

        cmd.stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());

        // If the GUI dies without unwinding, the filters have to go with it.
        // They deliberately keep running when stdin closes -- one started from
        // a shell should not exit just because its controller detached -- so
        // closing the pipe is not enough to clean up after a crash.
        unsafe {
            cmd.pre_exec(|| {
                libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM);
                Ok(())
            });
        }

        let mut child = cmd.spawn()?;
        let stdin = child.stdin.take();

        if let Some(out) = child.stdout.take() {
            let (node, sink) = (node.to_string(), sink.clone());
            std::thread::Builder::new()
                .name(format!("{node}/out"))
                .spawn(move || {
                    for line in BufReader::new(out).lines().map_while(Result::ok) {
                        sink(parse_line(&node, &line));
                    }
                    // stdout closing means the process is gone. Saying so
                    // here rather than waiting for the next liveness sweep is
                    // what makes a filter that died show as dead at once.
                    sink(ProcEvent::Exited {
                        node,
                        text: "the filter exited".into(),
                    });
                })
                .ok();
        }
        if let Some(err) = child.stderr.take() {
            let (node, sink) = (node.to_string(), sink.clone());
            std::thread::Builder::new()
                .name(format!("{node}/err"))
                .spawn(move || {
                    for line in BufReader::new(err).lines().map_while(Result::ok) {
                        sink(ProcEvent::Note {
                            node: node.clone(),
                            text: line.trim_start_matches("error: ").to_string(),
                            error: true,
                        });
                    }
                })
                .ok();
        }

        Ok(Self {
            plugin: desc.name.clone(),
            channels,
            child,
            stdin,
            spawned: std::time::Instant::now(),
        })
    }

    /// Ask for one parameter change. The value shown in the GUI does not move
    /// until the filter echoes it back.
    pub fn set(&mut self, name: &str, value: &str) -> std::io::Result<()> {
        let Some(stdin) = self.stdin.as_mut() else {
            return Err(std::io::Error::new(
                std::io::ErrorKind::BrokenPipe,
                "the filter is not running",
            ));
        };
        if value.is_empty() {
            writeln!(stdin, "{name}")?;
        } else {
            writeln!(stdin, "{name} {value}")?;
        }
        stdin.flush()
    }

    pub fn alive(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }

    /// How long it has been running, which is what tells a filter that
    /// crashed from one that simply never started.
    pub fn uptime(&self) -> std::time::Duration {
        self.spawned.elapsed()
    }

    /// Ask the filter to stop, and make sure it does.
    ///
    /// `quit` lets it leave the graph tidily -- the ports go away before the
    /// process does, so the server never sees a node vanish mid-quantum.
    pub fn shutdown(mut self) {
        let _ = self.set("quit", "");
        drop(self.stdin.take());

        let deadline = std::time::Instant::now() + std::time::Duration::from_millis(500);
        loop {
            match self.child.try_wait() {
                Ok(Some(_)) | Err(_) => return,
                Ok(None) if std::time::Instant::now() >= deadline => break,
                Ok(None) => std::thread::sleep(std::time::Duration::from_millis(10)),
            }
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Drop for FilterProc {
    fn drop(&mut self) {
        // Only reached on an unwind or a forgotten shutdown; the orderly path
        // consumes self.
        drop(self.stdin.take());
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// `name value`, a bare `name`, or a `#`-comment. Nothing else is legal on
/// stdout, which is what lets this be three lines.
fn parse_line(node: &str, line: &str) -> ProcEvent {
    let line = line.trim_end();
    if let Some(text) = line.strip_prefix('#') {
        return ProcEvent::Note {
            node: node.to_string(),
            text: text.trim().to_string(),
            error: false,
        };
    }
    let (name, value) = line.split_once(' ').unwrap_or((line, ""));
    ProcEvent::Param {
        node: node.to_string(),
        name: name.to_string(),
        value: value.trim().to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn param(line: &str) -> (String, String) {
        match parse_line("n", line) {
            ProcEvent::Param { name, value, .. } => (name, value),
            e => panic!("expected a parameter line, got {e:?}"),
        }
    }

    #[test]
    fn reads_the_protocol() {
        assert_eq!(param("cutoff 120"), ("cutoff".into(), "120".into()));
        // An unset string parameter reports as a bare name.
        assert_eq!(param("node-name"), ("node-name".into(), "".into()));
        // The trailing space a string parameter's report leaves behind.
        assert_eq!(param("target "), ("target".into(), "".into()));
        assert_eq!(param("in-level -5.5"), ("in-level".into(), "-5.5".into()));
        // An enum value is a word, not a number.
        assert_eq!(param("bells orfanidis"), ("bells".into(), "orfanidis".into()));
    }

    #[test]
    fn comments_are_not_parameters() {
        match parse_line("n", "# ok") {
            ProcEvent::Note { text, error, .. } => {
                assert_eq!(text, "ok");
                assert!(!error);
            }
            e => panic!("expected a note, got {e:?}"),
        }
    }
}
