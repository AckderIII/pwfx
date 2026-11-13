//! The PipeWire side of the app, which lives on its own thread.
//!
//! libpipewire's loop is not thread safe and its objects are not `Send`, so
//! everything that touches it stays here: the registry, the graph model built
//! from it, and the nodes and links the app owns. The rest of the program
//! talks to this thread in two directions and in terms of *names* only --
//!
//!   in   `PwCmd::SetNodes`, the buses that ought to exist,
//!        `PwCmd::SetLinks`, the set of links that ought to exist, as node
//!        and port names, and
//!        `PwCmd::SetRoutes`, which applications' streams ought to go to
//!        which bus;
//!   out  `PwEvent::Graph`, a snapshot of what is out there.
//!
//! Both are the same shape of command and are reconciled the same way: nodes
//! first, because a link can only be made once the ports at both ends of it
//! exist. Nothing waits for that, though -- the bus's ports arriving in the
//! registry is what wakes the next pass, and that pass makes the link.
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
//! One object is bound rather than just read off the registry: the `default`
//! metadata, which is where the session manager says what the default input
//! and output are. Its contents are not global properties, so they only
//! arrive through a listener; they go out in the snapshot alongside the
//! nodes, and a change to them wakes the same tick a node appearing does.
//!
//! It is also how an application's stream is routed. Linking a stream by
//! hand would mean tearing down the links the session manager made for it
//! and fighting it every time it relinks; instead `target.object` is set on
//! the stream in that metadata, which is what pavucontrol does, and the
//! session manager moves it. The value is the target's `node.name`, not its
//! serial: names survive the target being remade, and WirePlumber only
//! *remembers* a target given as a serial, so one given by name is not kept
//! for the application once this app has stopped asking for it.
//!
//! Nodes and links alike are held as proxies without `object.linger`, so if
//! this process dies the server tears them down with it and the graph is left
//! as it was found. A bus is therefore only there while the app is.
//!
//! The same goes for removing one on purpose: dropping the proxy *is* the
//! destroy. `pw_proxy_destroy`, which the drop calls, asks the server to
//! destroy the object itself, so calling `Core::destroy_object` first asks
//! twice -- and the second request, for a resource that is already gone,
//! comes back as an error on the core.

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

/// One node the app asks the daemon to make for it: a bus.
///
/// Everything about a null audio sink is fixed when it is created -- how many
/// channels it has, what they are called, which side of it other applications
/// are offered -- so there is nothing to change afterwards and no live half
/// to this. Equality is therefore the whole comparison: a spec that differs
/// in any field is a different node, and the reconcile destroys the old one
/// and creates the new.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct NodeSpec {
    /// `node.name`: what chains and `pw-link` address it by.
    pub name: String,
    /// `node.description`: what other applications show in their device list.
    pub description: String,
    pub media_class: String,
    /// `audio.position`, which fixes the channel count and the port names
    /// together.
    pub position: Vec<String>,
    /// Whether the node is kept from driving the graph.
    pub passive: bool,
}

/// Applications' streams the app wants on a node: every audio stream whose
/// `application.name` is `app` -- and, with `stream`, whose stream name
/// (`media.name`, else `node.name`) is that -- and that records (or plays,
/// per `record`) is sent to `target`. A route with a `stream` wins over one
/// without for the streams it names.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct RouteSpec {
    pub app: String,
    pub stream: Option<String>,
    pub record: bool,
    pub target: String,
}

pub enum PwCmd {
    /// The complete set of nodes the app wants to exist. Anything it owns
    /// that is not in here is destroyed.
    SetNodes(Vec<NodeSpec>),
    /// The complete set of links the app wants to exist. Anything it owns
    /// that is not in here is torn down.
    SetLinks(Vec<LinkSpec>),
    /// The complete set of routes the app wants. A stream it moved that is
    /// no longer covered by one is let go.
    SetRoutes(Vec<RouteSpec>),
    Quit,
}

pub enum PwEvent {
    Graph(Graph),
    /// How many of the wanted links are actually up, for the status line.
    Links { wanted: usize, made: usize },
    Error(String),
}

/// Everything the app has declared should exist. Held apart from any one
/// connection, so that a daemon restart replays it rather than losing it.
#[derive(Default)]
struct Wanted {
    nodes: Vec<NodeSpec>,
    links: Vec<LinkSpec>,
    routes: Vec<RouteSpec>,
}

type Emit = Rc<dyn Fn(PwEvent)>;

pub struct PwHandle {
    tx: pw::channel::Sender<PwCmd>,
    join: Option<std::thread::JoinHandle<()>>,
    /// The sets last declared, so an unchanged one is not sent again.
    last_nodes: RefCell<Vec<NodeSpec>>,
    last_links: RefCell<Vec<LinkSpec>>,
    last_routes: RefCell<Vec<RouteSpec>>,
}

impl PwHandle {
    /// Declare the buses that ought to exist.
    ///
    /// Only sent when it has actually moved: dragging a slider reconciles
    /// twenty times a second and would otherwise wake the PipeWire thread
    /// just as often to be told nothing changed.
    pub fn declare_nodes(&self, nodes: Vec<NodeSpec>) {
        if *self.last_nodes.borrow() != nodes {
            *self.last_nodes.borrow_mut() = nodes.clone();
            let _ = self.tx.send(PwCmd::SetNodes(nodes));
        }
    }

    /// Declare the links that ought to exist, on the same terms.
    pub fn declare_links(&self, links: Vec<LinkSpec>) {
        if *self.last_links.borrow() != links {
            *self.last_links.borrow_mut() = links.clone();
            let _ = self.tx.send(PwCmd::SetLinks(links));
        }
    }

    /// Declare which applications' streams ought to go where.
    pub fn declare_routes(&self, routes: Vec<RouteSpec>) {
        if *self.last_routes.borrow() != routes {
            *self.last_routes.borrow_mut() = routes.clone();
            let _ = self.tx.send(PwCmd::SetRoutes(routes));
        }
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
        last_nodes: RefCell::new(Vec::new()),
        last_links: RefCell::new(Vec::new()),
        last_routes: RefCell::new(Vec::new()),
    }
}

// ---------------------------------------------------------------------------

#[derive(Default)]
struct NodeRec {
    name: String,
    description: String,
    media_class: String,
    app: Option<String>,
    /// `media.name`, which on an application's stream is often the only
    /// thing that tells its streams apart. It is not in the registry's
    /// announcement -- only a few properties are -- so an application's
    /// stream is bound and this comes from its info.
    media_name: Option<String>,
    /// Whether that info has arrived. Until it has, the stream is not
    /// routed: a rule for the whole application would otherwise take a
    /// stream that a rule for that one stream is about to claim.
    known: bool,
    /// `node.dont-move`: the stream asked not to be moved, and is not.
    dont_move: bool,
    /// `object.serial`, which unlike the id is never handed out twice. A
    /// stream that ends and another that starts within one tick can share
    /// an id, and only this tells them apart.
    serial: u64,
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

/// A node this app asked the daemon to create and is responsible for.
struct OwnedNode {
    spec: NodeSpec,
    /// `None` when the creation was refused. The spec is kept anyway so the
    /// attempt is not repeated on every tick for as long as the app is open;
    /// editing the bus or reconnecting to the daemon is what clears it.
    proxy: Option<pw::node::Node>,
}

#[derive(Default)]
struct State {
    nodes: HashMap<u32, NodeRec>,
    ports: HashMap<u32, PortRec>,
    links: HashMap<u32, (u32, u32)>,
    desired: Vec<LinkSpec>,
    owned: Vec<Owned>,
    desired_nodes: Vec<NodeSpec>,
    owned_nodes: Vec<OwnedNode>,
    factory: Option<String>,
    /// The registry id of the `default` metadata, so its removal -- the
    /// session manager restarting -- can be told apart from anything else's.
    metadata: Option<u32>,
    default_source: Option<String>,
    default_sink: Option<String>,
    desired_routes: Vec<RouteSpec>,
    /// `target.object` as the metadata has it now, per node id.
    targets: HashMap<u32, String>,
    /// The streams this app pointed somewhere, by id and serial, and where.
    /// A stream is only pointed once per target: if the user then moves it
    /// themselves, that stands, and the rule takes it back only if it
    /// changes.
    routed: HashMap<u32, (u64, String)>,
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
                app: n.app.clone(),
                media_name: n.media_name.clone(),
                ports: by_node.remove(id).unwrap_or_default(),
            })
            .collect();
        nodes.sort_by_key(|n| n.id);
        Graph {
            nodes,
            connected: true,
            default_source: self.default_source.clone(),
            default_sink: self.default_sink.clone(),
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

/// Reconcile the nodes this app owns with the ones it wants.
///
/// A plain set difference, unlike the links: a node has no ports to resolve
/// and nothing to wait for, so what is not there is created and what is no
/// longer asked for is destroyed. Its ports then appear in the registry,
/// which is what brings the link reconcile back round to finish the job.
fn reconcile_nodes(st: &mut State, core: &pw::core::CoreRc, emit: &Emit) {
    let mut keep: Vec<OwnedNode> = Vec::with_capacity(st.owned_nodes.len());
    for o in std::mem::take(&mut st.owned_nodes) {
        if st.desired_nodes.contains(&o.spec) {
            keep.push(o);
        } else {
            drop(o.proxy);
        }
    }
    st.owned_nodes = keep;

    for spec in st.desired_nodes.clone() {
        if st.owned_nodes.iter().any(|o| o.spec == spec) {
            continue;
        }

        let mut props = pw::properties::PropertiesBox::new();
        props.insert("factory.name", "support.null-audio-sink");
        props.insert("node.name", &*spec.name);
        props.insert("node.description", &*spec.description);
        props.insert("media.class", &*spec.media_class);
        props.insert("audio.position", format!("[ {} ]", spec.position.join(", ")));
        // The monitor has to carry what went in, not a volume-scaled copy of
        // it: a bus that quietly changed the level of everything passing
        // through it would be a strange thing to put in the middle of a
        // chain of filters that are careful about exactly that.
        props.insert("monitor.channel-volumes", "false");
        props.insert("monitor.passthrough", "true");
        props.insert("device.class", "sound");
        if spec.passive {
            props.insert("node.passive", "true");
        }
        // No object.linger, same as the links: a bus belongs to this process
        // and goes when it does, rather than being left behind in the graph
        // for someone to wonder about.
        props.insert("object.linger", "false");

        let proxy = match core.create_object::<pw::node::Node>("adapter", &*props) {
            Ok(proxy) => Some(proxy),
            Err(e) => {
                emit(PwEvent::Error(format!(
                    "cannot create the bus {}: {e}",
                    spec.name
                )));
                None
            }
        };
        st.owned_nodes.push(OwnedNode { spec, proxy });
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
            drop(o.proxy);
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

    // What the app has declared outlives a reconnect: when the daemon comes
    // back the buses are created again and the links made again.
    let mut wanted = Wanted::default();
    let mut rx = Some(rx);

    loop {
        let core = pw::context::ContextRc::new(&mainloop, None)
            .ok()
            .and_then(|ctx| ctx.connect_rc(None).ok());

        let (next_rx, quit) = match core {
            Some(core) => session(&mainloop, &core, rx.take().unwrap(), &mut wanted, &emit),
            None => {
                emit(PwEvent::Error(
                    "no PipeWire daemon; retrying every two seconds".into(),
                ));
                emit(PwEvent::Graph(Graph::default()));
                idle(&mainloop, rx.take().unwrap(), &mut wanted, RETRY)
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
    wanted: &mut Wanted,
    emit: &Emit,
) -> (pw::channel::Receiver<PwCmd>, bool) {
    let registry = match core.get_registry_rc() {
        Ok(r) => r,
        Err(e) => {
            emit(PwEvent::Error(format!("no registry: {e}")));
            return idle(mainloop, rx, wanted, RETRY);
        }
    };

    let st = Rc::new(RefCell::new(State {
        desired: std::mem::take(&mut wanted.links),
        desired_nodes: std::mem::take(&mut wanted.nodes),
        desired_routes: std::mem::take(&mut wanted.routes),
        ..State::default()
    }));
    // Two flags, not one. The graph changing and what the app wants changing
    // both need a reconcile, but only the first is news to the window --
    // and telling it otherwise would be a loop, since what the window does
    // with a new graph is work out the links again and send them back.
    let graph_moved = Rc::new(Cell::new(true));
    let links_moved = Rc::new(Cell::new(false));
    let quit = Rc::new(Cell::new(false));

    // The bound `default` metadata and its listener, which have to be kept
    // alive for as long as the defaults are to keep arriving -- and which
    // the routes are written through.
    let metadata: Rc<RefCell<Option<(pw::metadata::Metadata, pw::metadata::MetadataListener)>>> =
        Rc::new(RefCell::new(None));
    // Applications' streams, bound for their `media.name` (`watch_stream`).
    // Kept out of `State` because each listener holds the state.
    let streams: Rc<RefCell<HashMap<u32, (pw::node::Node, pw::node::NodeListener)>>> =
        Rc::default();

    // A repeating tick rather than a one-shot armed from the callbacks: the
    // callbacks then only have to set a flag, which keeps them from having to
    // reach back into a timer that is created after them.
    let ticker = {
        let (st, core, emit) = (st.clone(), core.clone(), emit.clone());
        let (graph_moved, links_moved) = (graph_moved.clone(), links_moved.clone());
        let metadata = metadata.clone();
        mainloop.loop_().add_timer(move |_| {
            let graph = graph_moved.replace(false);
            if !graph && !links_moved.replace(false) {
                return;
            }
            let mut st = st.borrow_mut();
            // Nodes first: a link into a bus can only be made once the bus
            // has ports, and this is the pass that gives it them.
            reconcile_nodes(&mut st, &core, &emit);
            reconcile(&mut st, &core, &emit);
            // Last: a stream can only be pointed at a bus once the bus is
            // in the registry, and the node pass is what puts it there.
            if let Some((meta, _)) = metadata.borrow().as_ref() {
                reconcile_routes(&mut st, meta);
            }
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
            let (registry, metadata) = (registry.downgrade(), metadata.clone());
            let streams = streams.clone();
            move |g| {
                if g.type_ == ObjectType::Metadata {
                    if prop(g.props, "metadata.name").as_deref() == Some("default") {
                        if let Some(registry) = registry.upgrade() {
                            *metadata.borrow_mut() =
                                watch_defaults(&registry, g, &st, &graph_moved);
                        }
                    }
                    return;
                }
                if take_global(&mut st.borrow_mut(), g) {
                    graph_moved.set(true);
                }
                if g.type_ == ObjectType::Node && is_app_stream(g.props) {
                    let watched = registry
                        .upgrade()
                        .and_then(|r| watch_stream(&r, g, &st, &graph_moved));
                    match watched {
                        Some(w) => {
                            streams.borrow_mut().insert(g.id, w);
                        }
                        // Not bindable: route it by node.name rather than
                        // never.
                        None => {
                            if let Some(n) = st.borrow_mut().nodes.get_mut(&g.id) {
                                n.known = true;
                            }
                        }
                    }
                }
            }
        })
        .global_remove({
            let (st, graph_moved) = (st.clone(), graph_moved.clone());
            let (metadata, streams) = (metadata.clone(), streams.clone());
            move |id| {
                streams.borrow_mut().remove(&id);
                let mut st = st.borrow_mut();
                let mut hit = st.nodes.remove(&id).is_some()
                    | st.ports.remove(&id).is_some()
                    | st.links.remove(&id).is_some();
                if st.metadata == Some(id) {
                    // The session manager went away. Until it is back there
                    // is no default, and a chain on one is unplugged.
                    st.metadata = None;
                    st.default_source = None;
                    st.default_sink = None;
                    metadata.borrow_mut().take();
                    hit = true;
                }
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
            move |id, _seq, res, message| {
                // Only a broken pipe is the connection itself going. Other
                // errors arrive on the core too -- one about a resource the
                // server no longer knows is reported there, having nowhere
                // else to go -- and treating those as fatal tore every bus
                // and link down to rebuild them.
                if id == pw::core::PW_ID_CORE && res == -libc::EPIPE {
                    // The connection itself is done for; the outer loop
                    // reconnects.
                    emit(PwEvent::Error(format!("PipeWire connection lost: {message}")));
                    if let Some(m) = mainloop.upgrade() {
                        m.quit();
                    }
                } else if id == pw::core::PW_ID_CORE {
                    // Worth saying, not worth reconnecting over.
                    emit(PwEvent::Error(format!("PipeWire: {message}")));
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
            PwCmd::SetNodes(nodes) => {
                st.borrow_mut().desired_nodes = nodes;
                links_moved.set(true);
            }
            PwCmd::SetRoutes(routes) => {
                st.borrow_mut().desired_routes = routes;
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
    wanted.links = std::mem::take(&mut st.desired);
    wanted.nodes = std::mem::take(&mut st.desired_nodes);
    wanted.routes = std::mem::take(&mut st.desired_routes);
    // Hand back every stream this app pointed somewhere, on the same terms
    // as a rule going away: only where the metadata still says what was
    // written. Unlike a link, a metadata entry is not owned by this client
    // and does not go when it does, so it has to be taken back while still
    // connected -- and waited for, or the disconnect can overtake it.
    if quit.get() {
        if let Some((meta, _)) = metadata.borrow().as_ref() {
            let State { nodes, targets, routed, .. } = &mut *st;
            let mut wrote = false;
            for (id, target) in plan_routes(nodes, &[], targets, routed) {
                write_route(meta, id, target.as_deref());
                wrote = true;
            }
            if wrote {
                roundtrip(mainloop, core);
            }
        }
    }
    // Tearing the proxies down explicitly keeps the server from having to
    // notice this client vanish while it is still connected. Links before
    // nodes, so nothing is carrying audio out of a bus as it goes.
    drop(std::mem::take(&mut st.owned));
    drop(std::mem::take(&mut st.owned_nodes));
    drop(st);

    let quit = quit.get();
    if !quit {
        emit(PwEvent::Graph(Graph::default()));
        return idle(mainloop, rx, wanted, RETRY);
    }
    (rx, true)
}

/// Work out which streams to point where, and which to let go of.
///
/// Returns `(node id, target)` pairs to write to the metadata, `None` meaning
/// "delete the key". Pure, so it can be tested without a daemon; `routed` is
/// updated to what will be true once they are written.
///
/// A stream is pointed at its route's target once -- when it appears, when
/// its rule appears, or when the target does. After that it is left alone:
/// a user who moves it somewhere else in pavucontrol has made a decision,
/// and re-asserting the rule over it on every tick would be fighting them.
/// Letting go is just as careful: the key is deleted only if it still says
/// what this app wrote, so a stream someone has since moved stays moved.
fn plan_routes(
    nodes: &HashMap<u32, NodeRec>,
    desired: &[RouteSpec],
    targets: &HashMap<u32, String>,
    routed: &mut HashMap<u32, (u64, String)>,
) -> Vec<(u32, Option<String>)> {
    let mut want: HashMap<u32, (u64, &str)> = HashMap::new();
    for (id, n) in nodes {
        let Some(app) = n.app.as_deref() else { continue };
        let record = match n.media_class.as_str() {
            "Stream/Output/Audio" => false,
            "Stream/Input/Audio" => true,
            _ => continue,
        };
        if n.dont_move || !n.known {
            continue;
        }
        let stream = n.media_name.as_deref().unwrap_or(&n.name);
        let going = |r: &&RouteSpec| r.app == app && r.record == record;
        let Some(r) = desired
            .iter()
            .filter(going)
            .find(|r| r.stream.as_deref() == Some(stream))
            .or_else(|| desired.iter().filter(going).find(|r| r.stream.is_none()))
        else {
            continue;
        };
        // A target that is not there yet -- a bus still being made, or
        // switched off -- leaves the stream where the session manager put it
        // rather than pointing it at nothing. Its arrival is a registry
        // change, which is the next pass, which points it.
        if nodes.values().any(|t| t.name == r.target) {
            want.insert(*id, (n.serial, &r.target));
        }
    }

    let mut acts = Vec::new();
    routed.retain(|id, (serial, target)| {
        if want.get(id) == Some(&(*serial, target.as_str())) {
            return true;
        }
        // A stream that has gone takes its metadata with it, and so does
        // one whose id now belongs to a new stream.
        let same = nodes.get(id).is_some_and(|n| n.serial == *serial);
        if same && targets.get(id) == Some(target) {
            acts.push((*id, None));
        }
        false
    });
    for (id, (serial, target)) in want {
        if !routed.contains_key(&id) {
            routed.insert(id, (serial, target.to_string()));
            acts.push((id, Some(target.to_string())));
        }
    }
    acts.sort();
    acts
}

fn write_route(meta: &pw::metadata::Metadata, id: u32, target: Option<&str>) {
    match target {
        Some(t) => meta.set_property(id, "target.object", Some("Spa:String"), Some(t)),
        None => meta.set_property(id, "target.object", None, None),
    }
}

fn reconcile_routes(st: &mut State, meta: &pw::metadata::Metadata) {
    let State { nodes, desired_routes, targets, routed, .. } = st;
    for (id, target) in plan_routes(nodes, desired_routes, targets, routed) {
        write_route(meta, id, target.as_deref());
    }
}

/// Bind the `default` metadata and follow the two keys this app cares about.
fn watch_defaults(
    registry: &pw::registry::RegistryRc,
    g: &pw::registry::GlobalObject<&libspa::utils::dict::DictRef>,
    st: &Rc<RefCell<State>>,
    graph_moved: &Rc<Cell<bool>>,
) -> Option<(pw::metadata::Metadata, pw::metadata::MetadataListener)> {
    let proxy: pw::metadata::Metadata = registry.bind(g).ok()?;
    st.borrow_mut().metadata = Some(g.id);
    let listener = proxy
        .add_listener_local()
        .property({
            let (st, graph_moved) = (st.clone(), graph_moved.clone());
            move |subject, key, _type, value| {
                let mut st = st.borrow_mut();
                if subject != pw::core::PW_ID_CORE {
                    // Where each stream is pointed, which the route
                    // reconcile compares against before letting one go.
                    match (key, value) {
                        (Some("target.object"), Some(v)) => {
                            st.targets.insert(subject, v.to_string());
                        }
                        (Some("target.object"), None) | (None, _) => {
                            st.targets.remove(&subject);
                        }
                        _ => {}
                    }
                    return 0;
                }
                let new = value.and_then(default_name);
                let slot = match key {
                    Some("default.audio.source") => &mut st.default_source,
                    Some("default.audio.sink") => &mut st.default_sink,
                    Some(_) => return 0,
                    // No key: every property was cleared at once.
                    None => {
                        st.default_source = None;
                        st.default_sink = None;
                        graph_moved.set(true);
                        return 0;
                    }
                };
                if *slot != new {
                    *slot = new;
                    graph_moved.set(true);
                }
                0
            }
        })
        .register();
    Some((proxy, listener))
}

/// The node name out of a default's value, which is JSON:
/// `{"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"}`.
fn default_name(value: &str) -> Option<String> {
    let v: serde_json::Value = serde_json::from_str(value).ok()?;
    Some(v.get("name")?.as_str()?.to_string())
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
                    app: prop(p, "application.name"),
                    media_name: prop(p, "media.name"),
                    known: !is_app_stream(p),
                    dont_move: prop(p, "node.dont-move").as_deref() == Some("true"),
                    serial: prop(p, "object.serial").and_then(|s| s.parse().ok()).unwrap_or(0),
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

/// Wait until the server has seen everything sent so far, or a second has
/// passed: a sync is answered only after every message before it.
fn roundtrip(mainloop: &pw::main_loop::MainLoopRc, core: &pw::core::CoreRc) {
    let Ok(pending) = core.sync(0) else { return };
    let _done = core
        .add_listener_local()
        .done({
            let weak = mainloop.downgrade();
            move |id, seq| {
                if id == pw::core::PW_ID_CORE && seq == pending {
                    if let Some(m) = weak.upgrade() {
                        m.quit();
                    }
                }
            }
        })
        .register();
    let timeout = mainloop.loop_().add_timer({
        let weak = mainloop.downgrade();
        move |_| {
            if let Some(m) = weak.upgrade() {
                m.quit();
            }
        }
    });
    let _ = timeout.update_timer(Some(Duration::from_secs(1)), None);
    mainloop.run();
}

/// Run the loop for `delay` while still accepting commands, so waiting to
/// reconnect does not make the GUI feel wedged.
fn idle(
    mainloop: &pw::main_loop::MainLoopRc,
    rx: pw::channel::Receiver<PwCmd>,
    wanted: &mut Wanted,
    delay: Duration,
) -> (pw::channel::Receiver<PwCmd>, bool) {
    let quit = Rc::new(Cell::new(false));
    let pending: Rc<RefCell<Wanted>> = Rc::new(RefCell::new(Wanted::default()));
    // Which of nodes, links and routes arrived.
    let moved = Rc::new(Cell::new((false, false, false)));

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
        let moved = moved.clone();
        move |cmd| match cmd {
            PwCmd::SetLinks(links) => {
                pending.borrow_mut().links = links;
                let (n, _, r) = moved.get();
                moved.set((n, true, r));
            }
            PwCmd::SetNodes(nodes) => {
                pending.borrow_mut().nodes = nodes;
                let (_, l, r) = moved.get();
                moved.set((true, l, r));
            }
            PwCmd::SetRoutes(routes) => {
                pending.borrow_mut().routes = routes;
                let (n, l, _) = moved.get();
                moved.set((n, l, true));
            }
            PwCmd::Quit => {
                quit.set(true);
                mainloop.quit();
            }
        }
    });
    mainloop.run();
    let rx = attached.deattach();

    // Only what actually arrived replaces what is held: an app that edited a
    // chain while the daemon was away must not lose its buses with it.
    let (nodes_moved, links_moved, routes_moved) = moved.get();
    let mut pending = pending.borrow_mut();
    if nodes_moved {
        wanted.nodes = std::mem::take(&mut pending.nodes);
    }
    if links_moved {
        wanted.links = std::mem::take(&mut pending.links);
    }
    if routes_moved {
        wanted.routes = std::mem::take(&mut pending.routes);
    }
    (rx, quit.get())
}

/// Whether a node is an application's audio stream, the kind a route can
/// move and that is bound for its `media.name`.
fn is_app_stream(props: Option<&libspa::utils::dict::DictRef>) -> bool {
    matches!(
        props.and_then(|p| p.get("media.class")),
        Some("Stream/Output/Audio" | "Stream/Input/Audio")
    )
}

/// Bind an application's stream for the properties the registry leaves
/// out, and keep its `media.name` in the node's record as it changes.
fn watch_stream(
    registry: &pw::registry::RegistryRc,
    g: &pw::registry::GlobalObject<&libspa::utils::dict::DictRef>,
    st: &Rc<RefCell<State>>,
    graph_moved: &Rc<Cell<bool>>,
) -> Option<(pw::node::Node, pw::node::NodeListener)> {
    let proxy: pw::node::Node = registry.bind(g).ok()?;
    let id = g.id;
    let listener = proxy
        .add_listener_local()
        .info({
            let (st, graph_moved) = (st.clone(), graph_moved.clone());
            move |info| {
                let mut st = st.borrow_mut();
                let Some(n) = st.nodes.get_mut(&id) else { return };
                let name = prop(info.props(), "media.name").or_else(|| n.media_name.take());
                if !n.known || n.media_name != name {
                    n.media_name = name;
                    n.known = true;
                    graph_moved.set(true);
                }
            }
        })
        .register();
    Some((proxy, listener))
}

fn prop(props: Option<&libspa::utils::dict::DictRef>, key: &str) -> Option<String> {
    props?.get(key).map(|s| s.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_default_names_its_node_in_json() {
        assert_eq!(
            default_name(r#"{"name":"alsa_output.usb-Focusrite.HiFi__Line1__sink"}"#).as_deref(),
            Some("alsa_output.usb-Focusrite.HiFi__Line1__sink")
        );
        assert_eq!(default_name("not json"), None);
        assert_eq!(default_name(r#"{"other":1}"#), None);
    }

    fn rec(name: &str, class: &str, app: Option<&str>, serial: u64) -> NodeRec {
        NodeRec {
            name: name.into(),
            description: String::new(),
            media_class: class.into(),
            app: app.map(Into::into),
            media_name: None,
            known: true,
            dont_move: false,
            serial,
        }
    }

    /// A sink bus, a source bus, and Firefox playing and recording.
    fn graph() -> HashMap<u32, NodeRec> {
        HashMap::from([
            (10, rec("pwfx.bus.music", "Audio/Sink", None, 100)),
            (11, rec("pwfx.bus.mic", "Audio/Source/Virtual", None, 101)),
            (20, rec("firefox-out", "Stream/Output/Audio", Some("Firefox"), 200)),
            (21, rec("firefox-in", "Stream/Input/Audio", Some("Firefox"), 201)),
            (22, rec("mpv", "Stream/Output/Audio", Some("mpv"), 202)),
        ])
    }

    fn route(app: &str, record: bool, target: &str) -> RouteSpec {
        RouteSpec { app: app.into(), stream: None, record, target: target.into() }
    }

    fn stream_route(app: &str, stream: &str, record: bool, target: &str) -> RouteSpec {
        RouteSpec { stream: Some(stream.into()), ..route(app, record, target) }
    }

    #[test]
    fn a_route_for_one_stream_takes_only_that_one_and_wins() {
        // OBS as it really appears: every stream named "OBS", told apart by
        // media.name only.
        let mut nodes = graph();
        let mut obs = |id, media: &str, serial| {
            let mut r = rec("OBS", "Stream/Input/Audio", Some("OBS"), serial);
            r.media_name = Some(media.into());
            nodes.insert(id, r);
        };
        obs(30, "Desktop-Audio", 300);
        obs(31, "Mic/Aux", 301);
        nodes.insert(12, rec("pwfx.bus.other", "Audio/Source/Virtual", None, 102));

        let only = [stream_route("OBS", "Mic/Aux", true, "pwfx.bus.mic")];
        assert_eq!(plan_routes(&nodes, &only, &HashMap::new(), &mut HashMap::new()),
                   [set(31, "pwfx.bus.mic")]);

        // With the whole application on another bus, the named stream still
        // goes its own way, whichever rule comes first.
        let both = [route("OBS", true, "pwfx.bus.other"),
                    stream_route("OBS", "Mic/Aux", true, "pwfx.bus.mic")];
        assert_eq!(plan_routes(&nodes, &both, &HashMap::new(), &mut HashMap::new()),
                   [set(30, "pwfx.bus.other"), set(31, "pwfx.bus.mic")]);

        // A stream without a media.name is known by its node.name.
        let by_node = [stream_route("Firefox", "firefox-in", true, "pwfx.bus.mic")];
        assert_eq!(plan_routes(&nodes, &by_node, &HashMap::new(), &mut HashMap::new()),
                   [set(21, "pwfx.bus.mic")]);
    }

    fn set(id: u32, t: &str) -> (u32, Option<String>) {
        (id, Some(t.into()))
    }

    #[test]
    fn a_route_points_only_the_streams_going_its_way() {
        let nodes = graph();
        let mut routed = HashMap::new();
        let routes = [route("Firefox", false, "pwfx.bus.music")];
        // Firefox's playback, not its recording, and not mpv.
        assert_eq!(plan_routes(&nodes, &routes, &HashMap::new(), &mut routed),
                   [set(20, "pwfx.bus.music")]);
        // Once: the next pass has nothing to do.
        assert!(plan_routes(&nodes, &routes, &HashMap::new(), &mut routed).is_empty());

        let mut routed = HashMap::new();
        let routes = [route("Firefox", true, "pwfx.bus.mic")];
        assert_eq!(plan_routes(&nodes, &routes, &HashMap::new(), &mut routed),
                   [set(21, "pwfx.bus.mic")]);
    }

    #[test]
    fn a_stream_moved_by_hand_stays_moved() {
        let nodes = graph();
        let routes = [route("Firefox", false, "pwfx.bus.music")];
        let mut routed = HashMap::new();
        plan_routes(&nodes, &routes, &HashMap::new(), &mut routed);

        // The user sent it to the speakers in pavucontrol.
        let targets = HashMap::from([(20, "speakers".to_string())]);
        assert!(plan_routes(&nodes, &routes, &targets, &mut routed).is_empty(),
                "the rule is not re-asserted over them");
        // And dropping the rule does not undo their choice either.
        assert!(plan_routes(&nodes, &[], &targets, &mut routed).is_empty());
        assert!(routed.is_empty());
    }

    #[test]
    fn letting_go_deletes_only_what_this_app_wrote() {
        let nodes = graph();
        let routes = [route("Firefox", false, "pwfx.bus.music")];
        let mut routed = HashMap::new();
        plan_routes(&nodes, &routes, &HashMap::new(), &mut routed);
        let targets = HashMap::from([(20, "pwfx.bus.music".to_string())]);
        assert_eq!(plan_routes(&nodes, &[], &targets, &mut routed), [(20, None)]);
    }

    #[test]
    fn a_target_that_is_not_there_yet_is_waited_for() {
        let mut nodes = graph();
        nodes.remove(&10);
        let routes = [route("Firefox", false, "pwfx.bus.music")];
        let mut routed = HashMap::new();
        assert!(plan_routes(&nodes, &routes, &HashMap::new(), &mut routed).is_empty());
        nodes = graph();
        assert_eq!(plan_routes(&nodes, &routes, &HashMap::new(), &mut routed),
                   [set(20, "pwfx.bus.music")]);
    }

    #[test]
    fn a_new_stream_on_a_reused_id_is_a_new_stream() {
        // What the live test caught: Firefox's stream ends and a new one
        // starts under the same id within one tick. Keyed by id alone it
        // looked routed already and was left on the default.
        let mut nodes = graph();
        let routes = [route("Firefox", false, "pwfx.bus.music")];
        let mut routed = HashMap::new();
        plan_routes(&nodes, &routes, &HashMap::new(), &mut routed);
        nodes.get_mut(&20).unwrap().serial = 300;
        assert_eq!(plan_routes(&nodes, &routes, &HashMap::new(), &mut routed),
                   [set(20, "pwfx.bus.music")]);
    }

    #[test]
    fn a_stream_that_asks_not_to_be_moved_is_not() {
        let mut nodes = graph();
        nodes.get_mut(&20).unwrap().dont_move = true;
        let routes = [route("Firefox", false, "pwfx.bus.music")];
        assert!(plan_routes(&nodes, &routes, &HashMap::new(), &mut HashMap::new()).is_empty());
    }

    #[test]
    fn a_stream_is_not_routed_before_its_name_is_known() {
        let mut nodes = graph();
        nodes.get_mut(&20).unwrap().known = false;
        let routes = [route("Firefox", false, "pwfx.bus.music")];
        let mut routed = HashMap::new();
        assert!(plan_routes(&nodes, &routes, &HashMap::new(), &mut routed).is_empty());
        nodes.get_mut(&20).unwrap().known = true;
        assert_eq!(plan_routes(&nodes, &routes, &HashMap::new(), &mut routed),
                   [set(20, "pwfx.bus.music")]);
    }

    /// Against the real daemon: a stream's `media.name`, which the registry
    /// does not announce, reaches the snapshot. What the first version of
    /// routing one stream missed -- every test fed it the name by hand.
    /// Skips with no daemon or no `pw-cat`.
    #[test]
    fn a_real_streams_media_name_arrives() {
        let media = format!("pwfx-test-{}", std::process::id());
        let Ok(mut cat) = std::process::Command::new("pw-cat")
            .args(["--playback", "--raw", "--target", "0", "--format", "s16",
                   "--rate", "48000", "--channels", "1", "-P"])
            .arg(format!("{{ media.name = \"{media}\" application.name = \"pwfx-test\" }}"))
            .arg("-")
            .stdin(std::process::Stdio::piped())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
        else {
            eprintln!("no pw-cat; skipping");
            return;
        };
        let (tx, rx) = std::sync::mpsc::channel();
        let pw = start(move |ev| {
            if let PwEvent::Graph(g) = ev {
                let _ = tx.send(g);
            }
        });
        let deadline = Instant::now() + Duration::from_secs(3);
        let mut found = None;
        while let Some(left) = deadline.checked_duration_since(Instant::now()) {
            let Ok(g) = rx.recv_timeout(left) else { break };
            if let Some(n) = g.nodes.iter().find(|n| n.app.as_deref() == Some("pwfx-test")) {
                found = Some(n.stream_name().to_string());
                if n.media_name.is_some() {
                    break;
                }
            }
        }
        let _ = cat.kill();
        let _ = cat.wait();
        drop(pw);
        match found {
            Some(name) => assert_eq!(name, media),
            None => eprintln!("the test stream never appeared; skipping (no daemon?)"),
        }
    }

    /// Against the real daemon: the defaults arrive in the snapshot, and they
    /// name nodes that are in it. Skips with no daemon, or with a session
    /// manager that has not chosen a default.
    #[test]
    fn the_real_defaults_arrive_with_the_graph() {
        let (tx, rx) = std::sync::mpsc::channel();
        let pw = start(move |ev| {
            if let PwEvent::Graph(g) = ev {
                let _ = tx.send(g);
            }
        });
        let deadline = Instant::now() + Duration::from_secs(3);
        while let Some(left) = deadline.checked_duration_since(Instant::now()) {
            let Ok(g) = rx.recv_timeout(left) else { break };
            let Some(sink) = g.default_sink.clone() else { continue };
            assert!(g.node(&sink).is_some(), "the default sink {sink} is not in the graph");
            if let Some(source) = &g.default_source {
                assert!(g.node(source).is_some(), "the default source {source} is not in the graph");
            }
            drop(pw);
            return;
        }
        eprintln!("no default sink reported; skipping (no PipeWire daemon?)");
    }
}
