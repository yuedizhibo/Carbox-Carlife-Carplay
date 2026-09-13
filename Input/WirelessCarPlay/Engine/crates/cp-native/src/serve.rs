//! Control socket and engine loop.
//!
//! One single-threaded loop owns everything: the Unix control socket, the
//! bounded event ring and the AP supervision tick. No thread pool, no executor,
//! no unbounded queue. The engine in this slice is a self-test engine: it proves
//! supervision, bounds and the control channel work on the board. It does not
//! speak AirPlay, iAP2 or CarPlay, and every report says so.

//! This slice is the harness, not the protocol: it proves supervision, bounds
//! and the control channel on the board without claiming any Apple capability.

// The control-socket loop is Unix-only, so on a non-Unix host build the members
// it drives are intentionally unreachable.
#![cfg_attr(not(unix), allow(dead_code))]

use std::time::{Duration, Instant};

use cp_harness::bounds::Ring;
use cp_harness::config::RuntimeConfig;
use cp_harness::ipc::{self, ClientSlots, IpcError, Request};
use cp_harness::status::{
    ap_state, bounded_detail, preflight, status_report, ApState, EngineEvent, PreflightReport, RuntimeCounters, StatusReport,
};
use cp_harness::telemetry::{CarPlayTelemetry, OfferState};

use crate::engine::CatPlaySnapshot;
#[cfg(unix)]
use crate::engine::PROFILE_MISMATCH_PREFIX;
use crate::system;

/// Main loop period.
pub const TICK: Duration = Duration::from_millis(200);
/// Socket read timeout so one slow client cannot stall the loop.
pub const READ_TIMEOUT: Duration = Duration::from_millis(5);
/// Socket write timeout; a client that cannot take a reply is dropped.
pub const WRITE_TIMEOUT: Duration = Duration::from_millis(200);
/// Self-test heartbeat period.
pub const HEARTBEAT: Duration = Duration::from_secs(30);

pub const EV_STARTUP: &str = "startup";
pub const EV_HEARTBEAT: &str = "selftest-heartbeat";
pub const EV_AP_STATE: &str = "ap-state";
pub const EV_IPC: &str = "ipc-request";
pub const EV_ERROR: &str = "error";
pub const EV_SHUTDOWN: &str = "shutdown";

/// Bounded view of every cap the process runs under, for `diag`.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct BoundsReport {
    pub event_ring_used: usize,
    pub event_ring_cap: usize,
    pub event_ring_dropped: u64,
    pub media_cache_used: usize,
    pub media_cache_cap: usize,
    pub ipc_clients_used: usize,
    pub ipc_clients_cap: usize,
    pub ipc_line_bytes: usize,
    pub ipc_reply_bytes: usize,
    pub outbox_cap: usize,
    pub command_log_used: usize,
    pub command_log_cap: usize,
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct DiagReport {
    pub counters: RuntimeCounters,
    pub bounds: BoundsReport,
    pub events: Vec<EngineEvent>,
    pub command_log: Vec<String>,
    pub rss_kib: u64,
    pub memory_available_kib: u64,
}

/// What the AP supervisor reported last.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ApObservation {
    pub hostapd_state: String,
    pub stations: Vec<String>,
    pub restarts: u32,
    pub commands_run: u64,
    pub commands_failed: u64,
}

/// Reply to one control-socket line.
pub struct Handled {
    pub replies: Vec<String>,
    /// Keep the connection and stream events.
    pub follow: bool,
}

pub struct Server {
    cfg: RuntimeConfig,
    events: Ring<EngineEvent>,
    counters: RuntimeCounters,
    command_log: Vec<String>,
    seq: u64,
    started: Instant,
    slots: ClientSlots,
    last_heartbeat: Instant,
    ap: ApObservation,
    telemetry: CarPlayTelemetry,
    pub shutdown: bool,
}

impl Server {
    pub fn new(cfg: RuntimeConfig) -> Self {
        let cap = cfg.limits.event_ring;
        let mut server = Self {
            events: Ring::new(cap),
            counters: RuntimeCounters::default(),
            command_log: Vec::new(),
            seq: 0,
            started: Instant::now(),
            slots: ClientSlots::new(cfg.limits.ipc_clients),
            last_heartbeat: Instant::now(),
            ap: ApObservation::default(),
            telemetry: CarPlayTelemetry::default(),
            shutdown: false,
            cfg,
        };
        server.push_event(
            EV_STARTUP,
            &format!(
                "engine={} carplay-session=not-implemented ap-iface={} mfi=/dev/i2c-{}",
                server.cfg.engine.as_str(),
                server.cfg.ap.iface,
                server.cfg.mfi.target.bus
            ),
        );
        server
    }

    pub fn config(&self) -> &RuntimeConfig {
        &self.cfg
    }

    /// Take a client slot; the socket layer reports `too-many-clients` when full.
    pub fn acquire_client(&mut self) -> Result<(), IpcError> {
        self.slots.acquire()
    }

    pub fn release_client(&mut self) {
        self.slots.release();
    }

    pub fn push_event(&mut self, kind: &'static str, detail: &str) {
        self.seq = self.seq.wrapping_add(1);
        self.events.push(EngineEvent { seq: self.seq, kind, detail: bounded_detail(detail) });
        self.counters.events_pushed = self.counters.events_pushed.saturating_add(1);
        // The ring already counts every eviction, so mirror its total rather than
        // accumulating a per-push delta.
        self.counters.events_dropped = self.events.dropped();
    }

    /// Record what the AP supervisor observed; an event is emitted only when the
    /// hostapd state actually changes, so the ring cannot be flooded.
    pub fn observe_ap(&mut self, observation: ApObservation) {
        if observation.hostapd_state != self.ap.hostapd_state {
            self.push_event(
                EV_AP_STATE,
                &format!("hostapd={} stations={} restarts={}", observation.hostapd_state, observation.stations.len(), observation.restarts),
            );
        }
        self.counters.ap_restarts = observation.restarts;
        self.ap = observation;
    }

    /// Mirror the launcher's bounded command log so `diag` can show what was run.
    pub fn sync_command_log(&mut self, log: &crate::ap::CommandLog) {
        self.counters.commands_run = log.run;
        self.counters.commands_failed = log.failed;
        self.command_log = log.entries.iter().cloned().collect();
    }

    /// Project only observed CatPlay lifecycle booleans. No media, screen, or
    /// audio feature is promoted without an engine field that proves it.
    pub fn observe_catplay(&mut self, snapshot: &CatPlaySnapshot) -> Result<(), String> {
        snapshot.validate_for_profile(self.cfg.usb_profile)?;
        let is_error = snapshot.lifecycle == "error";
        let wireless = snapshot.wireless_ready_for_advertisement() && !is_error;
        let wired = snapshot.wired_endpoint_ready && !is_error;
        let iphone = snapshot.iphone_connected && !is_error;
        let vehicle = snapshot.vehicle_connected && !is_error;
        let bridge = snapshot.bridge_active && !is_error;
        if !self.telemetry.connected && bridge {
            self.telemetry.session_id = self.telemetry.session_id.saturating_add(1);
            if self.telemetry.session_id > 1 {
                self.telemetry.reconnects = self.telemetry.reconnects.saturating_add(1);
            }
        }
        self.telemetry.lifecycle = snapshot.lifecycle.clone();
        self.telemetry.wireless_listener_ready = wireless;
        self.telemetry.wired_endpoint_ready = wired;
        self.telemetry.iphone_connected = iphone;
        self.telemetry.vehicle_connected = vehicle;
        self.telemetry.bridge_active = bridge;
        self.telemetry.connected = bridge;
        self.telemetry.features.wifi_bonjour = if wireless { OfferState::Active } else { OfferState::NotNegotiated };
        self.telemetry.features.wired_usb = if wired { OfferState::Active } else { OfferState::NotNegotiated };
        self.telemetry.features.phone = if iphone { OfferState::Active } else { OfferState::NotOffered };
        self.telemetry.features.vehicle = if vehicle { OfferState::Active } else { OfferState::NotOffered };
        if is_error {
            self.telemetry.set_error("CatPlay lifecycle error");
        } else {
            self.telemetry.last_error.clear();
        }
        Ok(())
    }

    /// Clear all live state after transport loss or a child restart while
    /// retaining only bounded historical session counters.
    pub fn clear_catplay(&mut self, error: &str) {
        self.telemetry.lifecycle = "initial".to_string();
        self.telemetry.wireless_listener_ready = false;
        self.telemetry.wired_endpoint_ready = false;
        self.telemetry.iphone_connected = false;
        self.telemetry.vehicle_connected = false;
        self.telemetry.bridge_active = false;
        self.telemetry.connected = false;
        self.telemetry.features.wifi_bonjour = OfferState::NotNegotiated;
        self.telemetry.features.wired_usb = OfferState::NotNegotiated;
        self.telemetry.features.phone = OfferState::NotOffered;
        self.telemetry.features.vehicle = OfferState::NotOffered;
        self.telemetry.set_error(error);
    }

    /// One control-socket line in, zero or more lines out.
    pub fn handle_line(&mut self, line: &str) -> Handled {
        self.counters.ipc_requests = self.counters.ipc_requests.saturating_add(1);
        match ipc::parse_request(line, self.cfg.limits.ipc_line_bytes) {
            Err(error) => {
                self.counters.ipc_rejected = self.counters.ipc_rejected.saturating_add(1);
                self.push_event(EV_ERROR, &format!("ipc rejected: {}", error.as_str()));
                Handled { replies: vec![ipc::encode_error(0, error)], follow: false }
            }
            Ok(request) => {
                let follow = request.follow && request.op == cp_harness::ipc::OP_EVENTS;
                self.push_event(EV_IPC, &format!("op={}", request.op));
                let reply = self.reply(&request);
                Handled { replies: vec![reply], follow }
            }
        }
    }

    /// Events published since `last_seq`, oldest first, bounded by the ring.
    pub fn events_since(&self, last_seq: u64) -> Vec<EngineEvent> {
        self.events.iter().filter(|e| e.seq > last_seq).cloned().collect()
    }

    pub fn last_seq(&self) -> u64 {
        self.seq
    }

    fn reply(&mut self, request: &Request) -> String {
        // Replies are generated and item-bounded, so they get the wider reply
        // cap; the configured limit only bounds what a client may send us.
        let limit = cp_harness::bounds::IPC_REPLY_CAP;
        let result = match request.op.as_str() {
            cp_harness::ipc::OP_CONFIG => encode(&request.id, &self.cfg.summary(), limit),
            cp_harness::ipc::OP_PREFLIGHT => {
                let snapshot = system::collect(&self.cfg);
                let report: PreflightReport = preflight(&self.cfg, &snapshot);
                encode(&request.id, &report, limit)
            }
            cp_harness::ipc::OP_AP_STATE => {
                let snapshot = system::collect(&self.cfg);
                let state: ApState = ap_state(&self.cfg, &snapshot, &self.ap.hostapd_state, &self.ap.stations);
                encode(&request.id, &state, limit)
            }
            cp_harness::ipc::OP_STATUS => {
                let snapshot = system::collect(&self.cfg);
                let mut counters = self.counters.clone();
                counters.uptime_secs = self.started.elapsed().as_secs();
                counters.ap_restarts = self.ap.restarts;
                counters.commands_run = self.ap.commands_run;
                counters.commands_failed = self.ap.commands_failed;
                counters.events_dropped = self.events.dropped();
                let report: StatusReport = status_report(
                    &self.cfg,
                    &snapshot,
                    &format!("state={}\n", self.ap.hostapd_state),
                    &self.ap.stations,
                    counters,
                    env!("CARGO_PKG_VERSION"),
                    self.telemetry.clone(),
                );
                encode(&request.id, &report, limit)
            }
            cp_harness::ipc::OP_EVENTS => {
                let replay = request.replay.min(self.events.len());
                let events: Vec<EngineEvent> =
                    self.events.iter().rev().take(replay).cloned().collect::<Vec<_>>().into_iter().rev().collect();
                encode(&request.id, &events, limit)
            }
            cp_harness::ipc::OP_DIAG => {
                let diag = self.diag_report();
                encode(&request.id, &diag, limit)
            }
            cp_harness::ipc::OP_TELEMETRY => encode(&request.id, &self.telemetry, limit),
            cp_harness::ipc::OP_SHUTDOWN => {
                self.shutdown = true;
                self.push_event(EV_SHUTDOWN, "requested over the control socket");
                encode(&request.id, &"shutting-down", limit)
            }
            other => Err((IpcError::UnknownOp, other.to_string())),
        };
        match result {
            Ok(line) => line,
            Err((error, detail)) => {
                self.push_event(EV_ERROR, &format!("reply failed: {} {detail}", error.as_str()));
                ipc::encode_error(request.id, error)
            }
        }
    }

    pub fn diag_report(&self) -> DiagReport {
        DiagReport {
            counters: RuntimeCounters { uptime_secs: self.started.elapsed().as_secs(), ..self.counters.clone() },
            bounds: BoundsReport {
                event_ring_used: self.events.len(),
                event_ring_cap: self.events.capacity(),
                event_ring_dropped: self.events.dropped(),
                // No media engine exists in this slice, so the cache stays empty.
                media_cache_used: 0,
                media_cache_cap: self.cfg.limits.frame_cache,
                ipc_clients_used: self.slots.in_use(),
                ipc_clients_cap: self.slots.cap(),
                ipc_line_bytes: self.cfg.limits.ipc_line_bytes,
                ipc_reply_bytes: cp_harness::bounds::IPC_REPLY_CAP,
                outbox_cap: cp_harness::bounds::IPC_OUTBOX_CAP,
                command_log_used: self.command_log.len(),
                command_log_cap: cp_harness::bounds::COMMAND_LOG_CAP,
            },
            events: self
                .events
                .iter()
                .rev()
                .take(cp_harness::bounds::DIAG_EVENT_CAP)
                .cloned()
                .collect::<Vec<_>>()
                .into_iter()
                .rev()
                .collect(),
            command_log: self.command_log.clone(),
            rss_kib: system::rss_kib(),
            memory_available_kib: system::meminfo().1,
        }
    }

    /// One main-loop step: heartbeat plus status refresh. Returns false when the
    /// caller should exit.
    pub fn tick(&mut self) -> bool {
        if self.shutdown {
            return false;
        }
        self.counters.uptime_secs = self.started.elapsed().as_secs();
        if self.last_heartbeat.elapsed() >= HEARTBEAT {
            self.last_heartbeat = Instant::now();
            self.push_event(
                EV_HEARTBEAT,
                &format!(
                    "engine={} uptime={}s hostapd={} events={}/{}-dropped rss={}KiB",
                    self.cfg.engine.as_str(),
                    self.counters.uptime_secs,
                    if self.ap.hostapd_state.is_empty() { "unknown" } else { &self.ap.hostapd_state },
                    self.counters.events_pushed,
                    self.events.dropped(),
                    system::rss_kib()
                ),
            );
        }
        true
    }
}

fn encode<T: serde::Serialize>(id: &u64, payload: &T, limit: usize) -> Result<String, (IpcError, String)> {
    ipc::encode_reply(*id, payload, limit).map_err(|e| (e, "payload".to_string()))
}

/// The serving loop uses one path for every lost or untrustworthy telemetry
/// stream, so stale listener state cannot survive to mDNS after EOF, malformed
/// input, heartbeat expiry, or an engine restart.
#[cfg(unix)]
fn clear_catplay_transport(
    server: &mut Server,
    launcher: Option<&mut crate::ap::Launcher>,
    runtime: &mut crate::ap::ApRuntime,
    error: &str,
) {
    server.clear_catplay(error);
    if let Some(launcher) = launcher {
        launcher.set_wireless_listener_ready(runtime, false);
    }
}

/// A child that contradicts its launch profile is not recoverable telemetry
/// loss: it is stopped and the loop exits after withdrawing mDNS.
#[cfg(unix)]
fn fail_profile_mismatch(server: &mut Server, launcher: Option<&mut crate::ap::Launcher>, runtime: &mut crate::ap::ApRuntime, error: &str) {
    clear_catplay_transport(server, launcher, runtime, error);
    server.push_event(EV_ERROR, error);
    server.shutdown = true;
}

/// Client-side helper: one request, one reply line.
#[cfg(unix)]
pub fn query(socket_path: &str, request: &str, timeout: Duration) -> std::io::Result<String> {
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream;
    let mut stream = UnixStream::connect(socket_path)?;
    stream.set_write_timeout(Some(timeout))?;
    stream.set_read_timeout(Some(timeout))?;
    stream.write_all(request.as_bytes())?;
    stream.write_all(b"\n")?;
    let mut line = String::new();
    let mut buf = [0u8; 512];
    while line.len() < cp_harness::bounds::IPC_REPLY_CAP {
        let n = stream.read(&mut buf)?;
        if n == 0 {
            break;
        }
        line.push_str(&String::from_utf8_lossy(&buf[..n]));
        if line.len() > cp_harness::bounds::IPC_REPLY_CAP {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "reply exceeds the cap"));
        }
        if line.contains('\n') {
            break;
        }
    }
    Ok(line.trim_end_matches(['\n', '\r']).to_string())
}

#[cfg(not(unix))]
pub fn query(_socket_path: &str, _request: &str, _timeout: Duration) -> std::io::Result<String> {
    Err(std::io::Error::new(std::io::ErrorKind::Unsupported, "the control socket needs a Unix target"))
}

/// Stream events from a running sidecar until it closes the socket.
///
/// No read timeout here: the client explicitly asked to be held open. Each line
/// is still capped, so a peer cannot make this process grow.
#[cfg(unix)]
pub fn follow(socket_path: &str, request: &str, mut on_line: impl FnMut(&str)) -> std::io::Result<()> {
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream;
    let mut stream = UnixStream::connect(socket_path)?;
    stream.set_write_timeout(Some(WRITE_TIMEOUT))?;
    stream.write_all(request.as_bytes())?;
    stream.write_all(b"\n")?;
    stream.set_read_timeout(None)?;
    let mut inbox: Vec<u8> = Vec::new();
    let mut buf = [0u8; 512];
    loop {
        let n = stream.read(&mut buf)?;
        if n == 0 {
            return Ok(());
        }
        inbox.extend_from_slice(&buf[..n]);
        if inbox.len() > cp_harness::bounds::IPC_REPLY_CAP {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "streamed reply exceeds the cap"));
        }
        while let Some(position) = inbox.iter().position(|b| *b == b'\n') {
            let raw: Vec<u8> = inbox.drain(..=position).collect();
            let text = String::from_utf8_lossy(&raw);
            on_line(text.trim_end_matches(['\n', '\r']));
        }
    }
}

#[cfg(not(unix))]
pub fn follow(_socket_path: &str, _request: &str, _on_line: impl FnMut(&str)) -> std::io::Result<()> {
    Err(std::io::Error::new(std::io::ErrorKind::Unsupported, "the control socket needs a Unix target"))
}

/// Serve the control socket, and the AP when one was handed in, until
/// `server.shutdown` is set or `stop` is raised.
///
/// Bounded on every axis: at most [`ClientSlots`] clients, at most one pending
/// outbox per client, inbox capped by `ipc_line_bytes`, and each reply is
/// size-checked before it is queued.
#[cfg(unix)]
pub fn serve(
    server: &mut Server,
    mut ap: Option<&mut crate::ap::Launcher>,
    telemetry_path: Option<&str>,
    mut engine: Option<&mut crate::engine::EngineSupervisor>,
    stop: &std::sync::atomic::AtomicBool,
) -> std::io::Result<i32> {
    use cp_harness::ipc::Outbox;
    use std::io::{Read, Write};
    use std::os::unix::fs::PermissionsExt;
    use std::os::unix::net::{UnixListener, UnixStream};
    use std::sync::atomic::Ordering;

    struct Client {
        stream: UnixStream,
        inbox: Vec<u8>,
        outbox: Outbox,
        follow: bool,
        last_seq: u64,
    }

    let path = server.config().core_socket.clone();
    if let Some(parent) = std::path::Path::new(&path).parent() {
        std::fs::create_dir_all(parent)?;
    }
    // `create_new` makes concurrent `run` invocations fail before either can
    // change wlan0. A stale lock requires deliberate operator inspection;
    // blindly deleting it would reintroduce the same race after a slow start.
    let _lock = InstanceLock::acquire(&format!("{}/cp-native.lock", server.config().runtime_dir))?;
    // A stale socket from a killed run would make bind fail forever.
    if std::path::Path::new(&path).exists() {
        std::fs::remove_file(&path)?;
    }
    let listener = UnixListener::bind(&path)?;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;
    listener.set_nonblocking(true)?;
    println!(
        "[ipc] listening on {path} (max {} clients, {} byte request lines)",
        server.config().limits.ipc_clients,
        server.config().limits.ipc_line_bytes
    );

    let mut clients: Vec<Client> = Vec::new();
    let limit = server.config().limits.ipc_line_bytes;
    let mut runtime = crate::ap::ApRuntime::new();

    if let Some(launcher) = ap.as_deref_mut() {
        match launcher.bring_up(&mut runtime) {
            Ok(()) => {
                if launcher.wait_ready() {
                    let c = server.config();
                    println!(
                        "[ap] up: ssid={} iface={} ip={} channel={} width={}MHz",
                        c.ap.ssid, c.ap.iface, c.ap.ap_ip, c.ap.channel, c.ap.width_mhz
                    );
                } else {
                    let iface = server.config().ap.iface.clone();
                    eprintln!("[ap] {iface} readiness timeout after {}s", crate::ap::READY_TIMEOUT.as_secs());
                    server.push_event(EV_ERROR, "ap readiness timeout");
                }
            }
            Err(e) => {
                eprintln!("[ap] {e}");
                server.push_event(EV_ERROR, &e);
            }
        }
    }
    // AP preparation can block for its full readiness budget. Do not spend the
    // engine's three-second telemetry window until that work is complete.
    let mut catplay = telemetry_path.map(|path| crate::engine::CatPlayTelemetryClient::new(path.to_string()));

    while !stop.load(Ordering::Relaxed) {
        if !server.tick() {
            break;
        }
        if let Some(supervisor) = engine.as_deref_mut() {
            if !supervisor.poll() {
                clear_catplay_transport(server, ap.as_deref_mut(), &mut runtime, "CatPlay engine restart cap reached");
                server.push_event(EV_ERROR, "CatPlay engine restart cap reached");
                server.shutdown = true;
                continue;
            }
            if supervisor.take_exit() {
                clear_catplay_transport(server, ap.as_deref_mut(), &mut runtime, "CatPlay engine exited");
                if let Some(reader) = catplay.as_mut() {
                    reader.reset();
                }
            }
            if supervisor.needs_spawn() && supervisor.may_retry_now() {
                if let Err(error) = supervisor.spawn(server.config()) {
                    clear_catplay_transport(server, ap.as_deref_mut(), &mut runtime, "CatPlay engine restart failed");
                    server.push_event(EV_ERROR, &format!("CatPlay engine restart failed: {error}"));
                    server.shutdown = true;
                    continue;
                }
                // A restarted engine gets its own fresh window, including
                // after an exponential backoff longer than the old window.
                if let Some(reader) = catplay.as_mut() {
                    reader.reset();
                }
            }
        }
        if let Some(reader) = catplay.as_mut() {
            match reader.poll_for_profile(server.config().usb_profile) {
                Ok(snapshots) => {
                    for snapshot in snapshots {
                        if let Err(error) = server.observe_catplay(&snapshot) {
                            let error = format!("{PROFILE_MISMATCH_PREFIX}: {error}");
                            if let Some(supervisor) = engine.as_deref_mut() {
                                supervisor.stop();
                            }
                            fail_profile_mismatch(server, ap.as_deref_mut(), &mut runtime, &error);
                            break;
                        }
                        if let Some(launcher) = ap.as_deref_mut() {
                            launcher.set_wireless_listener_ready(&mut runtime, snapshot.wireless_ready_for_advertisement());
                        }
                    }
                }
                Err(error) if error.starts_with(PROFILE_MISMATCH_PREFIX) => {
                    if let Some(supervisor) = engine.as_deref_mut() {
                        supervisor.stop();
                    }
                    fail_profile_mismatch(server, ap.as_deref_mut(), &mut runtime, &error);
                }
                Err(error) => clear_catplay_transport(server, ap.as_deref_mut(), &mut runtime, &error),
            }
        }
        if let Some(launcher) = ap.as_deref_mut() {
            let observation = launcher.poll(&mut runtime);
            server.sync_command_log(&launcher.log);
            server.observe_ap(observation);
        }
        match listener.accept() {
            Ok((stream, _)) => {
                if server.acquire_client().is_err() {
                    let mut rejected = stream;
                    let _ = rejected.set_write_timeout(Some(WRITE_TIMEOUT));
                    let _ = rejected.write_all(format!("{}\n", ipc::encode_error(0, IpcError::TooManyClients)).as_bytes());
                    server.push_event(EV_ERROR, "ipc client refused: slot cap reached");
                } else {
                    let _ = stream.set_read_timeout(Some(READ_TIMEOUT));
                    let _ = stream.set_write_timeout(Some(WRITE_TIMEOUT));
                    clients.push(Client {
                        stream,
                        inbox: Vec::new(),
                        outbox: Outbox::new(cp_harness::bounds::IPC_OUTBOX_CAP),
                        follow: false,
                        last_seq: 0,
                    });
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {}
            Err(e) => {
                server.push_event(EV_ERROR, &format!("accept failed: {e}"));
                std::thread::sleep(TICK);
            }
        }

        let mut buf = [0u8; 512];
        let mut index = 0;
        while index < clients.len() {
            let mut drop_client = false;
            let mut replies: Vec<String> = Vec::new();
            {
                let client = &mut clients[index];
                match client.stream.read(&mut buf) {
                    Ok(0) => drop_client = true,
                    Ok(n) => {
                        client.inbox.extend_from_slice(&buf[..n]);
                        if client.inbox.len() > limit {
                            // Send this fixed-size error before closing. The
                            // previous implementation queued it and then
                            // skipped draining because `drop_client` was set.
                            let _ = client.stream.write_all(format!("{}\n", ipc::encode_error(0, IpcError::LineTooLong)).as_bytes());
                            drop_client = true;
                        } else {
                            while let Some(position) = client.inbox.iter().position(|b| *b == b'\n') {
                                let line: Vec<u8> = client.inbox.drain(..=position).collect();
                                let text = String::from_utf8_lossy(&line);
                                let handled = server.handle_line(&text);
                                replies.extend(handled.replies);
                                client.follow = handled.follow;
                                client.last_seq = server.last_seq();
                            }
                        }
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock || e.kind() == std::io::ErrorKind::TimedOut => {}
                    Err(_) => drop_client = true,
                }
                if client.follow {
                    for event in server.events_since(client.last_seq) {
                        client.last_seq = event.seq.max(client.last_seq);
                        if let Ok(line) = ipc::encode_reply(0, &event, cp_harness::bounds::IPC_REPLY_CAP) {
                            replies.push(line);
                        }
                    }
                }
                for line in replies {
                    if !client.outbox.push(line) {
                        drop_client = true;
                        server.push_event(EV_ERROR, "ipc client dropped: outbox full");
                        break;
                    }
                }
                if !drop_client {
                    for line in client.outbox.drain() {
                        if client.stream.write_all(line.as_bytes()).is_err() || client.stream.write_all(b"\n").is_err() {
                            drop_client = true;
                            break;
                        }
                    }
                }
            }
            if drop_client {
                clients.remove(index);
                server.release_client();
            } else {
                index += 1;
            }
        }
        std::thread::sleep(TICK);
    }

    for client in clients.iter() {
        let _ = client.stream.shutdown(std::net::Shutdown::Both);
    }
    clients.clear();
    if let Some(launcher) = ap {
        if let Some(children) = runtime.children.as_mut() {
            children.kill_all();
        }
        launcher.rollback();
    }
    if let Some(supervisor) = engine {
        supervisor.stop();
    }
    std::fs::remove_file(&path).ok();
    Ok(0)
}

#[cfg(unix)]
struct InstanceLock {
    path: String,
}

#[cfg(unix)]
impl InstanceLock {
    fn acquire(path: &str) -> std::io::Result<Self> {
        use std::io::Write;
        use std::os::unix::fs::OpenOptionsExt;
        let mut file = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(path)?;
        writeln!(file, "{}", std::process::id())?;
        file.sync_all()?;
        Ok(Self { path: path.to_string() })
    }
}

#[cfg(unix)]
impl Drop for InstanceLock {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

#[cfg(not(unix))]
pub fn serve(
    _server: &mut Server,
    _ap: Option<&mut crate::ap::Launcher>,
    _telemetry_path: Option<&str>,
    _engine: Option<&mut crate::engine::EngineSupervisor>,
    _stop: &std::sync::atomic::AtomicBool,
) -> std::io::Result<i32> {
    Err(std::io::Error::new(std::io::ErrorKind::Unsupported, "serving the control socket needs a Unix target"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use cp_harness::ipc::KNOWN_OPS;
    use std::collections::BTreeMap;

    fn cfg() -> RuntimeConfig {
        let mut env = BTreeMap::new();
        env.insert("CP_COUNTRY".to_string(), "US".to_string());
        env.insert("CP_EVENT_RING".to_string(), "4".to_string());
        RuntimeConfig::from_map(&env).expect("valid")
    }

    fn line(op: &str) -> String {
        format!("{{\"op\":\"{op}\",\"id\":1}}")
    }

    #[test]
    fn startup_event_states_the_missing_capability() {
        let server = Server::new(cfg());
        let events: Vec<&EngineEvent> = server.events.iter().collect();
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].kind, EV_STARTUP);
        assert!(events[0].detail.contains("carplay-session=not-implemented"), "{:?}", events[0]);
        assert!(events[0].detail.contains("/dev/i2c-1"), "{:?}", events[0]);
    }

    #[test]
    fn unknown_and_malformed_requests_are_counted_and_rejected() {
        let mut server = Server::new(cfg());
        let handled = server.handle_line("{\"op\":\"reboot\"}");
        assert_eq!(handled.replies.len(), 1);
        assert!(handled.replies[0].contains("unknown-op"), "{:?}", handled.replies);
        assert!(!handled.follow);
        let handled = server.handle_line("garbage");
        assert!(handled.replies[0].contains("malformed"), "{:?}", handled.replies);
        assert_eq!(server.counters.ipc_requests, 2);
        assert_eq!(server.counters.ipc_rejected, 2);
    }

    #[test]
    fn overlong_request_line_is_rejected() {
        let mut server = Server::new(cfg());
        let handled = server.handle_line(&format!("{{\"op\":\"status\",\"pad\":\"{}\"}}", "x".repeat(4000)));
        assert!(handled.replies[0].contains("line-too-long"), "{:?}", handled.replies);
    }

    #[test]
    fn known_ops_all_answer_on_one_line() {
        let mut server = Server::new(cfg());
        for op in KNOWN_OPS {
            let handled = server.handle_line(&line(op));
            assert_eq!(handled.replies.len(), 1, "{op}");
            let reply = &handled.replies[0];
            assert!(!reply.contains('\n'), "{op}: {reply}");
            assert!(reply.contains("\"id\":1"), "{op}: {reply}");
            if op == cp_harness::ipc::OP_SHUTDOWN {
                assert!(server.shutdown);
                continue;
            }
            assert!(reply.contains("\"ok\":true"), "{op}: {reply}");
        }
    }

    #[test]
    fn config_reply_is_redacted() {
        let mut server = Server::new(cfg());
        let handled = server.handle_line(&line(cp_harness::ipc::OP_CONFIG));
        assert!(!handled.replies[0].contains(&server.config().ap.passphrase), "{:?}", handled.replies);
        assert!(handled.replies[0].contains("\"passphrase_len\""), "{:?}", handled.replies);
    }

    #[test]
    fn status_reply_reports_the_session_as_unimplemented() {
        let mut server = Server::new(cfg());
        let handled = server.handle_line(&line(cp_harness::ipc::OP_STATUS));
        assert!(handled.replies[0].contains("\"carplay_session\":\"not-implemented\""), "{:?}", handled.replies);
    }

    #[test]
    fn telemetry_reply_is_versioned_and_truthful() {
        let mut server = Server::new(cfg());
        let handled = server.handle_line("{\"op\":\"telemetry\",\"id\":9}");
        assert!(handled.replies[0].contains("\"schema_version\":1"), "{:?}", handled.replies);
        assert!(handled.replies[0].contains("\"second_screen\":\"not-offered\""), "{:?}", handled.replies);
    }

    #[test]
    fn rejected_stream_record_clears_the_previously_projected_live_state() {
        let mut c = cfg();
        c.usb_profile = cp_harness::config::UsbProfile::Vehicle;
        let mut server = Server::new(c);
        let running = CatPlaySnapshot {
            schema_version: 1,
            engine: "catplay".to_string(),
            lifecycle: "running".to_string(),
            wireless_listener_ready: true,
            wired_endpoint_ready: true,
            iphone_connected: true,
            vehicle_connected: true,
            bridge_active: true,
            input_only: false,
        };
        server.observe_catplay(&running).expect("vehicle record");
        let stale_error = br#"{"schema_version":1,"engine":"catplay","lifecycle":"error","wireless_listener_ready":true,"wired_endpoint_ready":true,"iphone_connected":true,"vehicle_connected":true,"bridge_active":true,"input_only":false}"#;
        assert!(crate::engine::parse_snapshot(stale_error).is_err());
        // This is the same failure branch used by the telemetry stream.
        server.clear_catplay("contradictory CatPlay error lifecycle");
        assert!(!server.telemetry.connected);
        assert!(!server.telemetry.wireless_listener_ready);
        assert!(!server.telemetry.wired_endpoint_ready);
        assert!(!server.telemetry.iphone_connected);
        assert!(!server.telemetry.vehicle_connected);
        assert!(!server.telemetry.bridge_active);
        assert_eq!(server.telemetry.features.wifi_bonjour, OfferState::NotNegotiated);
        assert_eq!(server.telemetry.features.second_screen, OfferState::NotOffered);
    }

    #[cfg(unix)]
    #[test]
    fn heartbeat_expiry_clears_server_state_and_maybe_publisher_through_serve_path() {
        use std::os::unix::net::UnixListener;

        let mut c = cfg();
        c.usb_profile = cp_harness::config::UsbProfile::Vehicle;
        let mut server = Server::new(c.clone());
        server
            .observe_catplay(&CatPlaySnapshot {
                schema_version: 1,
                engine: "catplay".to_string(),
                lifecycle: "running".to_string(),
                wireless_listener_ready: true,
                wired_endpoint_ready: true,
                iphone_connected: true,
                vehicle_connected: true,
                bridge_active: true,
                input_only: false,
            })
            .expect("vehicle record");
        let mut launcher = crate::ap::Launcher::new(c.clone(), crate::ap::plan(&c, "", "", "", false));
        let mut runtime = crate::ap::ApRuntime::new();
        launcher.set_wireless_listener_ready(&mut runtime, true);

        let path = std::env::temp_dir().join(format!("cp-native-serve-heartbeat-{}", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path).expect("fake telemetry socket");
        let now = Instant::now();
        let mut client = crate::engine::CatPlayTelemetryClient::new_at(path.to_string_lossy().into_owned(), now);
        assert!(client.poll_at(now).expect("connect").is_empty());
        let (_peer, _) = listener.accept().expect("accept telemetry client");
        let error = client
            .poll_at(now + crate::engine::TELEMETRY_HEARTBEAT_TIMEOUT)
            .expect_err("silent peer must expire its heartbeat");
        clear_catplay_transport(&mut server, Some(&mut launcher), &mut runtime, &error);

        assert!(!server.telemetry.connected);
        assert!(!server.telemetry.wireless_listener_ready);
        assert_eq!(server.telemetry.features.wifi_bonjour, OfferState::NotNegotiated);
        assert!(!runtime.publisher_desired(), "serve loss path withdraws mDNS");
        let _ = std::fs::remove_file(&path);
    }

    #[cfg(unix)]
    #[test]
    fn profile_mismatch_shutdown_clears_state_and_withdraws_publisher() {
        let c = cfg();
        let mut server = Server::new(c.clone());
        let input_only = CatPlaySnapshot {
            schema_version: 1,
            engine: "catplay".to_string(),
            lifecycle: "waiting_for_iphone".to_string(),
            wireless_listener_ready: true,
            wired_endpoint_ready: false,
            iphone_connected: true,
            vehicle_connected: false,
            bridge_active: false,
            input_only: true,
        };
        server.observe_catplay(&input_only).expect("development record");
        let mut launcher = crate::ap::Launcher::new(c.clone(), crate::ap::plan(&c, "", "", "", false));
        let mut runtime = crate::ap::ApRuntime::new();
        launcher.set_wireless_listener_ready(&mut runtime, true);
        fail_profile_mismatch(
            &mut server,
            Some(&mut launcher),
            &mut runtime,
            "CatPlay telemetry profile mismatch: development requires input_only=true",
        );
        assert!(server.shutdown);
        assert!(!server.telemetry.wireless_listener_ready);
        assert!(!runtime.publisher_desired(), "profile mismatch withdraws mDNS before shutdown");
    }

    #[test]
    fn only_observed_bridge_transitions_increment_session_counters() {
        let mut c = cfg();
        c.usb_profile = cp_harness::config::UsbProfile::Vehicle;
        let mut server = Server::new(c);
        let running = CatPlaySnapshot {
            schema_version: 1,
            engine: "catplay".to_string(),
            lifecycle: "running".to_string(),
            wireless_listener_ready: true,
            wired_endpoint_ready: true,
            iphone_connected: true,
            vehicle_connected: true,
            bridge_active: true,
            input_only: false,
        };
        server.observe_catplay(&running).expect("vehicle record");
        server.observe_catplay(&running).expect("vehicle record");
        assert_eq!(server.telemetry.session_id, 1);
        assert_eq!(server.telemetry.reconnects, 0);
        server.clear_catplay("test disconnect");
        server.observe_catplay(&running).expect("vehicle record");
        assert_eq!(server.telemetry.session_id, 2);
        assert_eq!(server.telemetry.reconnects, 1);
    }

    #[test]
    fn live_telemetry_and_status_share_the_same_projection() {
        let mut c = cfg();
        c.usb_profile = cp_harness::config::UsbProfile::Vehicle;
        let mut server = Server::new(c);
        server
            .observe_catplay(&CatPlaySnapshot {
                schema_version: 1,
                engine: "catplay".to_string(),
                lifecycle: "running".to_string(),
                wireless_listener_ready: true,
                wired_endpoint_ready: true,
                iphone_connected: true,
                vehicle_connected: true,
                bridge_active: true,
                input_only: false,
            })
            .expect("vehicle record");
        let telemetry = server.handle_line("{\"op\":\"telemetry\",\"id\":9}").replies.remove(0);
        let status = server.handle_line("{\"op\":\"status\",\"id\":9}").replies.remove(0);
        assert!(telemetry.contains("\"bridge_active\":true"), "{telemetry}");
        assert!(status.contains("\"bridge_active\":true"), "{status}");
        assert!(status.contains("\"wifi_bonjour\":\"active\""), "{status}");
    }

    #[test]
    fn only_events_can_follow() {
        let mut server = Server::new(cfg());
        assert!(server.handle_line("{\"op\":\"events\",\"follow\":true}").follow);
        assert!(!server.handle_line("{\"op\":\"status\",\"follow\":true}").follow);
    }

    #[test]
    fn events_replay_is_bounded_by_the_ring() {
        let mut server = Server::new(cfg());
        for i in 0..10 {
            server.push_event(EV_HEARTBEAT, &format!("beat {i}"));
        }
        assert_eq!(server.events.len(), 4);
        let handled = server.handle_line("{\"op\":\"events\",\"id\":2,\"replay\":4}");
        let reply = &handled.replies[0];
        // The request itself is recorded as an event, so it takes the fourth slot
        // and evicts the oldest heartbeat: the ring, not the caller, decides.
        assert!(reply.contains("beat 9"), "{reply}");
        assert!(reply.contains("beat 7"), "{reply}");
        assert!(!reply.contains("beat 6"), "{reply}");
        assert!(!reply.contains("beat 5"), "{reply}");
        assert!(reply.contains("op=events"), "{reply}");
        assert_eq!(server.events.len(), 4);
        // 12 pushes into a 4-entry ring: startup, 10 heartbeats and the request.
        assert_eq!(server.counters.events_pushed, 12);
        assert_eq!(server.counters.events_dropped, 8);
    }

    #[test]
    fn events_since_only_returns_newer_entries() {
        let mut server = Server::new(cfg());
        server.push_event(EV_HEARTBEAT, "one");
        let mark = server.last_seq();
        server.push_event(EV_HEARTBEAT, "two");
        let newer = server.events_since(mark);
        assert_eq!(newer.len(), 1);
        assert_eq!(newer[0].detail, "two");
    }

    #[test]
    fn ap_state_changes_emit_exactly_one_event() {
        let mut server = Server::new(cfg());
        let observation =
            ApObservation { hostapd_state: "ENABLED".to_string(), stations: vec![], restarts: 0, commands_run: 4, commands_failed: 0 };
        server.observe_ap(observation.clone());
        server.observe_ap(observation.clone());
        server.observe_ap(ApObservation { hostapd_state: "DISABLED".to_string(), ..observation });
        let states: Vec<&EngineEvent> = server.events.iter().filter(|e| e.kind == EV_AP_STATE).collect();
        assert_eq!(states.len(), 2, "{states:?}");
        assert!(states[0].detail.contains("hostapd=ENABLED"), "{:?}", states[0]);
    }

    #[test]
    fn diag_reports_every_bound() {
        let server = Server::new(cfg());
        let diag = server.diag_report();
        assert_eq!(diag.bounds.event_ring_cap, 4);
        assert_eq!(diag.bounds.event_ring_used, 1);
        assert_eq!(diag.bounds.media_cache_used, 0);
        assert_eq!(diag.bounds.media_cache_cap, cp_harness::bounds::FRAME_CACHE_CAP);
        assert_eq!(diag.bounds.ipc_clients_cap, cp_harness::bounds::IPC_CLIENT_CAP);
        assert_eq!(diag.bounds.outbox_cap, cp_harness::bounds::IPC_OUTBOX_CAP);
        assert_eq!(diag.bounds.ipc_reply_bytes, cp_harness::bounds::IPC_REPLY_CAP);
        assert_eq!(diag.bounds.command_log_cap, cp_harness::bounds::COMMAND_LOG_CAP);
        assert_eq!(diag.events.len(), 1);
        let json = serde_json::to_string(&diag).expect("json");
        assert!(json.contains("\"bounds\""), "{json}");
    }

    #[test]
    fn client_slots_are_owned_by_the_server() {
        let mut server = Server::new(cfg());
        assert_eq!(server.slots.in_use(), 0);
        assert!(server.acquire_client().is_ok());
        assert!(server.acquire_client().is_ok());
        assert!(server.acquire_client().is_err());
        assert_eq!(server.diag_report().bounds.ipc_clients_used, 2);
        server.release_client();
        assert_eq!(server.slots.in_use(), 1);
    }

    #[test]
    fn tick_stops_after_shutdown() {
        let mut server = Server::new(cfg());
        assert!(server.tick());
        server.shutdown = true;
        assert!(!server.tick());
    }

    #[test]
    fn heartbeat_is_rate_limited() {
        let mut server = Server::new(cfg());
        for _ in 0..5 {
            server.tick();
        }
        assert_eq!(server.events.iter().filter(|e| e.kind == EV_HEARTBEAT).count(), 0);
        server.last_heartbeat = Instant::now() - HEARTBEAT - Duration::from_secs(1);
        server.tick();
        assert_eq!(server.events.iter().filter(|e| e.kind == EV_HEARTBEAT).count(), 1);
    }

    #[test]
    fn detail_is_truncated_before_it_reaches_the_ring() {
        let mut server = Server::new(cfg());
        server.push_event(EV_ERROR, &"z".repeat(500));
        let event = server.events.iter().last().expect("event");
        assert!(event.detail.len() <= 99, "{}", event.detail.len());
    }
}
