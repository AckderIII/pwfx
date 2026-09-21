//! The PipeWire side of the app, which lives on its own thread.
//!
//! libpipewire's loop is not thread safe and its objects are not `Send`, so
//! everything that touches it stays here: the registry, the graph model built
//! from it, and the links the app owns. The rest of the program talks to this
//! thread in two directions and in terms of *names* only --
//!
//!   in   `PwCmd::SetLinks`, the set of links that ought to exist, as node
//!        and port names;
//!   out  `PwEvent::Graph`, a snapshot of what is out there.
//!
//! Declaring the wanted links rather than issuing connect/disconnect calls is
//! what makes the app survive things moving underneath it. Node ids change
//! every time a filter process restarts and every time a device changes
//! profile; names do not. So the desired set is stored by name, re-resolved on
//! every registry change, and anything that cannot be resolved yet is simply
//! not made until the node appears. A filter that takes 200 ms to come up
//! needs no waiting on and no retry logic: its ports appearing in the registry
//! *is* the retry.
//!
//! Links are held as proxies without `object.linger`, so if this process dies
//! the server tears them down with it and the graph is left as it was found.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;
use std::time::{Duration, Instant};

use pipewire as pw;
use pw::types::ObjectType;

use crate::graph::{Dir, Graph, Node, Port};

/// How long after a registry change the graph is rebuilt and the links
/// reconciled. Registry traffic arrives in bursts -- a device changing profile
/// moves dozens of globals at once -- so the work is done once per tick rather
/// than once per global.
const TICK: Duration = Duration::from_millis(50);

/// How long a freshly created link is taken on trust before its absence from
/// the registry counts as "someone tore it down".
const SETTLE: Duration = Duration::from_secs(2);

const RETRY: Duration = Duration::from_secs(2);

/// One link, addressed the way the app thinks about it.
#[derive(Clone, PartialEq, Eq, Hash, Debug)]
pub struct LinkSpec {
    pub out_node: String,
    pub out_port: String,
    pub in_node: String,
    pub in_port: String,
}

pub enum PwCmd {
    /// The complete set of links the app wants to exist. Anything it owns
    /// that is not in here is torn down.
    SetLinks(Vec<LinkSpec>),
    Quit,
}

pub enum PwEvent {
    Graph(Graph),
    /// How many of the wanted links are actually up, for the status line.
    Links { wanted: usize, made: usize },
    Error(String),
}

type Emit = Rc<dyn Fn(PwEvent)>;

pub struct PwHandle {
    tx: pw::channel::Sender<PwCmd>,
    join: Option<std::thread::JoinHandle<()>>,
}

impl PwHandle {
    pub fn send(&self, cmd: PwCmd) {
        let _ = self.tx.send(cmd);
    }
}

impl Drop for PwHandle {
    fn drop(&mut self) {
        let _ = self.tx.send(PwCmd::Quit);
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

/// Start the thread. `on_event` is called on the PipeWire thread, so it must
/// hand anything it wants to show back to the GUI loop itself.
pub fn start(on_event: impl Fn(PwEvent) + Send + 'static) -> PwHandle {
    let (tx, rx) = pw::channel::channel::<PwCmd>();
    let join = std::thread::Builder::new()
        .name("pipewire".into())
        .spawn(move || run(rx, Rc::new(on_event)))
        .expect("cannot spawn the PipeWire thread");
    PwHandle {
        tx,
        join: Some(join),
    }
}

// ---------------------------------------------------------------------------

#[derive(Default)]
struct NodeRec {
    name: String,
    description: String,
    media_class: String,
}

struct PortRec {
    node: u32,
    name: String,
    channel: Option<String>,
    dir: Dir,
    index: u32,
    audio: bool,
}

/// A link this app made and is responsible for.
struct Owned {
    spec: LinkSpec,
    proxy: pw::link::Link,
    /// The port ids it was made from, so a node restarting under the same
    /// name is noticed and the link remade against the new ports.
    ports: (u32, u32),
    made: Instant,
}

#[derive(Default)]
struct State {
    nodes: HashMap<u32, NodeRec>,
    ports: HashMap<u32, PortRec>,
    links: HashMap<u32, (u32, u32)>,
    desired: Vec<LinkSpec>,
    owned: Vec<Owned>,
    factory: Option<String>,
}

impl State {
    fn snapshot(&self) -> Graph {
        let mut by_node: HashMap<u32, Vec<Port>> = HashMap::new();
        for (id, p) in &self.ports {
            if !p.audio {
                continue;
            }
            by_node.entry(p.node).or_default().push(Port {
                id: *id,
                name: p.name.clone(),
                channel: p.channel.clone(),
                dir: p.dir,
                index: p.index,
            });
        }
        let mut nodes: Vec<Node> = self
            .nodes
            .iter()
            .filter(|(_, n)| !n.name.is_empty())
            .map(|(id, n)| Node {
                id: *id,
                name: n.name.clone(),
                description: if n.description.is_empty() {
                    n.name.clone()
                } else {
                    n.description.clone()
                },
                media_class: n.media_class.clone(),
                ports: by_node.remove(id).unwrap_or_default(),
            })
            .collect();
        nodes.sort_by_key(|n| n.id);
        Graph {
            nodes,
            connected: true,
        }
    }

    fn find_port(&self, node: &str, port: &str, dir: Dir) -> Option<u32> {
        let node_id = self
            .nodes
            .iter()
            .find(|(_, n)| n.name == node)
            .map(|(id, _)| *id)?;
        self.ports
            .iter()
            .find(|(_, p)| p.node == node_id && p.dir == dir && p.name == port)
            .map(|(id, _)| *id)
    }

    fn resolve(&self, spec: &LinkSpec) -> Option<(u32, u32)> {
        Some((
            self.find_port(&spec.out_node, &spec.out_port, Dir::Out)?,
            self.find_port(&spec.in_node, &spec.in_port, Dir::In)?,
        ))
    }
}

/// Reconcile the links that exist with the links that are wanted.
///
/// Runs on every tick that saw a change, so it has to be cheap and
/// idempotent.
fn reconcile(st: &mut State, core: &pw::core::CoreRc, emit: &Emit) {
    let factory = st.factory.clone().unwrap_or_else(|| "link-factory".into());

    // Drop what is no longer wanted, and anything whose ports moved or whose
    // link went away behind our back (a profile switch, another patchbay).
    let mut keep: Vec<Owned> = Vec::with_capacity(st.owned.len());
    for o in std::mem::take(&mut st.owned) {
        let wanted = st.desired.contains(&o.spec);
        let same_ports = st.resolve(&o.spec) == Some(o.ports);
        // The server has not necessarily registered a link we only just asked
        // for, so a young one is taken on trust rather than declared lost.
        let alive = o.made.elapsed() < SETTLE || st.links.values().any(|l| *l == o.ports);
        if wanted && same_ports && alive {
            keep.push(o);
        } else {
            let _ = core.destroy_object(o.proxy);
        }
    }
    st.owned = keep;

    let mut made = 0usize;
    for spec in st.desired.clone() {
        if st.owned.iter().any(|o| o.spec == spec) {
            made += 1;
            continue;
        }
        let Some(ports) = st.resolve(&spec) else { continue };

        // Someone else -- the session manager, a patchbay -- may already have
        // made exactly this link. Asking for it again is an error, and taking
        // it over would mean owning a link the app did not create.
        if st.links.values().any(|l| *l == ports) {
            made += 1;
            continue;
        }

        let mut props = pw::properties::PropertiesBox::new();
        props.insert("link.output.port", ports.0.to_string());
        props.insert("link.input.port", ports.1.to_string());
        props.insert("link.output.node", st.ports[&ports.0].node.to_string());
        props.insert("link.input.node", st.ports[&ports.1].node.to_string());
        // No object.linger: if this process dies, the links die with it.
        props.insert("object.linger", "false");

        match core.create_object::<pw::link::Link>(&factory, &*props) {
            Ok(proxy) => {
                st.owned.push(Owned {
                    spec,
                    proxy,
                    ports,
                    made: Instant::now(),
                });
                made += 1;
            }
            Err(e) => emit(PwEvent::Error(format!(
                "cannot link {}:{} to {}:{}: {e}",
                spec.out_node, spec.out_port, spec.in_node, spec.in_port
            ))),
        }
    }

    emit(PwEvent::Links {
        wanted: st.desired.len(),
        made,
    });
}

fn run(rx: pw::channel::Receiver<PwCmd>, emit: Emit) {
    pw::init();
    let mainloop = match pw::main_loop::MainLoopRc::new(None) {
        Ok(m) => m,
        Err(e) => {
            emit(PwEvent::Error(format!("cannot create a PipeWire loop: {e}")));
            return;
        }
    };

    // The wanted links outlive a reconnect: when the graph comes back they
    // are simply made again.
    let mut desired: Vec<LinkSpec> = Vec::new();
    let mut rx = Some(rx);

    loop {
        let core = pw::context::ContextRc::new(&mainloop, None)
            .ok()
            .and_then(|ctx| ctx.connect_rc(None).ok());

        let (next_rx, quit) = match core {
            Some(core) => session(&mainloop, &core, rx.take().unwrap(), &mut desired, &emit),
            None => {
                emit(PwEvent::Error(
                    "no PipeWire daemon; retrying every two seconds".into(),
                ));
                emit(PwEvent::Graph(Graph::default()));
                idle(&mainloop, rx.take().unwrap(), &mut desired, RETRY)
            }
        };
        rx = Some(next_rx);
        if quit {
            return;
        }
    }
}

/// Run one connection to the daemon. Returns the receiver and whether the app
/// asked to quit; a `false` means the connection dropped and is to be retried.
fn session(
    mainloop: &pw::main_loop::MainLoopRc,
    core: &pw::core::CoreRc,
    rx: pw::channel::Receiver<PwCmd>,
    desired: &mut Vec<LinkSpec>,
    emit: &Emit,
) -> (pw::channel::Receiver<PwCmd>, bool) {
    let registry = match core.get_registry_rc() {
        Ok(r) => r,
        Err(e) => {
            emit(PwEvent::Error(format!("no registry: {e}")));
            return idle(mainloop, rx, desired, RETRY);
        }
    };

    let st = Rc::new(RefCell::new(State {
        desired: std::mem::take(desired),
        ..State::default()
    }));
    // Two flags, not one. The graph changing and the wanted links changing
    // both need a reconcile, but only the first is news to the window --
    // and telling it otherwise would be a loop, since what the window does
    // with a new graph is work out the links again and send them back.
    let graph_moved = Rc::new(Cell::new(true));
    let links_moved = Rc::new(Cell::new(false));
    let quit = Rc::new(Cell::new(false));

    // A repeating tick rather than a one-shot armed from the callbacks: the
    // callbacks then only have to set a flag, which keeps them from having to
    // reach back into a timer that is created after them.
    let ticker = {
        let (st, core, emit) = (st.clone(), core.clone(), emit.clone());
        let (graph_moved, links_moved) = (graph_moved.clone(), links_moved.clone());
        mainloop.loop_().add_timer(move |_| {
            let graph = graph_moved.replace(false);
            if !graph && !links_moved.replace(false) {
                return;
            }
            let mut st = st.borrow_mut();
            reconcile(&mut st, &core, &emit);
            if graph {
                emit(PwEvent::Graph(st.snapshot()));
            }
        })
    };
    let _ = ticker.update_timer(Some(TICK), Some(TICK));

    let _reg_listener = registry
        .add_listener_local()
        .global({
            let (st, graph_moved) = (st.clone(), graph_moved.clone());
            move |g| {
                if take_global(&mut st.borrow_mut(), g) {
                    graph_moved.set(true);
                }
            }
        })
        .global_remove({
            let (st, graph_moved) = (st.clone(), graph_moved.clone());
            move |id| {
                let mut st = st.borrow_mut();
                let hit = st.nodes.remove(&id).is_some()
                    | st.ports.remove(&id).is_some()
                    | st.links.remove(&id).is_some();
                if hit {
                    graph_moved.set(true);
                }
            }
        })
        .register();

    let _core_listener = core
        .add_listener_local()
        .error({
            let mainloop = mainloop.downgrade();
            let emit = emit.clone();
            move |id, _seq, _res, message| {
                if id == pw::core::PW_ID_CORE {
                    // The connection itself is done for; the outer loop
                    // reconnects.
                    emit(PwEvent::Error(format!("PipeWire connection lost: {message}")));
                    if let Some(m) = mainloop.upgrade() {
                        m.quit();
                    }
                }
                // Anything else is one object failing, most often a link to a
                // node that went away mid-flight. The next tick sorts it out.
            }
        })
        .register();

    let attached = rx.attach(mainloop.loop_(), {
        let (st, quit, mainloop) = (st.clone(), quit.clone(), mainloop.clone());
        let links_moved = links_moved.clone();
        move |cmd| match cmd {
            PwCmd::SetLinks(links) => {
                st.borrow_mut().desired = links;
                links_moved.set(true);
            }
            PwCmd::Quit => {
                quit.set(true);
                mainloop.quit();
            }
        }
    });

    mainloop.run();
    let rx = attached.deattach();

    let mut st = st.borrow_mut();
    *desired = std::mem::take(&mut st.desired);
    // Tearing the proxies down explicitly keeps the server from having to
    // notice this client vanish while it is still connected.
    for o in std::mem::take(&mut st.owned) {
        let _ = core.destroy_object(o.proxy);
    }
    drop(st);

    let quit = quit.get();
    if !quit {
        emit(PwEvent::Graph(Graph::default()));
        return idle(mainloop, rx, desired, RETRY);
    }
    (rx, true)
}

/// Returns true if this global changed anything worth a reconcile.
fn take_global(
    st: &mut State,
    g: &pw::registry::GlobalObject<&libspa::utils::dict::DictRef>,
) -> bool {
    let p = g.props;
    match g.type_ {
        ObjectType::Node => {
            st.nodes.insert(
                g.id,
                NodeRec {
                    name: prop(p, "node.name").unwrap_or_default(),
                    description: prop(p, "node.description")
                        .or_else(|| prop(p, "node.nick"))
                        .unwrap_or_default(),
                    media_class: prop(p, "media.class").unwrap_or_default(),
                },
            );
            true
        }
        ObjectType::Port => {
            let Some(node) = prop(p, "node.id").and_then(|s| s.parse().ok()) else {
                return false;
            };
            // Ports of a MIDI or video node are not something this app can
            // link, and listing them would only clutter the pickers.
            let dsp = prop(p, "format.dsp").unwrap_or_default();
            let audio = if dsp.is_empty() {
                st.nodes
                    .get(&node)
                    .is_some_and(|n| n.media_class.contains("Audio"))
            } else {
                dsp.contains("audio")
            };
            st.ports.insert(
                g.id,
                PortRec {
                    node,
                    name: prop(p, "port.name").unwrap_or_default(),
                    channel: prop(p, "audio.channel"),
                    dir: match prop(p, "port.direction").as_deref() {
                        Some("in") => Dir::In,
                        _ => Dir::Out,
                    },
                    index: prop(p, "port.id").and_then(|s| s.parse().ok()).unwrap_or(0),
                    audio,
                },
            );
            true
        }
        ObjectType::Link => {
            let out = prop(p, "link.output.port").and_then(|s| s.parse().ok());
            let inp = prop(p, "link.input.port").and_then(|s| s.parse().ok());
            match (out, inp) {
                (Some(o), Some(i)) => {
                    st.links.insert(g.id, (o, i));
                    true
                }
                _ => false,
            }
        }
        ObjectType::Factory => {
            if prop(p, "factory.type.name").as_deref() == Some(ObjectType::Link.to_str()) {
                st.factory = prop(p, "factory.name");
            }
            false
        }
        _ => false,
    }
}

/// Run the loop for `delay` while still accepting commands, so waiting to
/// reconnect does not make the GUI feel wedged.
fn idle(
    mainloop: &pw::main_loop::MainLoopRc,
    rx: pw::channel::Receiver<PwCmd>,
    desired: &mut Vec<LinkSpec>,
    delay: Duration,
) -> (pw::channel::Receiver<PwCmd>, bool) {
    let quit = Rc::new(Cell::new(false));
    let pending: Rc<RefCell<Option<Vec<LinkSpec>>>> = Rc::new(RefCell::new(None));

    let timer = mainloop.loop_().add_timer({
        let weak = mainloop.downgrade();
        move |_| {
            if let Some(m) = weak.upgrade() {
                m.quit();
            }
        }
    });
    let _ = timer.update_timer(Some(delay), None);

    let attached = rx.attach(mainloop.loop_(), {
        let (quit, pending, mainloop) = (quit.clone(), pending.clone(), mainloop.clone());
        move |cmd| match cmd {
            PwCmd::SetLinks(links) => *pending.borrow_mut() = Some(links),
            PwCmd::Quit => {
                quit.set(true);
                mainloop.quit();
            }
        }
    });
    mainloop.run();
    let rx = attached.deattach();

    if let Some(l) = pending.borrow_mut().take() {
        *desired = l;
    }
    (rx, quit.get())
}

fn prop(props: Option<&libspa::utils::dict::DictRef>, key: &str) -> Option<String> {
    props?.get(key).map(|s| s.to_string())
}
