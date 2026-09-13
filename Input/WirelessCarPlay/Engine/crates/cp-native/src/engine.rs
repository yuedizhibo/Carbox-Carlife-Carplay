//! External protocol-engine capability handshake and bounded restart policy.
//!
//! CatPlay remains an independently built external artifact: this module never
//! copies or links its source. Before `cp-native` can select it, the executable
//! must answer a small JSON support handshake. Live listener state comes only
//! from the bounded Unix-socket lifecycle stream after the child is started.

// The Unix control loop owns the restart state. Windows host tests still
// compile the configuration/handshake code but cannot call that loop.
#![cfg_attr(not(unix), allow(dead_code))]

use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use cp_harness::config::{Engine, RuntimeConfig, UsbProfile};
use cp_harness::telemetry::TELEMETRY_SCHEMA_VERSION;
use serde::Deserialize;

#[cfg(unix)]
use std::io::Read;
#[cfg(unix)]
use std::os::unix::net::UnixStream;

use crate::system;

pub const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(3);
pub const ENGINE_RESTART_CAP: u32 = 3;
pub const ENGINE_RESTART_BASE: Duration = Duration::from_secs(2);
pub const CATPLAY_LINE_CAP: usize = 4096;
/// CatPlay emits lifecycle heartbeats once per second. Missing three means the
/// live projection is no longer trustworthy, even if its process still lives.
pub const TELEMETRY_HEARTBEAT_TIMEOUT: Duration = Duration::from_secs(3);
/// Prefix used by the serving loop to turn a mode-confused child into a hard
/// failure instead of accepting a later record under the wrong USB profile.
pub const PROFILE_MISMATCH_PREFIX: &str = "CatPlay telemetry profile mismatch";
const TELEMETRY_CONNECT_BACKOFF: Duration = Duration::from_millis(200);
const TELEMETRY_READS_PER_TICK: usize = 4;

#[derive(Debug, Clone, Deserialize)]
pub struct EngineCapabilities {
    pub schema_version: u16,
    pub engine: String,
    pub listener_ready: bool,
    pub wireless_input: bool,
    pub wired_output: bool,
    /// The executable can run without claiming the UDC/configfs gadget.
    pub input_only: bool,
    pub telemetry_socket: String,
}

impl EngineCapabilities {
    pub fn validate(&self) -> Result<(), String> {
        if self.schema_version != TELEMETRY_SCHEMA_VERSION {
            return Err(format!("schema {} is incompatible with {}", self.schema_version, TELEMETRY_SCHEMA_VERSION));
        }
        if self.engine != "catplay" {
            return Err("engine identity is not catplay".to_string());
        }
        // This probe is support evidence only: the patched CatPlay executable
        // deliberately reports false before spawn, so it is never live readiness.
        let _ = self.listener_ready;
        if !self.wireless_input || !self.wired_output || !self.input_only {
            return Err("engine does not support wireless input, wired output, and explicit input-only mode".to_string());
        }
        if !self.telemetry_socket.starts_with('/') || self.telemetry_socket.len() > 108 {
            return Err("engine telemetry socket is not a usable absolute AF_UNIX path".to_string());
        }
        Ok(())
    }
}

pub fn parse_capabilities(text: &str) -> Result<EngineCapabilities, String> {
    if text.len() > system::OUTPUT_CAP {
        return Err("capability handshake exceeds output cap".to_string());
    }
    let capability: EngineCapabilities =
        serde_json::from_str(text.trim()).map_err(|_| "capability handshake is not valid JSON".to_string())?;
    capability.validate()?;
    Ok(capability)
}

#[derive(Debug, Clone, Deserialize)]
pub struct CatPlaySnapshot {
    pub schema_version: u16,
    pub engine: String,
    pub lifecycle: String,
    pub wireless_listener_ready: bool,
    pub wired_endpoint_ready: bool,
    pub iphone_connected: bool,
    pub vehicle_connected: bool,
    pub bridge_active: bool,
    /// True when this record is from the UDC-free input-only engine mode.
    pub input_only: bool,
}

fn is_input_lifecycle(lifecycle: &str) -> bool {
    matches!(
        lifecycle,
        "input_initial"
            | "input_waiting_for_bluetooth"
            | "input_starting_bluetooth"
            | "input_powering_bluetooth"
            | "input_reconnecting"
            | "input_waiting_for_interface"
            | "input_waiting_for_interface_running"
            | "input_waiting_for_multicast"
            | "input_starting_listener"
            | "input_inviting"
            | "input_receiving"
            | "input_passive"
            | "input_error"
    )
}

fn is_input_ready_lifecycle(lifecycle: &str) -> bool {
    matches!(lifecycle, "input_inviting" | "input_receiving" | "input_passive")
}

impl CatPlaySnapshot {
    pub fn validate(&self) -> Result<(), String> {
        if self.schema_version != TELEMETRY_SCHEMA_VERSION || self.engine != "catplay" {
            return Err("invalid CatPlay telemetry identity".to_string());
        }
        match self.lifecycle.as_str() {
            "initial"
            | "waiting_for_udc"
            | "waiting_for_usb_transmitter_gadget"
            | "waiting_for_wireless_carplay_gadget"
            | "waiting_for_car"
            | "waiting_for_iphone"
            | "running"
            | "error"
            | "input_initial"
            | "input_waiting_for_bluetooth"
            | "input_starting_bluetooth"
            | "input_powering_bluetooth"
            | "input_reconnecting"
            | "input_waiting_for_interface"
            | "input_waiting_for_interface_running"
            | "input_waiting_for_multicast"
            | "input_starting_listener"
            | "input_inviting"
            | "input_receiving"
            | "input_passive"
            | "input_error" => {}
            _ => return Err("invalid CatPlay lifecycle".to_string()),
        }
        if is_input_lifecycle(&self.lifecycle) != self.input_only {
            return Err("CatPlay input_only flag does not match lifecycle".to_string());
        }
        if self.input_only && (self.wired_endpoint_ready || self.vehicle_connected || self.bridge_active) {
            return Err("contradictory CatPlay input-only lifecycle".to_string());
        }
        // A vehicle bridge joins the wireless iPhone side to the wired vehicle
        // endpoint. Do not accept a claimed bridge without both roles live.
        if self.bridge_active
            && (self.lifecycle != "running"
                || !self.wireless_listener_ready
                || !self.wired_endpoint_ready
                || !self.iphone_connected
                || !self.vehicle_connected)
        {
            return Err("contradictory CatPlay bridge lifecycle".to_string());
        }
        // Errors are terminal observations, never stale copies of live state.
        if matches!(self.lifecycle.as_str(), "error" | "input_error")
            && (self.wireless_listener_ready
                || self.wired_endpoint_ready
                || self.iphone_connected
                || self.vehicle_connected
                || self.bridge_active)
        {
            return Err("contradictory CatPlay error lifecycle".to_string());
        }
        Ok(())
    }

    /// Validate a record against the mode this sidecar used to launch CatPlay.
    /// The child is an external executable, so its self-reported `input_only`
    /// flag is evidence only after it agrees with the configured USB owner.
    pub fn validate_for_profile(&self, profile: UsbProfile) -> Result<(), String> {
        self.validate()?;
        match profile {
            UsbProfile::Development => {
                if !self.input_only {
                    return Err("development requires input_only=true".to_string());
                }
                if !is_input_lifecycle(&self.lifecycle) {
                    return Err("development requires an input-only lifecycle".to_string());
                }
                if self.wired_endpoint_ready || self.vehicle_connected || self.bridge_active {
                    return Err("development must not claim wired, vehicle, or bridge state".to_string());
                }
            }
            UsbProfile::Vehicle => {
                if self.input_only {
                    return Err("vehicle requires input_only=false".to_string());
                }
                if is_input_lifecycle(&self.lifecycle) {
                    return Err("vehicle must not report an input-only lifecycle".to_string());
                }
                // `validate` above retains the existing dual-role invariant:
                // any claimed bridge must be running with both live endpoints.
            }
        }
        Ok(())
    }

    /// A listener may be published only after a profile-compatible receiver
    /// has reached a live invitation/receive/passive state.
    pub fn wireless_ready_for_advertisement(&self) -> bool {
        self.wireless_listener_ready
            && if self.input_only {
                is_input_ready_lifecycle(&self.lifecycle)
            } else {
                matches!(self.lifecycle.as_str(), "waiting_for_car" | "waiting_for_iphone" | "running")
            }
    }
}

pub fn parse_snapshot(line: &[u8]) -> Result<CatPlaySnapshot, String> {
    if line.len() > CATPLAY_LINE_CAP {
        return Err("CatPlay telemetry line exceeds cap".to_string());
    }
    let snapshot: CatPlaySnapshot = serde_json::from_slice(line).map_err(|_| "CatPlay telemetry is not valid JSON".to_string())?;
    snapshot.validate()?;
    Ok(snapshot)
}

pub fn parse_snapshot_for_profile(line: &[u8], profile: UsbProfile) -> Result<CatPlaySnapshot, String> {
    let snapshot = parse_snapshot(line)?;
    snapshot
        .validate_for_profile(profile)
        .map_err(|error| format!("{PROFILE_MISMATCH_PREFIX}: {error}"))?;
    Ok(snapshot)
}

/// Bounded, single-stream lifecycle reader. A failed or malformed stream is
/// discarded wholesale: a future state is trusted only after a fresh line.
#[cfg(unix)]
pub struct CatPlayTelemetryClient {
    path: String,
    stream: Option<UnixStream>,
    partial: Vec<u8>,
    next_connect: Instant,
    connect_deadline: Instant,
    last_snapshot: Option<Instant>,
    connected_at: Option<Instant>,
}

#[cfg(unix)]
impl CatPlayTelemetryClient {
    pub fn new(path: String) -> Self {
        Self::new_at(path, Instant::now())
    }

    pub(crate) fn new_at(path: String, now: Instant) -> Self {
        Self {
            path,
            stream: None,
            partial: Vec::new(),
            next_connect: now,
            connect_deadline: now + HANDSHAKE_TIMEOUT,
            last_snapshot: None,
            connected_at: None,
        }
    }

    /// Start a fresh bounded connection window. Call this only after the AP
    /// preparation or engine spawn that gates it has completed.
    pub fn reset(&mut self) {
        self.reset_at(Instant::now());
    }

    fn reset_at(&mut self, now: Instant) {
        self.stream = None;
        self.partial.clear();
        self.next_connect = now;
        self.connect_deadline = now + HANDSHAKE_TIMEOUT;
        self.last_snapshot = None;
        self.connected_at = None;
    }

    fn disconnect_at(&mut self, now: Instant) {
        // EOF, malformed data, and I/O failures each get a new *bounded*
        // window. This avoids permanently disabling reconnect after long
        // uptime while still preventing an endless tight connect loop.
        self.stream = None;
        self.partial.clear();
        self.next_connect = now + TELEMETRY_CONNECT_BACKOFF;
        self.connect_deadline = now + HANDSHAKE_TIMEOUT;
        self.last_snapshot = None;
        self.connected_at = None;
    }

    /// Return accepted lifecycle observations. The returned vector is bounded
    /// by reads/tick and each read is fixed size; it never becomes a queue.
    /// Profile-bound stream read used by the serving loop. Generic reads remain
    /// available only for parser/transport tests; no live state uses them.
    pub fn poll_for_profile(&mut self, profile: UsbProfile) -> Result<Vec<CatPlaySnapshot>, String> {
        self.poll_for_profile_at(profile, Instant::now())
    }

    #[cfg(test)]
    pub(crate) fn poll_at(&mut self, now: Instant) -> Result<Vec<CatPlaySnapshot>, String> {
        self.poll_inner(None, now)
    }

    pub(crate) fn poll_for_profile_at(&mut self, profile: UsbProfile, now: Instant) -> Result<Vec<CatPlaySnapshot>, String> {
        self.poll_inner(Some(profile), now)
    }

    fn poll_inner(&mut self, profile: Option<UsbProfile>, now: Instant) -> Result<Vec<CatPlaySnapshot>, String> {
        if self.stream.is_none() && now >= self.next_connect && now <= self.connect_deadline {
            match UnixStream::connect(&self.path) {
                Ok(stream) => {
                    stream
                        .set_nonblocking(true)
                        .map_err(|e| format!("CatPlay telemetry nonblocking setup failed: {e}"))?;
                    self.stream = Some(stream);
                    self.connected_at = Some(now);
                }
                Err(_) => self.next_connect = now + TELEMETRY_CONNECT_BACKOFF,
            }
        }
        let Some(stream) = self.stream.as_mut() else { return Ok(Vec::new()) };
        let heartbeat_base = self.last_snapshot.or(self.connected_at).expect("connected stream has a start time");
        if now.duration_since(heartbeat_base) >= TELEMETRY_HEARTBEAT_TIMEOUT {
            self.disconnect_at(now);
            return Err("CatPlay telemetry heartbeat timed out".to_string());
        }
        let mut accepted = Vec::new();
        let mut buf = [0u8; 512];
        for _ in 0..TELEMETRY_READS_PER_TICK {
            match stream.read(&mut buf) {
                Ok(0) => {
                    self.disconnect_at(now);
                    return Err("CatPlay telemetry disconnected".to_string());
                }
                Ok(read) => {
                    // Consume byte-wise so retained partial-line memory never
                    // exceeds the protocol cap, even when reads straddle lines.
                    for byte in &buf[..read] {
                        if *byte == b'\n' {
                            let line = std::mem::take(&mut self.partial);
                            match match profile {
                                Some(profile) => parse_snapshot_for_profile(&line, profile),
                                None => parse_snapshot(&line),
                            } {
                                Ok(snapshot) => accepted.push(snapshot),
                                Err(error) => {
                                    self.disconnect_at(now);
                                    return Err(error);
                                }
                            }
                        } else if self.partial.len() == CATPLAY_LINE_CAP {
                            self.disconnect_at(now);
                            return Err("CatPlay telemetry line exceeds cap".to_string());
                        } else {
                            self.partial.push(*byte);
                        }
                    }
                }
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(error) => {
                    self.disconnect_at(now);
                    return Err(format!("CatPlay telemetry read failed: {error}"));
                }
            }
        }
        if !accepted.is_empty() {
            self.last_snapshot = Some(now);
        }
        Ok(accepted)
    }
}

pub fn handshake(cfg: &RuntimeConfig) -> Result<Option<EngineCapabilities>, String> {
    if cfg.engine == Engine::SelfTest {
        return Ok(None);
    }
    let output = system::run_capture(&cfg.catplay_engine_path, &["--cp-capabilities-json"], HANDSHAKE_TIMEOUT)
        .ok_or_else(|| format!("CatPlay engine '{}' is absent, not executable, or did not answer", cfg.catplay_engine_path))?;
    if !output.success {
        return Err(format!("CatPlay capability command failed: {}", output.stderr.trim()));
    }
    parse_capabilities(&output.stdout).map(Some)
}

/// Select the only protocol-engine mode permitted by each USB ownership
/// profile. Kept pure so no spawn path can accidentally claim the UDC in
/// development mode.
pub fn engine_argv(profile: UsbProfile) -> &'static [&'static str] {
    match profile {
        UsbProfile::Development => &["--cp-input-only"],
        UsbProfile::Vehicle => &["--cp-bridge"],
    }
}

pub fn restart_backoff(attempt: u32) -> Duration {
    ENGINE_RESTART_BASE.saturating_mul(1_u32 << attempt.min(4))
}

/// Minimal process owner used only after a successful handshake. Its caller
/// polls it from the existing single-threaded loop; no executor/thread pool is
/// introduced and restart attempts are capped.
pub struct EngineSupervisor {
    child: Option<Child>,
    restarts: u32,
    retry_after: Instant,
    just_exited: bool,
}

impl EngineSupervisor {
    pub fn new() -> Self {
        Self { child: None, restarts: 0, retry_after: Instant::now(), just_exited: false }
    }

    pub fn spawn(&mut self, cfg: &RuntimeConfig) -> Result<(), String> {
        if cfg.engine != Engine::CatPlay {
            return Ok(());
        }
        let child = Command::new(&cfg.catplay_engine_path)
            .args(engine_argv(cfg.usb_profile))
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|error| format!("cannot start CatPlay engine: {error}"))?;
        self.child = Some(child);
        Ok(())
    }

    pub fn poll(&mut self) -> bool {
        self.just_exited = false;
        let Some(child) = self.child.as_mut() else { return self.restarts < ENGINE_RESTART_CAP };
        if child.try_wait().ok().flatten().is_none() {
            return true;
        }
        self.child = None;
        self.just_exited = true;
        self.restarts = self.restarts.saturating_add(1);
        self.retry_after = Instant::now() + restart_backoff(self.restarts - 1);
        self.restarts < ENGINE_RESTART_CAP
    }

    pub fn take_exit(&mut self) -> bool {
        std::mem::take(&mut self.just_exited)
    }

    pub fn may_retry_now(&self) -> bool {
        self.restarts < ENGINE_RESTART_CAP && Instant::now() >= self.retry_after
    }
    pub fn needs_spawn(&self) -> bool {
        self.child.is_none()
    }

    pub fn stop(&mut self) {
        if let Some(child) = self.child.as_mut() {
            let _ = child.kill();
            let _ = child.wait();
        }
        self.child = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn handshake_is_support_only_and_still_validates_identity() {
        let good = r#"{"schema_version":1,"engine":"catplay","listener_ready":false,"wireless_input":true,"wired_output":true,"input_only":true,"telemetry_socket":"/run/zero2w/catplay.sock"}"#;
        assert!(parse_capabilities(good).is_ok());
        let fake = good.replace("\"engine\":\"catplay\"", "\"engine\":\"shell\"");
        assert!(parse_capabilities(&fake).is_err());
        assert!(parse_capabilities(&good.replace("\"wireless_input\":true", "\"wireless_input\":false")).is_err());
        assert!(parse_capabilities(&good.replace("\"input_only\":true", "\"input_only\":false")).is_err());
    }

    #[test]
    fn snapshot_requires_known_schema_identity_and_lifecycle() {
        let good = br#"{"schema_version":1,"engine":"catplay","lifecycle":"running","wireless_listener_ready":true,"wired_endpoint_ready":true,"iphone_connected":true,"vehicle_connected":true,"bridge_active":true,"input_only":false}"#;
        assert!(parse_snapshot(good).is_ok());
        assert!(parse_snapshot(&good[..CATPLAY_LINE_CAP.min(good.len())]).is_ok());
        let bad = String::from_utf8(good.to_vec()).unwrap().replace("running", "invented");
        assert!(parse_snapshot(bad.as_bytes()).is_err());
        let contradictory = String::from_utf8(good.to_vec())
            .unwrap()
            .replace("\"iphone_connected\":true", "\"iphone_connected\":false");
        assert!(parse_snapshot(contradictory.as_bytes()).is_err());
        let input_only = br#"{"schema_version":1,"engine":"catplay","lifecycle":"input_inviting","wireless_listener_ready":true,"wired_endpoint_ready":false,"iphone_connected":false,"vehicle_connected":false,"bridge_active":false,"input_only":true}"#;
        let input_only_snapshot = parse_snapshot(input_only).expect("truthful input-only record");
        assert!(input_only_snapshot.wireless_ready_for_advertisement());
        let no_listener = String::from_utf8(input_only.to_vec())
            .unwrap()
            .replace("\"wireless_listener_ready\":true", "\"wireless_listener_ready\":false");
        assert!(!parse_snapshot(no_listener.as_bytes()).unwrap().wireless_ready_for_advertisement());
        assert!(parse_snapshot(
            String::from_utf8(input_only.to_vec())
                .unwrap()
                .replace("\"wired_endpoint_ready\":false", "\"wired_endpoint_ready\":true")
                .as_bytes()
        )
        .is_err());
        assert!(parse_snapshot(
            String::from_utf8(input_only.to_vec())
                .unwrap()
                .replace("\"bridge_active\":false", "\"bridge_active\":true")
                .as_bytes()
        )
        .is_err());
        assert!(
            parse_snapshot(String::from_utf8(input_only.to_vec()).unwrap().replace(",\"input_only\":true", "").as_bytes()).is_err(),
            "missing input_only must fail closed"
        );
        assert!(
            parse_snapshot(
                String::from_utf8(input_only.to_vec())
                    .unwrap()
                    .replace("\"input_only\":true", "\"input_only\":false")
                    .as_bytes()
            )
            .is_err(),
            "input lifecycle must not claim input_only=false"
        );
        assert!(parse_snapshot(
            String::from_utf8(good.to_vec())
                .unwrap()
                .replace("\"wired_endpoint_ready\":true", "\"wired_endpoint_ready\":false")
                .as_bytes()
        )
        .is_err());

        let stale_error = String::from_utf8(good.to_vec()).unwrap().replace("running", "error");
        assert!(parse_snapshot(stale_error.as_bytes()).is_err(), "error must not retain live flags");
        let cleared_error = stale_error
            .replace("\"wireless_listener_ready\":true", "\"wireless_listener_ready\":false")
            .replace("\"wired_endpoint_ready\":true", "\"wired_endpoint_ready\":false")
            .replace("\"iphone_connected\":true", "\"iphone_connected\":false")
            .replace("\"vehicle_connected\":true", "\"vehicle_connected\":false")
            .replace("\"bridge_active\":true", "\"bridge_active\":false");
        assert!(parse_snapshot(cleared_error.as_bytes()).is_ok());
        assert!(parse_snapshot(&vec![b'x'; CATPLAY_LINE_CAP + 1]).is_err());
    }

    #[test]
    fn snapshots_are_fail_closed_against_the_launch_profile() {
        // Exact board record that blocked the prior attempt: the old artifact
        // omitted input_only. It must never be accepted implicitly.
        let board_without_input_only = br#"{"schema_version":1,"engine":"catplay","lifecycle":"input_inviting","wireless_listener_ready":true,"wired_endpoint_ready":false,"iphone_connected":false,"vehicle_connected":false,"bridge_active":false}"#;
        assert!(parse_snapshot_for_profile(board_without_input_only, UsbProfile::Development).is_err());

        let development = br#"{"schema_version":1,"engine":"catplay","lifecycle":"input_inviting","wireless_listener_ready":true,"wired_endpoint_ready":false,"iphone_connected":false,"vehicle_connected":false,"bridge_active":false,"input_only":true}"#;
        let ready = parse_snapshot_for_profile(development, UsbProfile::Development).expect("input invitation is valid in development");
        assert!(ready.wireless_ready_for_advertisement());
        assert!(parse_snapshot_for_profile(development, UsbProfile::Vehicle)
            .unwrap_err()
            .contains("vehicle requires input_only=false"));

        let input_lifecycle_false = String::from_utf8(development.to_vec())
            .unwrap()
            .replace("\"input_only\":true", "\"input_only\":false");
        assert!(parse_snapshot_for_profile(input_lifecycle_false.as_bytes(), UsbProfile::Development).is_err());
        assert!(parse_snapshot_for_profile(input_lifecycle_false.as_bytes(), UsbProfile::Vehicle).is_err());

        let output_claim = String::from_utf8(development.to_vec())
            .unwrap()
            .replace("\"wired_endpoint_ready\":false", "\"wired_endpoint_ready\":true");
        assert!(parse_snapshot_for_profile(output_claim.as_bytes(), UsbProfile::Development).is_err());

        let vehicle = br#"{"schema_version":1,"engine":"catplay","lifecycle":"running","wireless_listener_ready":true,"wired_endpoint_ready":true,"iphone_connected":true,"vehicle_connected":true,"bridge_active":true,"input_only":false}"#;
        assert!(parse_snapshot_for_profile(vehicle, UsbProfile::Vehicle).is_ok());
        assert!(parse_snapshot_for_profile(vehicle, UsbProfile::Development)
            .unwrap_err()
            .contains("development requires input_only=true"));
    }

    #[cfg(unix)]
    #[test]
    fn stream_rejection_and_heartbeat_timeout_are_clock_bounded() {
        use std::io::Write;
        use std::os::unix::net::UnixListener;

        let path = std::env::temp_dir().join(format!("cp-native-telemetry-{}", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path).expect("fake telemetry socket");
        let now = Instant::now();
        let mut client = CatPlayTelemetryClient::new_at(path.to_string_lossy().into_owned(), now);
        assert!(client.poll_at(now).expect("connect").is_empty());
        let (mut peer, _) = listener.accept().expect("accepted client");
        peer.write_all(b"{\"schema_version\":1,\"engine\":\"catplay\",\"lifecycle\":\"error\",\"wireless_listener_ready\":true,\"wired_endpoint_ready\":true,\"iphone_connected\":true,\"vehicle_connected\":true,\"bridge_active\":true,\"input_only\":false}\n")
            .expect("write stale error");
        assert!(client.poll_at(now).is_err(), "contradictory stream record is rejected");

        drop(peer);
        let listener = listener;
        let mut client = CatPlayTelemetryClient::new_at(path.to_string_lossy().into_owned(), now);
        assert!(client.poll_at(now).expect("connect").is_empty());
        let (_peer, _) = listener.accept().expect("accepted heartbeat client");
        let timeout = now + TELEMETRY_HEARTBEAT_TIMEOUT;
        assert!(client.poll_at(timeout).unwrap_err().contains("heartbeat timed out"));
        let _ = std::fs::remove_file(&path);
    }

    #[cfg(unix)]
    #[test]
    fn telemetry_reconnects_after_eof_and_malformed_input() {
        use std::io::Write;
        use std::os::unix::net::UnixListener;

        let path = std::env::temp_dir().join(format!("cp-native-telemetry-reconnect-{}", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path).expect("fake telemetry socket");
        let now = Instant::now();
        let mut client = CatPlayTelemetryClient::new_at(path.to_string_lossy().into_owned(), now);

        assert!(client.poll_at(now).expect("connect").is_empty());
        let (peer, _) = listener.accept().expect("accept EOF client");
        drop(peer);
        assert!(client.poll_at(now).unwrap_err().contains("disconnected"));
        let retry = now + TELEMETRY_CONNECT_BACKOFF;
        assert!(client.poll_at(retry).expect("reconnect after EOF").is_empty());
        let (mut peer, _) = listener.accept().expect("accept malformed client");
        peer.write_all(b"not-json\n").expect("write malformed record");
        assert!(client.poll_at(retry).unwrap_err().contains("not valid JSON"));
        assert!(client
            .poll_at(retry + TELEMETRY_CONNECT_BACKOFF)
            .expect("reconnect after malformed input")
            .is_empty());
        let (_peer, _) = listener.accept().expect("accept recovered client");
        assert!(client.stream.is_some());
        let _ = std::fs::remove_file(&path);
    }

    #[cfg(unix)]
    #[test]
    fn restart_reset_opens_a_fresh_window_after_long_backoff() {
        use std::os::unix::net::UnixListener;

        let path = std::env::temp_dir().join(format!("cp-native-telemetry-reset-{}", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path).expect("fake telemetry socket");
        let now = Instant::now();
        let mut client = CatPlayTelemetryClient::new_at(path.to_string_lossy().into_owned(), now);
        assert!(client.poll_at(now).expect("initial connect").is_empty());
        let (_old_peer, _) = listener.accept().expect("accept initial client");

        let restarted = now + Duration::from_secs(5);
        client.reset_at(restarted);
        assert!(client.poll_at(restarted).expect("fresh restart window").is_empty());
        let (_new_peer, _) = listener.accept().expect("accept reset client");
        assert!(client.stream.is_some());
        assert_eq!(client.connect_deadline, restarted + HANDSHAKE_TIMEOUT);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn engine_argv_is_exact_and_development_never_selects_bridge() {
        assert_eq!(engine_argv(UsbProfile::Development), ["--cp-input-only"]);
        assert_eq!(engine_argv(UsbProfile::Vehicle), ["--cp-bridge"]);
        assert!(!engine_argv(UsbProfile::Development).contains(&"--cp-bridge"));
    }

    #[test]
    fn engine_restart_backoff_is_capped() {
        assert_eq!(restart_backoff(0), ENGINE_RESTART_BASE);
        assert!(restart_backoff(2) > restart_backoff(1));
        assert_eq!(restart_backoff(99), restart_backoff(4));
    }
}
