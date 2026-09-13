//! AP launcher: writes the planned configuration, takes the Wi-Fi interface over
//! from NetworkManager, starts `hostapd` + `dnsmasq` and supervises them.
//!
//! Every mutation is planned by [`cp_harness::ap`] first and gated by the
//! preflight/network checks, so `--dry-run` prints exactly what `ap-up` would do.
//! The supervision logic lives in [`Launcher::poll`] and is driven either by
//! `ap-up` (this file) or by the control-socket loop in `serve`, never both.

use std::process::{Child, Command as ProcCommand, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use cp_harness::ap::{self, ApPaths, Command as PlannedCommand};
use cp_harness::bounds::Ring;
use cp_harness::config::RuntimeConfig;
use cp_harness::net::{dhcp_ready_for_ap, Finding, NetSnapshot};

use crate::serve::ApObservation;
use crate::system;

/// Readiness budget for hostapd + dnsmasq after the interface is configured.
pub const READY_TIMEOUT: Duration = Duration::from_secs(20);
/// Pause between supervision ticks.
pub const SUPERVISION_TICK: Duration = Duration::from_millis(1000);
/// Initial pause before a restart attempt; retries increase exponentially.
pub const RESTART_DELAY: Duration = Duration::from_secs(2);
/// A radio process that repeatedly dies is a failed AP, not an infinite
/// restart loop that drains logs and CPU on a 1 GiB board.
pub const MAX_AP_RESTARTS: u32 = 3;
/// mDNS is non-critical but its respawn is still bounded and delayed.
pub const MAX_PUBLISHER_RESTARTS: u32 = 3;
/// Interval of the periodic AP status line, and of the station list refresh.
pub const STATUS_INTERVAL: Duration = Duration::from_secs(60);
/// Programs whose failure is tolerable: they clean up state that may not exist.
pub const BEST_EFFORT_PROGRAMS: [&str; 3] = ["nmcli", "systemctl", "rfkill"];
/// A malformed daemon cannot fill tmpfs through retained logs/leases. DHCP is
/// separately limited to 41 leases, so this is a generous diagnostic ceiling.
pub const AP_ARTIFACT_CAP: u64 = 64 * 1024;

/// Everything the launcher will write and run, as data.
pub struct ApPlan {
    pub paths: ApPaths,
    /// Files to write: `(path, content, mode)`.
    pub files: Vec<(String, String, u32)>,
    /// Commands to run before spawning, in order.
    pub setup: Vec<PlannedCommand>,
    pub hostapd: Vec<String>,
    pub dnsmasq: Vec<String>,
    /// Initial publisher state (normally none: readiness is live telemetry).
    pub avahi: Option<Vec<String>>,
    /// Guarded publisher argv retained for runtime lifecycle observations.
    publisher_argv: Option<Vec<String>>,
    /// Connection active before the AP took this interface from NetworkManager.
    pub previous_nm_connection: Option<String>,
    /// Existing unmanaged drop-in, restored exactly by rollback.
    pub previous_nm_dropin: Option<String>,
}

/// `advertise_airplay` is true only after the protocol engine has passed its
/// listener/capability handshake. AP management alone is never a reason to
/// advertise a receiver to a phone.
pub fn plan(cfg: &RuntimeConfig, device_id: &str, pk: &str, pi: &str, advertise_airplay: bool) -> ApPlan {
    let paths = ApPaths::new(&cfg.runtime_dir);
    let mac = system::iface_mac(cfg);
    let has_link_local = system::iface_has_link_local(&cfg.ap.iface);

    let files = vec![
        (paths.hostapd_conf.clone(), ap::hostapd_conf(cfg), 0o600u32),
        (paths.dnsmasq_conf.clone(), ap::dnsmasq_conf(cfg, &paths), 0o600u32),
        (paths.nm_unmanaged_conf.clone(), ap::nm_unmanaged_conf(&cfg.ap.iface), 0o644u32),
    ];

    let mut setup = ap::nm_release_commands(cfg);
    setup.extend(ap::interface_setup_commands(cfg));
    setup.extend(ap::link_local_commands(cfg, mac, has_link_local));

    let hostapd = ap::hostapd_argv(&paths);
    let dnsmasq = ap::dnsmasq_argv(&paths);
    // The local help probe is performed once while the plan is constructed.
    // Runtime polling only consumes this validated argv; it must never spawn a
    // capability probe after a publisher restart cap has been reached.
    let publisher_argv =
        (!device_id.is_empty() && system::avahi_supports_interface()).then(|| ap::avahi_publish_argv(cfg, device_id, pk, pi));

    ApPlan {
        previous_nm_connection: system::nm_active_connection(&cfg.ap.iface),
        previous_nm_dropin: system::read_text(&paths.nm_unmanaged_conf),
        paths,
        files,
        setup,
        hostapd,
        dnsmasq,
        // `avahi-publish-service --interface` is version-dependent. If the
        // installed binary cannot prove support, do not publish unscoped.
        avahi: advertise_airplay.then(|| publisher_argv.clone()).flatten(),
        publisher_argv,
    }
}

/// Human-readable dry-run rendering: what would be written and executed.
pub fn render(plan: &ApPlan, cfg: &RuntimeConfig, findings: &[Finding]) -> String {
    let mut out = String::new();
    out.push_str(&format!(
        "ap-plan for {} (iface {} channel {} width {}MHz ip {})\n",
        cfg.ap.ssid, cfg.ap.iface, cfg.ap.channel, cfg.ap.width_mhz, cfg.ap.ap_ip
    ));
    out.push_str("files\n");
    for (path, content, mode) in &plan.files {
        out.push_str(&format!("  {path} (mode {mode:o})\n"));
        for line in content.lines() {
            let shown = if line.starts_with("wpa_passphrase=") { "wpa_passphrase=<redacted>" } else { line };
            out.push_str(&format!("    | {shown}\n"));
        }
    }
    out.push_str("commands\n");
    for cmd in &plan.setup {
        out.push_str(&format!("  {}{}\n", cmd.display(), if is_best_effort(cmd) { "  (best effort)" } else { "" }));
    }
    out.push_str(&format!("  spawn {}\n", plan.hostapd.join(" ")));
    out.push_str(&format!("  spawn {}\n", plan.dnsmasq.join(" ")));
    if let Some(previous) = &plan.previous_nm_connection {
        out.push_str(&format!("  rollback NetworkManager connection {previous}\n"));
    } else {
        out.push_str("  rollback NetworkManager device management only (no previous connection captured)\n");
    }
    if let Some(avahi) = &plan.avahi {
        out.push_str(&format!("  spawn {}\n", avahi.join(" ")));
    } else {
        out.push_str("  no mDNS publish: no live wireless listener observation\n");
    }
    if !findings.is_empty() {
        out.push_str("findings\n");
        for f in findings {
            out.push_str(&format!("  [{}] {:<28} {}\n", f.severity.as_str(), f.id, f.message));
        }
    }
    out
}

fn is_best_effort(cmd: &PlannedCommand) -> bool {
    BEST_EFFORT_PROGRAMS.contains(&cmd.program.as_str())
}

/// Bounded log of executed commands.
pub struct CommandLog {
    pub entries: Ring<String>,
    pub run: u64,
    pub failed: u64,
}

impl CommandLog {
    pub fn new() -> Self {
        Self { entries: Ring::new(cp_harness::bounds::COMMAND_LOG_CAP), run: 0, failed: 0 }
    }

    /// Execute one planned command and record it.
    pub fn execute(&mut self, cmd: &PlannedCommand) -> bool {
        self.run = self.run.saturating_add(1);
        let status = ProcCommand::new(&cmd.program).args(&cmd.args).stdout(Stdio::null()).stderr(Stdio::null()).status();
        let ok = matches!(status, Ok(s) if s.success());
        if ok {
            self.entries.push(cmd.display());
        } else {
            self.failed = self.failed.saturating_add(1);
            self.entries.push(format!("FAILED {}", cmd.display()));
        }
        ok
    }

    /// Run a plan. Best-effort steps may fail; anything else must succeed.
    pub fn execute_all(&mut self, cmds: &[PlannedCommand]) -> bool {
        let mut ok = true;
        for cmd in cmds {
            let succeeded = self.execute(cmd);
            if !succeeded && !is_best_effort(cmd) {
                ok = false;
            }
        }
        ok
    }
}

/// The supervised child processes.
pub struct Children {
    pub hostapd: Child,
    pub dnsmasq: Child,
    pub avahi: Option<Child>,
}

impl Children {
    pub fn kill_all(&mut self) {
        let _ = self.hostapd.kill();
        let _ = self.dnsmasq.kill();
        if let Some(avahi) = self.avahi.as_mut() {
            let _ = avahi.kill();
        }
        let _ = self.hostapd.wait();
        let _ = self.dnsmasq.wait();
        if let Some(avahi) = self.avahi.as_mut() {
            let _ = avahi.wait();
        }
    }

    /// `Some(name)` when a child that carries the AP has exited.
    pub fn exited(&mut self) -> Option<&'static str> {
        if self.hostapd.try_wait().ok().flatten().is_some() {
            return Some("hostapd");
        }
        if self.dnsmasq.try_wait().ok().flatten().is_some() {
            return Some("dnsmasq");
        }
        None
    }

    /// Drop a dead mDNS publisher while leaving the radio processes alone.
    pub fn take_publisher_if_exited(&mut self) -> bool {
        if self.avahi.as_mut().and_then(|child| child.try_wait().ok().flatten()).is_some() {
            self.avahi = None;
            true
        } else {
            false
        }
    }
}

/// Mutable state of one bring-up, so `poll` can be called from any loop.
pub struct ApRuntime {
    pub children: Option<Children>,
    last_status: Instant,
    restart_not_before: Instant,
    publisher_not_before: Instant,
    publisher_restarts: u32,
    publisher_desired: bool,
    pub failed: bool,
}

impl ApRuntime {
    pub fn new() -> Self {
        let now = Instant::now();
        Self {
            children: None,
            last_status: now,
            restart_not_before: now,
            publisher_not_before: now,
            publisher_restarts: 0,
            publisher_desired: false,
            failed: false,
        }
    }

    #[cfg(all(test, unix))]
    pub(crate) fn publisher_desired(&self) -> bool {
        self.publisher_desired
    }
}

/// Capped exponential restart delay. The maximum is intentionally short: a
/// persistent failure transitions to failed after [`MAX_AP_RESTARTS`] attempts.
pub fn restart_backoff(attempt: u32) -> Duration {
    let shift = attempt.min(4);
    RESTART_DELAY.saturating_mul(1_u32 << shift)
}

/// Run the first publish attempt. A transient failure is retained as desired
/// state and given its first delayed retry rather than silently suppressing
/// mDNS forever.
fn launch_initial_publisher<T>(runtime: &mut ApRuntime, now: Instant, launch: impl FnOnce() -> Option<T>) -> Option<T> {
    let child = launch();
    if child.is_none() {
        runtime.publisher_not_before = now + restart_backoff(0);
    }
    child
}

/// Inject the launcher so failed-start scheduling is deterministic in tests;
/// production supplies the bounded `spawn` wrapper above.
fn launch_retry_publisher<T>(runtime: &mut ApRuntime, now: Instant, launch: impl FnOnce() -> Option<T>) -> Option<T> {
    if !runtime.publisher_desired || runtime.publisher_restarts >= MAX_PUBLISHER_RESTARTS || now < runtime.publisher_not_before {
        return None;
    }
    runtime.publisher_restarts = runtime.publisher_restarts.saturating_add(1);
    runtime.publisher_not_before = now + restart_backoff(runtime.publisher_restarts - 1);
    eprintln!("[ap] mDNS publisher retry {}/{}", runtime.publisher_restarts, MAX_PUBLISHER_RESTARTS);
    launch()
}

/// Use the argv validated during planning to make one publisher attempt. The
/// retry gate runs before the launch closure, so an absent child at the cap
/// cannot trigger another spawn or a capability probe on each poll.
fn launch_publisher<T>(
    runtime: &mut ApRuntime,
    now: Instant,
    argv: Option<&[String]>,
    initial: bool,
    launch: impl FnOnce(&[String]) -> Option<T>,
) -> Option<T> {
    if !runtime.publisher_desired || runtime.publisher_restarts >= MAX_PUBLISHER_RESTARTS || now < runtime.publisher_not_before {
        return None;
    }
    let argv = argv?;
    if initial {
        launch_initial_publisher(runtime, now, || launch(argv))
    } else {
        launch_retry_publisher(runtime, now, || launch(argv))
    }
}

pub struct Launcher {
    cfg: RuntimeConfig,
    plan: ApPlan,
    pub log: CommandLog,
    pub restarts: u32,
}

impl Launcher {
    pub fn new(cfg: RuntimeConfig, plan: ApPlan) -> Self {
        Self { cfg, plan, log: CommandLog::new(), restarts: 0 }
    }

    /// Blocking findings are evaluated by `preflight` before this launcher is
    /// constructed; see `cmd_ap_up` and `cmd_run` in `main.rs`.
    pub fn write_files(&self) -> std::io::Result<()> {
        std::fs::create_dir_all(&self.plan.paths.runtime_dir)?;
        for (path, content, mode) in &self.plan.files {
            if let Some(parent) = std::path::Path::new(path).parent() {
                std::fs::create_dir_all(parent)?;
            }
            // hostapd.conf contains the WPA passphrase. Write every generated
            // config atomically; the secret-bearing file is created as 0600
            // before content is written, so no partially written or world
            // readable passphrase file can be observed.
            write_atomic(path, content.as_bytes(), *mode)?;
        }
        prepare_bounded_artifact(&self.plan.paths.dnsmasq_leases, 0o600)?;
        prepare_bounded_artifact(&self.plan.paths.hostapd_log, 0o600)?;
        Ok(())
    }

    fn start_children(&mut self, runtime: &mut ApRuntime) -> std::io::Result<()> {
        if !self.log.execute_all(&self.plan.setup) {
            return Err(std::io::Error::other("required AP setup command failed"));
        }
        let dnsmasq = spawn(&self.plan.dnsmasq, None)?;
        let hostapd = spawn(&self.plan.hostapd, Some(&self.plan.paths.hostapd_log))?;
        // A listener can become ready while the AP is being restarted. Leave
        // mDNS absent here; `poll` applies the normal capped retry gate using
        // the argv capability already validated in the plan.
        runtime.children = Some(Children { hostapd, dnsmasq, avahi: None });
        Ok(())
    }

    /// Write the configuration and start the children once.
    pub fn bring_up(&mut self, runtime: &mut ApRuntime) -> Result<(), String> {
        self.write_files().map_err(|e| format!("cannot write AP configuration: {e}"))?;
        if let Err(error) = self.start_children(runtime) {
            self.rollback();
            return Err(format!("cannot start hostapd/dnsmasq: {error}"));
        }
        Ok(())
    }

    /// Restore the NetworkManager state captured before AP takeover. This is
    /// best effort: rollback must never hide the original AP launch failure.
    pub fn rollback(&mut self) {
        if let Some(previous) = &self.plan.previous_nm_dropin {
            let _ = write_atomic(&self.plan.paths.nm_unmanaged_conf, previous.as_bytes(), 0o644);
        } else {
            let _ = std::fs::remove_file(&self.plan.paths.nm_unmanaged_conf);
        }
        let managed = PlannedCommand::new("nmcli", &["device", "set", &self.cfg.ap.iface, "managed", "yes"]);
        let _ = self.log.execute(&managed);
        if let Some(connection) = &self.plan.previous_nm_connection {
            let reconnect = PlannedCommand::new("nmcli", &["connection", "up", "id", connection]);
            let _ = self.log.execute(&reconnect);
        }
    }

    fn respawn_publisher(&mut self, runtime: &mut ApRuntime, initial: bool) {
        let now = Instant::now();
        let child = launch_publisher(runtime, now, self.plan.publisher_argv.as_deref(), initial, |argv| spawn(argv, None).ok());
        if let Some(child) = child {
            if let Some(children) = runtime.children.as_mut() {
                children.avahi = Some(child);
            }
        }
    }

    /// mDNS is controlled solely by the latest valid CatPlay lifecycle state.
    /// A false observation immediately kills a previously published service.
    #[cfg_attr(not(unix), allow(dead_code))]
    pub fn set_wireless_listener_ready(&mut self, runtime: &mut ApRuntime, ready: bool) {
        if runtime.publisher_desired == ready {
            return;
        }
        runtime.publisher_desired = ready;
        runtime.publisher_restarts = 0;
        runtime.publisher_not_before = Instant::now();
        let Some(children) = runtime.children.as_mut() else { return };
        if !ready {
            if let Some(mut publisher) = children.avahi.take() {
                let _ = publisher.kill();
                let _ = publisher.wait();
            }
            return;
        }
        let missing = children.avahi.is_none();
        if missing {
            self.respawn_publisher(runtime, true);
        }
    }

    /// hostapd reports `ENABLED` and a DHCP server answers on the AP address.
    /// Deliberately cheap: no `nmcli`/`systemctl` fan-out while polling.
    pub fn ap_ready(&self) -> bool {
        if ap::hostapd_state(&system::hostapd_status(&self.cfg)) != "ENABLED" {
            return false;
        }
        let listeners = system::udp_listeners();
        dhcp_ready_for_ap(&NetSnapshot { udp_listeners: listeners, ..NetSnapshot::default() }, self.cfg.ap.ap_ip)
    }

    pub fn wait_ready(&self) -> bool {
        let deadline = Instant::now() + READY_TIMEOUT;
        while Instant::now() < deadline {
            if self.ap_ready() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(500));
        }
        false
    }

    /// One supervision step: restart a dead child, refresh the observed state.
    /// Cheap enough to call once per second from any loop.
    pub fn poll(&mut self, runtime: &mut ApRuntime) -> ApObservation {
        let mut observation = ApObservation {
            hostapd_state: ap::hostapd_state(&system::hostapd_status(&self.cfg)),
            stations: Vec::new(),
            restarts: self.restarts,
            commands_run: self.log.run,
            commands_failed: self.log.failed,
        };

        if let Some(children) = runtime.children.as_mut() {
            if let Some(name) = children.exited() {
                children.kill_all();
                runtime.children = None;
                if self.restarts >= MAX_AP_RESTARTS {
                    runtime.failed = true;
                    eprintln!("[ap] {name} exited; restart cap {MAX_AP_RESTARTS} reached, AP failed");
                } else {
                    self.restarts = self.restarts.saturating_add(1);
                    runtime.restart_not_before = Instant::now() + restart_backoff(self.restarts - 1);
                    eprintln!(
                        "[ap] {name} exited; retry {}/{} after {:?}",
                        self.restarts,
                        MAX_AP_RESTARTS,
                        restart_backoff(self.restarts - 1)
                    );
                }
                observation.hostapd_state.clear();
            } else if runtime.publisher_desired && children.take_publisher_if_exited() {
                eprintln!("[ap] mDNS publisher exited");
            }
        }
        // A failed initial spawn leaves `avahi` absent. Desired=true must
        // therefore schedule the same bounded retries as EOF.
        let publisher_missing = runtime.children.as_ref().is_some_and(|children| children.avahi.is_none());
        if runtime.publisher_desired && publisher_missing {
            self.respawn_publisher(runtime, false);
        }

        if runtime.children.is_none() && !runtime.failed && Instant::now() >= runtime.restart_not_before && self.restarts > 0 {
            if let Err(e) = self.start_children(runtime) {
                eprintln!("[ap] restart failed: {e}");
                runtime.failed = true;
            }
        }
        if runtime.failed {
            observation.hostapd_state = "FAILED".to_string();
        }

        if runtime.last_status.elapsed() >= STATUS_INTERVAL {
            runtime.last_status = Instant::now();
            observation.stations = system::stations(&self.cfg);
            println!(
                "[ap] state={} stations={} restarts={} commands={}/{}-failed",
                if observation.hostapd_state.is_empty() { "down" } else { &observation.hostapd_state },
                observation.stations.len(),
                self.restarts,
                self.log.run,
                self.log.failed
            );
        }

        observation.restarts = self.restarts;
        observation.commands_run = self.log.run;
        observation.commands_failed = self.log.failed;
        observation
    }

    /// Foreground supervision for `ap-up`. `on_tick` returning false stops it.
    pub fn supervise(&mut self, stop: &AtomicBool, mut on_tick: impl FnMut(&ApObservation) -> bool) -> i32 {
        let mut runtime = ApRuntime::new();
        if let Err(e) = self.bring_up(&mut runtime) {
            eprintln!("[ap] {e}");
            return 3;
        }
        if self.wait_ready() {
            println!(
                "[ap] up: ssid={} iface={} ip={} channel={} width={}MHz",
                self.cfg.ap.ssid, self.cfg.ap.iface, self.cfg.ap.ap_ip, self.cfg.ap.channel, self.cfg.ap.width_mhz
            );
        } else {
            eprintln!("[ap] readiness timeout after {}s; hostapd log: {}", READY_TIMEOUT.as_secs(), self.plan.paths.hostapd_log);
        }
        while !stop.load(Ordering::Relaxed) {
            let observation = self.poll(&mut runtime);
            if !on_tick(&observation) {
                break;
            }
            std::thread::sleep(SUPERVISION_TICK);
        }
        if let Some(children) = runtime.children.as_mut() {
            children.kill_all();
        }
        self.rollback();
        if runtime.failed {
            3
        } else {
            0
        }
    }
}

fn prepare_bounded_artifact(path: &str, mode: u32) -> std::io::Result<()> {
    let truncate = std::fs::metadata(path).map(|metadata| metadata.len() > AP_ARTIFACT_CAP).unwrap_or(false);
    if truncate || !std::path::Path::new(path).exists() {
        write_atomic(path, &[], mode)?;
    }
    set_mode(path, mode)
}

fn write_atomic(path: &str, contents: &[u8], mode: u32) -> std::io::Result<()> {
    use std::io::Write;
    let target = std::path::Path::new(path);
    let name = target.file_name().and_then(|n| n.to_str()).unwrap_or("cp-file");
    let tmp = target.with_file_name(format!(".{name}.{}.tmp", std::process::id()));
    let _ = std::fs::remove_file(&tmp);
    #[cfg(unix)]
    let mut file = {
        use std::os::unix::fs::OpenOptionsExt;
        std::fs::OpenOptions::new().write(true).create_new(true).mode(mode).open(&tmp)?
    };
    #[cfg(not(unix))]
    let mut file = std::fs::OpenOptions::new().write(true).create_new(true).open(&tmp)?;
    if let Err(error) = file.write_all(contents).and_then(|()| file.sync_all()) {
        drop(file);
        let _ = std::fs::remove_file(&tmp);
        return Err(error);
    }
    drop(file);
    if let Err(error) = set_mode(tmp.to_string_lossy().as_ref(), mode).and_then(|()| std::fs::rename(&tmp, target)) {
        let _ = std::fs::remove_file(&tmp);
        return Err(error);
    }
    Ok(())
}

fn spawn(argv: &[String], log_path: Option<&str>) -> std::io::Result<Child> {
    let (program, args) = argv
        .split_first()
        .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::InvalidInput, "empty argv"))?;
    let stdout = match log_path {
        Some(path) => Stdio::from(std::fs::File::create(path)?),
        None => Stdio::null(),
    };
    ProcCommand::new(program).args(args).stdout(stdout).stderr(Stdio::null()).spawn()
}

#[cfg(unix)]
fn set_mode(path: &str, mode: u32) -> std::io::Result<()> {
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode))
}

#[cfg(not(unix))]
fn set_mode(_path: &str, _mode: u32) -> std::io::Result<()> {
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    fn cfg() -> RuntimeConfig {
        RuntimeConfig::from_map(&[("CP_COUNTRY".to_string(), "US".to_string())].into_iter().collect::<BTreeMap<_, _>>()).expect("valid")
    }

    #[test]
    fn restart_backoff_is_bounded_and_monotonic() {
        assert_eq!(restart_backoff(0), RESTART_DELAY);
        assert!(restart_backoff(1) > restart_backoff(0));
        assert_eq!(restart_backoff(99), restart_backoff(4));
        assert_eq!(MAX_AP_RESTARTS, 3);
        assert_eq!(MAX_PUBLISHER_RESTARTS, 3);
    }

    #[test]
    fn plan_writes_three_scoped_files() {
        let c = cfg();
        let plan = plan(&c, "AA:BB:CC:DD:EE:FF", "", "", false);
        let paths: Vec<&str> = plan.files.iter().map(|(p, _, _)| p.as_str()).collect();
        assert!(paths.contains(&"/run/zero2w/cp-hostapd.conf"), "{paths:?}");
        assert!(paths.contains(&"/run/zero2w/cp-dnsmasq.conf"), "{paths:?}");
        assert!(paths.contains(&"/etc/NetworkManager/conf.d/99-zero2w-cp-ap-unmanaged.conf"), "{paths:?}");
        for (path, _, mode) in &plan.files {
            let expected = if path.contains("NetworkManager") { 0o644 } else { 0o600 };
            assert_eq!(*mode, expected, "{path}");
        }
    }

    #[test]
    fn plan_setup_starts_with_the_nm_takeover_and_never_names_the_debug_link() {
        let c = cfg();
        let plan = plan(&c, "AA:BB:CC:DD:EE:FF", "", "", false);
        assert_eq!(plan.setup[0].display(), "nmcli device set wlan0 managed no");
        assert!(plan.setup.iter().any(|cmd| cmd.display() == "iw reg set US"));
        assert!(plan.setup.iter().any(|cmd| cmd.display() == "ip addr add 10.10.0.1/24 dev wlan0"));
        for cmd in &plan.setup {
            assert!(!cmd.display().contains("usb0"), "{}", cmd.display());
            assert!(!cmd.display().contains("192.168.77"), "{}", cmd.display());
        }
        assert_eq!(plan.hostapd, vec!["hostapd".to_string(), "/run/zero2w/cp-hostapd.conf".to_string()]);
        assert!(plan.dnsmasq.iter().any(|a| a == "--keep-in-foreground"));
    }

    #[test]
    fn publisher_gate_changes_only_on_live_listener_observations() {
        let c = cfg();
        let mut launcher = Launcher::new(c.clone(), plan(&c, "AA:BB:CC:DD:EE:FF", "", "", false));
        let mut runtime = ApRuntime::new();
        assert!(!runtime.publisher_desired);
        launcher.set_wireless_listener_ready(&mut runtime, true);
        assert!(runtime.publisher_desired);
        launcher.set_wireless_listener_ready(&mut runtime, false);
        assert!(!runtime.publisher_desired);
    }

    #[test]
    fn publisher_production_path_retries_after_backoff_and_stops_at_cap() {
        let mut runtime = ApRuntime::new();
        let now = Instant::now();
        let argv = vec!["avahi-publish-service".to_string(), "--interface".to_string(), "wlan0".to_string()];
        runtime.publisher_desired = true;
        let mut spawns = 0;

        // A transient initial failure schedules a retry. The normal runtime
        // path will not call its launch closure before that deadline.
        assert!(launch_publisher(&mut runtime, now, Some(&argv), true, |_| {
            spawns += 1;
            None::<()>
        })
        .is_none());
        assert_eq!(spawns, 1);
        assert!(launch_publisher(&mut runtime, now, Some(&argv), false, |_| {
            spawns += 1;
            Some(())
        })
        .is_none());
        assert_eq!(spawns, 1, "backoff suppresses a spawn attempt");

        let first_retry = now + restart_backoff(0);
        assert!(launch_publisher(&mut runtime, first_retry, Some(&argv), false, |_| {
            spawns += 1;
            Some(())
        })
        .is_some());
        assert_eq!(runtime.publisher_restarts, 1);
        for _ in 1..MAX_PUBLISHER_RESTARTS {
            let at = runtime.publisher_not_before;
            assert!(launch_publisher(&mut runtime, at, Some(&argv), false, |_| {
                spawns += 1;
                None::<()>
            })
            .is_none());
        }
        assert_eq!(runtime.publisher_restarts, MAX_PUBLISHER_RESTARTS);
        let spawns_at_cap = spawns;
        for _ in 0..3 {
            assert!(launch_publisher(&mut runtime, now + Duration::from_secs(120), Some(&argv), false, |_| {
                spawns += 1;
                Some(())
            })
            .is_none());
        }
        assert_eq!(spawns, spawns_at_cap, "the cap suppresses every later poll without probing or spawning");
    }

    #[test]
    fn ap_only_plan_never_advertises_airplay() {
        let c = cfg();
        // The remaining runtime probe is intentionally outside this pure test;
        // even a valid device id is insufficient without a live receiver.
        assert!(plan(&c, "AA:BB:CC:DD:EE:FF", "", "", false).avahi.is_none());
        assert!(plan(&c, "", "", "", true).avahi.is_none());
    }

    #[test]
    fn best_effort_steps_are_marked() {
        let c = cfg();
        let plan = plan(&c, "AA:BB:CC:DD:EE:FF", "", "", false);
        let nm = plan.setup.iter().find(|cmd| cmd.program == "nmcli").expect("nmcli");
        assert!(is_best_effort(nm));
        let ip = plan.setup.iter().find(|cmd| cmd.program == "ip").expect("ip");
        assert!(!is_best_effort(ip));
    }

    #[test]
    fn render_redacts_the_passphrase() {
        let c = cfg();
        let plan = plan(&c, "AA:BB:CC:DD:EE:FF", "", "", false);
        let text = render(&plan, &c, &[]);
        assert!(!text.contains(&c.ap.passphrase), "{text}");
        assert!(text.contains("wpa_passphrase=<redacted>"), "{text}");
        assert!(text.contains("dd0800a0400000020021"), "{text}");
        assert!(text.contains("spawn hostapd /run/zero2w/cp-hostapd.conf"), "{text}");
        assert!(text.contains("(best effort)"), "{text}");
    }

    #[test]
    fn render_explains_a_missing_publisher() {
        let c = cfg();
        let plan = plan(&c, "", "", "", false);
        let text = render(&plan, &c, &[]);
        assert!(text.contains("no mDNS publish"), "{text}");
        assert!(text.contains("live wireless listener"), "{text}");
    }

    #[test]
    fn render_lists_findings() {
        let c = cfg();
        let plan = plan(&c, "AA:BB:CC:DD:EE:FF", "", "", false);
        let text = render(&plan, &c, &[Finding::blocking("test-id", "test message")]);
        assert!(text.contains("[blocking] test-id"), "{text}");
        assert!(text.contains("test message"), "{text}");
    }

    #[test]
    fn command_log_is_bounded_and_counts_failures() {
        let mut log = CommandLog::new();
        let total = cp_harness::bounds::COMMAND_LOG_CAP + 10;
        for _ in 0..total {
            log.execute(&PlannedCommand::new("cp-native-definitely-missing", &["x"]));
        }
        assert_eq!(log.entries.len(), cp_harness::bounds::COMMAND_LOG_CAP);
        assert_eq!(log.run, total as u64);
        assert_eq!(log.failed, total as u64);
        assert!(log.entries.iter().all(|e| e.starts_with("FAILED ")));
    }

    #[test]
    fn best_effort_failures_do_not_fail_the_plan() {
        let mut log = CommandLog::new();
        // nmcli is absent off-target, and that must not read as a failed plan.
        let ok = log.execute_all(&[PlannedCommand::new("nmcli", &["device", "set", "wlan0", "managed", "no"])]);
        assert!(ok, "best-effort step failed the plan: {:?}", log.entries.iter().collect::<Vec<_>>());
    }

    #[test]
    fn required_failures_fail_the_plan() {
        let mut log = CommandLog::new();
        let ok = log.execute_all(&[PlannedCommand::new("cp-native-definitely-missing", &["x"])]);
        assert!(!ok);
        assert_eq!(log.failed, 1);
    }
}
