//! Preflight, status and diagnostics: what the sidecar checks, and how it
//! reports what is real versus what is still missing.
//!
//! Every check is a pure function over a [`SystemSnapshot`], so the same rules
//! run off-target in tests and on the board from `cp-native preflight`.

use std::collections::BTreeMap;

use serde::Serialize;

use crate::ap::{hostapd_state, ApPaths};
use crate::bounds::REPORT_ITEM_CAP;
use crate::config::{ConfigSummary, Engine, RuntimeConfig};
use crate::mfi::{check_mfi_target, MfiDecision, SysfsView};
use crate::net::{check_ap_plan, dhcp_ready_for_ap, Finding, FindingSeverity, NetSnapshot};
use crate::telemetry::CarPlayTelemetry;

/// Capability state reported to any operator or UI. The real CarPlay session is
/// not implemented by this slice and must never be reported as available.
pub const CAP_IMPLEMENTED: &str = "implemented";
pub const CAP_NOT_IMPLEMENTED: &str = "not-implemented";
pub const CAP_EXTERNAL_TOOL: &str = "external-tool";

/// Binaries the AP launcher needs, with the Debian package that provides them.
pub const REQUIRED_COMMANDS: [(&str, &str); 7] = [
    ("ip", "iproute2"),
    ("iw", "iw"),
    ("nmcli", "network-manager"),
    ("hostapd", "hostapd"),
    ("hostapd_cli", "hostapd"),
    ("dnsmasq", "dnsmasq"),
    ("rfkill", "rfkill"),
];

/// Binaries the wider CarPlay path needs later; missing ones are warnings.
pub const RECOMMENDED_COMMANDS: [(&str, &str); 6] = [
    ("avahi-publish-service", "avahi-utils"),
    ("avahi-browse", "avahi-utils"),
    ("bluetoothctl", "bluez"),
    ("aplay", "alsa-utils"),
    ("arecord", "alsa-utils"),
    ("i2cdetect", "i2c-tools"),
];

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum CheckStatus {
    Ok,
    Warning,
    Missing,
}

impl CheckStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            CheckStatus::Ok => "ok",
            CheckStatus::Warning => "warning",
            CheckStatus::Missing => "missing",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Check {
    pub id: &'static str,
    pub required: bool,
    pub status: CheckStatus,
    pub detail: String,
}

/// One collected view of the outside world.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
pub struct SystemSnapshot {
    pub net: NetSnapshot,
    pub sysfs: SysfsView,
    /// Binary name -> resolved path, or absent when not on `PATH`.
    pub commands: BTreeMap<String, String>,
    /// Path -> exists.
    pub files: BTreeMap<String, bool>,
    /// Path -> first line, for small identity files.
    pub texts: BTreeMap<String, String>,
    pub rss_kib: u64,
    pub memory_total_kib: u64,
    pub memory_available_kib: u64,
    pub uname: String,
}

impl SystemSnapshot {
    pub fn command(&self, name: &str) -> Option<&str> {
        self.commands.get(name).map(String::as_str).filter(|p| !p.is_empty())
    }

    pub fn file_exists(&self, path: &str) -> bool {
        self.files.get(path).copied().unwrap_or(false)
    }

    pub fn text(&self, path: &str) -> Option<&str> {
        self.texts.get(path).map(String::as_str)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct PreflightReport {
    pub checks: Vec<Check>,
    pub findings: Vec<Finding>,
    pub mfi: MfiDecision,
    pub blocking: usize,
    pub install_hint: String,
}

impl PreflightReport {
    pub fn ok(&self) -> bool {
        self.blocking == 0
    }
}

/// Evaluate dependencies, MFi safety and network safety in one pass.
pub fn preflight(cfg: &RuntimeConfig, snap: &SystemSnapshot) -> PreflightReport {
    let mut checks: Vec<Check> = Vec::new();
    let mut missing_packages: Vec<&'static str> = Vec::new();

    let mut add = |check: Check, packages: &mut Vec<&'static str>| {
        if checks.len() >= REPORT_ITEM_CAP {
            return;
        }
        if check.status != CheckStatus::Ok {
            if let Some((_, pkg)) = package_for(check.id) {
                if !packages.contains(&pkg) {
                    packages.push(pkg);
                }
            }
        }
        checks.push(check);
    };

    for (name, pkg) in REQUIRED_COMMANDS {
        match snap.command(name) {
            Some(path) => add(Check { id: name, required: true, status: CheckStatus::Ok, detail: path.to_string() }, &mut missing_packages),
            None => add(
                Check {
                    id: name,
                    required: true,
                    status: CheckStatus::Missing,
                    detail: format!("'{name}' not found on PATH (package: {pkg})"),
                },
                &mut missing_packages,
            ),
        }
    }
    for (name, pkg) in RECOMMENDED_COMMANDS {
        match snap.command(name) {
            Some(path) => {
                add(Check { id: name, required: false, status: CheckStatus::Ok, detail: path.to_string() }, &mut missing_packages)
            }
            None => add(
                Check {
                    id: name,
                    required: false,
                    status: CheckStatus::Warning,
                    detail: format!("'{name}' not found on PATH (package: {pkg})"),
                },
                &mut missing_packages,
            ),
        }
    }

    add(
        if snap.file_exists(&cfg.mfi.tool) {
            Check { id: "mfi-tool", required: true, status: CheckStatus::Ok, detail: cfg.mfi.tool.clone() }
        } else {
            Check {
                id: "mfi-tool",
                required: true,
                status: CheckStatus::Missing,
                detail: format!("native MFi utility '{}' is missing; build Input/WirelessCarPlay/MFI and deploy it", cfg.mfi.tool),
            }
        },
        &mut missing_packages,
    );

    let paths = ApPaths::new(&cfg.runtime_dir);
    add(
        if snap.file_exists(&paths.runtime_dir) {
            Check { id: "runtime-dir", required: false, status: CheckStatus::Ok, detail: paths.runtime_dir.clone() }
        } else {
            // /run is tmpfs, so this is the normal state after every boot and the
            // launcher creates the directory before writing anything.
            Check {
                id: "runtime-dir",
                required: false,
                status: CheckStatus::Warning,
                detail: format!("'{}' does not exist yet; the launcher creates it before writing configs", paths.runtime_dir),
            }
        },
        &mut missing_packages,
    );

    add(
        if snap.file_exists("/dev/i2c-1") {
            Check { id: "i2c-dev", required: false, status: CheckStatus::Ok, detail: "/dev/i2c-1 present".to_string() }
        } else {
            Check {
                id: "i2c-dev",
                required: false,
                status: CheckStatus::Warning,
                detail: "/dev/i2c-1 is missing; the pi-i2c1 overlay is not enabled or the adapter renumbered".to_string(),
            }
        },
        &mut missing_packages,
    );

    let mfi = check_mfi_target(&snap.sysfs, cfg.mfi.target);
    add(
        Check {
            id: "mfi-policy",
            required: true,
            status: if mfi.allowed { CheckStatus::Ok } else { CheckStatus::Missing },
            detail: mfi.message.clone(),
        },
        &mut missing_packages,
    );
    if let Some(warning) = mfi.identity_warning.clone() {
        add(Check { id: "mfi-identity", required: false, status: CheckStatus::Warning, detail: warning }, &mut missing_packages);
    }

    let ac200 = check_mfi_target(&snap.sysfs, crate::mfi::MfiTarget { bus: crate::mfi::AC200_BUS, addr: 0x10 });
    add(
        Check {
            id: "ac200-refusal",
            required: true,
            status: if ac200.allowed { CheckStatus::Missing } else { CheckStatus::Ok },
            detail: ac200.message.clone(),
        },
        &mut missing_packages,
    );

    let findings = check_ap_plan(cfg, &snap.net);
    let blocking = findings.iter().filter(|f| f.severity == FindingSeverity::Blocking).count()
        + checks.iter().filter(|c| c.status == CheckStatus::Missing && c.required).count();

    let install_hint =
        if missing_packages.is_empty() { String::new() } else { format!("apt-get install -y {}", missing_packages.join(" ")) };

    PreflightReport { checks, findings, mfi, blocking, install_hint }
}

fn package_for(id: &str) -> Option<(&'static str, &'static str)> {
    REQUIRED_COMMANDS
        .iter()
        .chain(RECOMMENDED_COMMANDS.iter())
        .find(|(name, _)| *name == id)
        .map(|(name, pkg)| (*name, *pkg))
}

/// Live AP state as observed, not as intended.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ApState {
    pub iface: String,
    pub ssid: String,
    pub channel: u8,
    pub hostapd_state: String,
    pub dhcp_serving_ap_ip: bool,
    pub stations: Vec<String>,
}

/// What this build can and cannot do. Reported verbatim by `status` so no UI or
/// log reader can mistake the harness for a working CarPlay receiver.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Capability {
    pub engine: String,
    pub ap_management: &'static str,
    pub mfi_authentication: &'static str,
    pub airplay_publish: &'static str,
    pub carplay_session: &'static str,
    pub video: &'static str,
    pub audio: &'static str,
    pub microphone: &'static str,
    pub touch_hid: &'static str,
}

pub fn capability(cfg: &RuntimeConfig) -> Capability {
    Capability {
        engine: cfg.engine.as_str().to_string(),
        ap_management: CAP_IMPLEMENTED,
        mfi_authentication: CAP_EXTERNAL_TOOL,
        // Only the runtime engine handshake may promote this. An AP alone
        // does not publish an AirPlay service.
        airplay_publish: if cfg.engine == Engine::CatPlay { CAP_EXTERNAL_TOOL } else { CAP_NOT_IMPLEMENTED },
        carplay_session: CAP_NOT_IMPLEMENTED,
        video: CAP_NOT_IMPLEMENTED,
        audio: CAP_NOT_IMPLEMENTED,
        microphone: CAP_NOT_IMPLEMENTED,
        touch_hid: CAP_NOT_IMPLEMENTED,
    }
}

/// Counters kept by the running sidecar. All of them are bounded by design: the
/// event ring evicts, the frame cache has a hard cap, and nothing else grows.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
pub struct RuntimeCounters {
    pub uptime_secs: u64,
    pub ap_restarts: u32,
    pub commands_run: u64,
    pub commands_failed: u64,
    pub ipc_requests: u64,
    pub ipc_rejected: u64,
    pub events_pushed: u64,
    pub events_dropped: u64,
    pub frames_cached: usize,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct StatusReport {
    pub harness: &'static str,
    pub version: String,
    pub uname: String,
    pub rss_kib: u64,
    pub memory_available_kib: u64,
    pub config: ConfigSummary,
    pub capability: Capability,
    pub mfi: MfiDecision,
    pub ap: ApState,
    pub counters: RuntimeCounters,
    /// Stable state contract for the C++ core/web. Until an external engine
    /// supplies observations it contains truthful not-negotiated values.
    pub telemetry: CarPlayTelemetry,
    pub blocking_findings: usize,
}

/// Stations listed in one AP state report.
pub const STATION_REPORT_CAP: usize = 4;

/// Observe the AP: `hostapd_cli status` output plus the DHCP listener check that
/// is specific to the AP address.
pub fn ap_state(cfg: &RuntimeConfig, snap: &SystemSnapshot, hostapd_status: &str, stations: &[String]) -> ApState {
    ApState {
        iface: cfg.ap.iface.clone(),
        ssid: cfg.ap.ssid.clone(),
        channel: cfg.ap.channel,
        hostapd_state: hostapd_state(hostapd_status),
        dhcp_serving_ap_ip: dhcp_ready_for_ap(&snap.net, cfg.ap.ap_ip),
        stations: stations.iter().take(STATION_REPORT_CAP).cloned().collect(),
    }
}

pub fn status_report(
    cfg: &RuntimeConfig,
    snap: &SystemSnapshot,
    hostapd_status: &str,
    stations: &[String],
    counters: RuntimeCounters,
    version: &str,
    telemetry: CarPlayTelemetry,
) -> StatusReport {
    let preflight = preflight(cfg, snap);
    StatusReport {
        harness: crate::HARNESS_NAME,
        version: version.to_string(),
        uname: snap.uname.clone(),
        rss_kib: snap.rss_kib,
        memory_available_kib: snap.memory_available_kib,
        config: cfg.summary(),
        capability: capability(cfg),
        mfi: preflight.mfi,
        ap: ap_state(cfg, snap, hostapd_status, stations),
        counters,
        telemetry,
        blocking_findings: preflight.blocking,
    }
}

/// One bounded log/status event.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct EngineEvent {
    pub seq: u64,
    pub kind: &'static str,
    pub detail: String,
}

/// Truncate a detail string so one event can never balloon the ring.
pub fn bounded_detail(text: &str) -> String {
    const MAX: usize = 96;
    if text.len() <= MAX {
        text.to_string()
    } else {
        let mut end = MAX;
        while end > 0 && !text.is_char_boundary(end) {
            end -= 1;
        }
        format!("{}...", &text[..end])
    }
}

/// Human-readable rendering used by the non-`--json` output of every command.
pub fn render_preflight(report: &PreflightReport) -> String {
    let mut out = String::new();
    out.push_str("preflight\n");
    for c in &report.checks {
        out.push_str(&format!("  [{}] {:<20} {}{}\n", c.status.as_str(), c.id, c.detail, if c.required { "" } else { " (optional)" }));
    }
    if !report.findings.is_empty() {
        out.push_str("network\n");
        for f in &report.findings {
            out.push_str(&format!("  [{}] {:<28} {}\n", f.severity.as_str(), f.id, f.message));
        }
    }
    out.push_str(&format!("mfi: {}\n", report.mfi.message));
    if !report.install_hint.is_empty() {
        out.push_str(&format!("install: {}\n", report.install_hint));
    }
    let result = if report.ok() { "PASS".to_string() } else { format!("BLOCKED ({} blocking)", report.blocking) };
    out.push_str(&format!("result: {result}\n"));
    out
}

pub fn render_status(report: &StatusReport) -> String {
    let mut out = String::new();
    out.push_str(&format!("{} {} rss={}KiB avail={}KiB\n", report.harness, report.version, report.rss_kib, report.memory_available_kib));
    out.push_str(&format!(
        "ap: {} ch={} hostapd={} dhcp-for-ap={}\n",
        report.ap.iface, report.ap.channel, report.ap.hostapd_state, report.ap.dhcp_serving_ap_ip
    ));
    out.push_str(&format!("mfi: {}\n", report.mfi.message));
    out.push_str(&format!(
        "capability: carplay-session={} video={} audio={} mic={} touch={}\n",
        report.capability.carplay_session,
        report.capability.video,
        report.capability.audio,
        report.capability.microphone,
        report.capability.touch_hid
    ));
    out.push_str(&format!(
        "counters: uptime={}s ap-restarts={} cmds={}/{}-failed ipc={}/{}-rejected events={}/{}-dropped frames={}\n",
        report.counters.uptime_secs,
        report.counters.ap_restarts,
        report.counters.commands_run,
        report.counters.commands_failed,
        report.counters.ipc_requests,
        report.counters.ipc_rejected,
        report.counters.events_pushed,
        report.counters.events_dropped,
        report.counters.frames_cached
    ));
    out.push_str(&format!("blocking-findings: {}\n", report.blocking_findings));
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::{IfaceState, UdpEndpoint, UnitState};
    use std::net::Ipv4Addr;

    fn cfg() -> RuntimeConfig {
        RuntimeConfig::from_map(&[("CP_COUNTRY".to_string(), "US".to_string())].into_iter().collect::<BTreeMap<_, _>>()).expect("valid")
    }

    /// A board that is ready: every tool installed, debug link up, MFi clean.
    fn ready_board() -> SystemSnapshot {
        let mut commands = BTreeMap::new();
        for (name, _) in REQUIRED_COMMANDS.iter().chain(RECOMMENDED_COMMANDS.iter()) {
            commands.insert(name.to_string(), format!("/usr/bin/{name}"));
        }
        let mut files = BTreeMap::new();
        files.insert("/root/MFIAuth/auth3-native".to_string(), true);
        files.insert("/run/zero2w".to_string(), true);
        files.insert("/dev/i2c-1".to_string(), true);

        let mut sysfs = SysfsView::new();
        sysfs.insert("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@5002400\n");

        let mut ifaces = BTreeMap::new();
        ifaces.insert(
            "wlan0".to_string(),
            IfaceState { mac: "aa:bb:cc:dd:ee:ff".to_string(), ipv4: vec![], ipv6_link_local: true, up: true, nm_managed: Some(true) },
        );
        ifaces.insert(
            "usb0".to_string(),
            IfaceState {
                mac: "12:34:56:78:9a:bc".to_string(),
                ipv4: vec!["192.168.77.2/24".to_string()],
                ipv6_link_local: true,
                up: true,
                nm_managed: Some(false),
            },
        );
        let mut units = BTreeMap::new();
        units.insert("zero2w-usb-rndis.service".to_string(), UnitState { active: true, sub: "exited".to_string() });
        units.insert("zero2w-usb-dhcp.service".to_string(), UnitState { active: true, sub: "running".to_string() });

        SystemSnapshot {
            net: NetSnapshot {
                ifaces,
                units,
                udp_listeners: vec![UdpEndpoint { local_ip: Ipv4Addr::new(192, 168, 77, 2), port: 67 }],
                nm_unmanaged: vec![],
                regulatory_country: "US".to_string(),
                supported_channels: vec![36, 40, 44, 48],
                supports_vht80: Some(true),
            },
            sysfs,
            commands,
            files,
            texts: BTreeMap::new(),
            rss_kib: 1200,
            memory_total_kib: 1_000_000,
            memory_available_kib: 700_000,
            uname: "Linux zero2w 6.6 aarch64".to_string(),
        }
    }

    #[test]
    fn ready_board_passes_preflight() {
        let report = preflight(&cfg(), &ready_board());
        assert!(report.ok(), "{}", render_preflight(&report));
        assert!(report.mfi.allowed);
        assert!(report.install_hint.is_empty());
        assert!(report.findings.iter().all(|f| f.severity != FindingSeverity::Blocking));
    }

    #[test]
    fn missing_hostapd_blocks_and_names_its_package() {
        let mut snap = ready_board();
        snap.commands.remove("hostapd");
        snap.commands.remove("hostapd_cli");
        let report = preflight(&cfg(), &snap);
        assert!(!report.ok());
        assert_eq!(report.install_hint, "apt-get install -y hostapd");
        let missing: Vec<&str> = report.checks.iter().filter(|c| c.status == CheckStatus::Missing).map(|c| c.id).collect();
        assert_eq!(missing, vec!["hostapd", "hostapd_cli"]);
    }

    #[test]
    fn missing_avahi_is_only_a_warning() {
        let mut snap = ready_board();
        snap.commands.remove("avahi-publish-service");
        let report = preflight(&cfg(), &snap);
        assert!(report.ok(), "{}", render_preflight(&report));
        assert_eq!(report.install_hint, "apt-get install -y avahi-utils");
    }

    #[test]
    fn missing_mfi_tool_blocks() {
        let mut snap = ready_board();
        snap.files.insert("/root/MFIAuth/auth3-native".to_string(), false);
        let report = preflight(&cfg(), &snap);
        assert!(!report.ok());
        assert!(report.checks.iter().any(|c| c.id == "mfi-tool" && c.status == CheckStatus::Missing));
    }

    #[test]
    fn ac200_refusal_is_itself_a_check() {
        // The refusal of bus 2 must be visible in every report, not just implied.
        let report = preflight(&cfg(), &ready_board());
        let refusal = report.checks.iter().find(|c| c.id == "ac200-refusal").expect("check");
        assert_eq!(refusal.status, CheckStatus::Ok);
        assert!(refusal.detail.contains("AC200"), "{refusal:?}");
    }

    #[test]
    fn ac200_named_in_sysfs_still_refuses_bus_one() {
        let mut snap = ready_board();
        snap.sysfs.insert("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@5002c00\n");
        let report = preflight(&cfg(), &snap);
        assert!(!report.ok());
        assert!(!report.mfi.allowed);
    }

    #[test]
    fn debug_link_down_blocks_preflight() {
        let mut snap = ready_board();
        snap.net.units.get_mut("zero2w-usb-rndis.service").expect("unit").active = false;
        let report = preflight(&cfg(), &snap);
        assert!(!report.ok());
        assert!(report
            .findings
            .iter()
            .any(|f| f.id == "rndis-unit-inactive" && f.severity == FindingSeverity::Blocking));
    }

    #[test]
    fn capability_reports_the_session_as_unimplemented() {
        let cap = capability(&cfg());
        assert_eq!(cap.carplay_session, CAP_NOT_IMPLEMENTED);
        assert_eq!(cap.video, CAP_NOT_IMPLEMENTED);
        assert_eq!(cap.audio, CAP_NOT_IMPLEMENTED);
        assert_eq!(cap.microphone, CAP_NOT_IMPLEMENTED);
        assert_eq!(cap.touch_hid, CAP_NOT_IMPLEMENTED);
        assert_eq!(cap.mfi_authentication, CAP_EXTERNAL_TOOL);
        assert_eq!(cap.ap_management, CAP_IMPLEMENTED);
        assert_eq!(cap.engine, "selftest");
    }

    #[test]
    fn ap_state_reflects_observed_not_intended() {
        let mut snap = ready_board();
        let state = ap_state(&cfg(), &snap, "mode=AP\nstate=DISABLED\n", &[]);
        assert_eq!(state.hostapd_state, "DISABLED");
        assert!(!state.dhcp_serving_ap_ip);

        snap.net.udp_listeners.push(UdpEndpoint { local_ip: Ipv4Addr::new(10, 10, 0, 1), port: 67 });
        let state = ap_state(&cfg(), &snap, "state=ENABLED\n", &["aa:bb:cc:dd:ee:01".to_string()]);
        assert_eq!(state.hostapd_state, "ENABLED");
        assert!(state.dhcp_serving_ap_ip);
        assert_eq!(state.stations, vec!["aa:bb:cc:dd:ee:01".to_string()]);
    }

    #[test]
    fn station_list_is_bounded() {
        let many: Vec<String> = (0..20).map(|i| format!("sta-{i}")).collect();
        let state = ap_state(&cfg(), &ready_board(), "state=ENABLED\n", &many);
        assert_eq!(state.stations.len(), STATION_REPORT_CAP);
    }

    #[test]
    fn status_report_serializes_and_renders() {
        let report =
            status_report(&cfg(), &ready_board(), "state=ENABLED\n", &[], RuntimeCounters::default(), "0.1.0", CarPlayTelemetry::default());
        let json = serde_json::to_string(&report).expect("json");
        assert!(json.contains("\"carplay_session\":\"not-implemented\""), "{json}");
        assert!(!json.contains(&cfg().ap.passphrase), "{json}");
        let text = render_status(&report);
        assert!(text.contains("hostapd=ENABLED"), "{text}");
        assert!(text.contains("carplay-session=not-implemented"), "{text}");
    }

    #[test]
    fn detail_strings_are_bounded() {
        assert_eq!(bounded_detail("short"), "short");
        let long = "x".repeat(200);
        let bounded = bounded_detail(&long);
        assert!(bounded.len() <= 99, "{}", bounded.len());
        assert!(bounded.ends_with("..."));
    }

    #[test]
    fn detail_truncation_respects_char_boundaries() {
        let text = "é".repeat(120);
        let bounded = bounded_detail(&text);
        assert!(bounded.len() <= 102, "{}", bounded.len());
    }

    #[test]
    fn preflight_checks_are_bounded() {
        let report = preflight(&cfg(), &ready_board());
        assert!(report.checks.len() <= REPORT_ITEM_CAP);
        assert!(report.checks.len() >= REQUIRED_COMMANDS.len() + RECOMMENDED_COMMANDS.len());
    }

    #[test]
    fn render_preflight_shows_the_blocking_reason() {
        let mut snap = ready_board();
        snap.net.ifaces.remove("usb0");
        let text = render_preflight(&preflight(&cfg(), &snap));
        assert!(text.contains("BLOCKED"), "{text}");
        assert!(text.contains("rndis-iface-missing"), "{text}");
    }
}
