//! `cp-native` - headless Zero2W Wireless-CarPlay sidecar.
//!
//! Process separation is the point of this binary. It carries the GPL-3.0 AP
//! logic ported from LIVI and, later, the CarPlay protocol engine. The C++20
//! core in `Core/` and `Input/WirelessCarPlay/` stays a separate process serving its
//! own web UI on port 8080; the only channel between them is the line-JSON
//! control socket. Nothing here links into the core and nothing there links
//! into this.
//!
//! This slice implements the harness, not the protocol: AP bring-up and
//! supervision, MFi bus safety, debug-link preservation, preflight, status and
//! diagnostics. `status` reports `carplay_session=not-implemented` until a real
//! engine exists.

mod ap;
mod cli;
mod engine;
mod serve;
mod system;

use std::net::Ipv4Addr;
use std::process::ExitCode;
use std::sync::atomic::AtomicBool;
use std::time::Duration;

use cp_harness::ap as ap_plan;
use cp_harness::config::{RuntimeConfig, UsbProfile};
use cp_harness::mfi::{check_mfi_target, MfiTarget, AC200_BUS};
use cp_harness::net::{check_ap_plan, Finding, FindingSeverity};
use cp_harness::status::{preflight, render_preflight, render_status, status_report, RuntimeCounters};

pub const EXIT_OK: u8 = 0;
pub const EXIT_BLOCKED: u8 = 1;
pub const EXIT_USAGE: u8 = 2;
pub const EXIT_RUNTIME: u8 = 3;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
/// Budget for one query against a running sidecar; `preflight` fans out to
/// several subprocesses, so it is generous.
pub const QUERY_TIMEOUT: Duration = Duration::from_secs(20);

/// One invocation of the verified native MFi utility.
#[derive(Debug, Clone, serde::Serialize)]
struct MfiToolRun {
    command: String,
    argv: String,
    success: bool,
    stdout: String,
    stderr: String,
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let parsed = match cli::parse(&args) {
        Ok(parsed) => parsed,
        Err(error) => {
            eprintln!("cp-native: {error}");
            eprint!("{USAGE}");
            return ExitCode::from(EXIT_USAGE);
        }
    };
    if parsed.command == cli::Command::Help {
        print!("{USAGE}");
        return ExitCode::from(EXIT_OK);
    }
    let env = match cli::collect_env(parsed.env_file.as_deref()) {
        Ok(env) => env,
        Err(error) => {
            eprintln!("cp-native: {error}");
            return ExitCode::from(EXIT_USAGE);
        }
    };
    let cfg = match RuntimeConfig::from_map(&env) {
        Ok(cfg) => cfg,
        Err(errors) => {
            eprintln!("cp-native: invalid configuration\n{errors}");
            return ExitCode::from(EXIT_BLOCKED);
        }
    };
    ExitCode::from(dispatch(&parsed, &cfg))
}

const USAGE: &str = cli::USAGE;

fn dispatch(cli: &cli::Cli, cfg: &RuntimeConfig) -> u8 {
    // One root gate: every command that can write, spawn or configure needs it,
    // and `--dry-run` never mutates so it stays usable for review.
    if !cli.command.read_only() && !cli.dry_run && !require_root(cli.command_name()) {
        return EXIT_RUNTIME;
    }
    match cli.command {
        cli::Command::Config => cmd_config(cli, cfg),
        cli::Command::Preflight => cmd_preflight(cli, cfg),
        cli::Command::ApPlan => cmd_ap_plan(cli, cfg),
        cli::Command::ApUp => cmd_ap_up(cli, cfg),
        cli::Command::MfiCheck => cmd_mfi_check(cli, cfg),
        cli::Command::Run => cmd_run(cli, cfg),
        cli::Command::Status => cmd_status(cli, cfg),
        cli::Command::Diag => cmd_diag(cli, cfg),
        cli::Command::Help => {
            print!("{USAGE}");
            EXIT_OK
        }
    }
}

fn emit_json<T: serde::Serialize>(value: &T) {
    match serde_json::to_string_pretty(value) {
        Ok(text) => println!("{text}"),
        Err(error) => println!("{{\"error\":\"cannot serialise report: {error}\"}}"),
    }
}

fn device_id(cfg: &RuntimeConfig) -> String {
    system::iface_mac(cfg).map(ap_plan::device_id_from_mac).unwrap_or_default()
}

fn require_root(command: &str) -> bool {
    match system::effective_uid() {
        Some(0) => true,
        Some(uid) => {
            eprintln!("cp-native: '{command}' needs root, running as uid {uid}");
            false
        }
        None => {
            eprintln!("cp-native: '{command}' needs root and the effective uid could not be read");
            false
        }
    }
}

fn live_socket(cli: &cli::Cli, cfg: &RuntimeConfig) -> Option<String> {
    let path = cli.socket.clone().unwrap_or_else(|| cfg.core_socket.clone());
    system::exists(&path).then_some(path)
}

fn render_config(cfg: &RuntimeConfig) -> String {
    let mut out = format!("cp-native {VERSION}\n");
    out.push_str(&format!("engine           {}\n", cfg.engine.as_str()));
    out.push_str(&format!(
        "ap               iface={} ssid={} channel={} width={}MHz country={} ip={} max-sta={}\n",
        cfg.ap.iface, cfg.ap.ssid, cfg.ap.channel, cfg.ap.width_mhz, cfg.ap.country, cfg.ap.ap_ip, cfg.ap.max_stations
    ));
    out.push_str(&format!("ap passphrase    {} bytes (redacted)\n", cfg.ap.passphrase.len()));
    out.push_str(&format!(
        "airplay          port={} pk={}B pi={}B srcvers={} name={}\n",
        cfg.airplay_port,
        cfg.airplay_pk.len(),
        cfg.airplay_pi.len(),
        cfg.source_version,
        cfg.device_name
    ));
    out.push_str(&format!(
        "mfi              {} @ 0x{:02X} tool={} power-gpio={}\n",
        cfg.mfi.target.dev_path(),
        cfg.mfi.target.addr,
        cfg.mfi.tool,
        cfg.mfi.power_gpio
    ));
    out.push_str(&format!(
        "debug link       iface={} ip={} required={} units={}\n",
        cfg.rndis.iface,
        cfg.rndis.ip,
        cfg.rndis.required,
        cfg.rndis.units.join(",")
    ));
    out.push_str(&format!("control socket   {}\n", cfg.core_socket));
    out.push_str(&format!("runtime dir      {}\n", cfg.runtime_dir));
    out.push_str(&format!(
        "limits           events={} media={} ipc-line={}B ipc-clients={}\n",
        cfg.limits.event_ring, cfg.limits.frame_cache, cfg.limits.ipc_line_bytes, cfg.limits.ipc_clients
    ));
    out
}

fn cmd_config(cli: &cli::Cli, cfg: &RuntimeConfig) -> u8 {
    if cli.json {
        emit_json(&cfg.summary());
    } else {
        print!("{}", render_config(cfg));
    }
    EXIT_OK
}

fn cmd_preflight(cli: &cli::Cli, cfg: &RuntimeConfig) -> u8 {
    let snapshot = system::collect(cfg);
    let report = preflight(cfg, &snapshot);
    if cli.json {
        emit_json(&report);
    } else {
        print!("{}", render_preflight(&report));
    }
    if report.ok() {
        EXIT_OK
    } else {
        EXIT_BLOCKED
    }
}

fn cmd_ap_plan(cli: &cli::Cli, cfg: &RuntimeConfig) -> u8 {
    let snapshot = system::collect(cfg);
    let findings = check_ap_plan(cfg, &snapshot.net);
    let blocking: Vec<&Finding> = findings.iter().filter(|f| f.severity == FindingSeverity::Blocking).collect();
    let plan = ap::plan(cfg, &device_id(cfg), &cfg.airplay_pk, &cfg.airplay_pi, false);

    if cli.json {
        emit_json(&serde_json::json!({
            "paths": plan.paths,
            "files": plan.files.iter().map(|(path, content, mode)| {
                let shown = if path.ends_with("hostapd.conf") {
                    content.lines().map(|l| if l.starts_with("wpa_passphrase=") { "wpa_passphrase=<redacted>" } else { l }).collect::<Vec<_>>().join("\n")
                } else {
                    content.clone()
                };
                serde_json::json!({ "path": path, "mode": format!("{mode:o}"), "content": shown })
            }).collect::<Vec<_>>(),
            "setup": plan.setup.iter().map(|cmd| serde_json::json!({
                "command": cmd.display(),
                "best_effort": cmd.program == "nmcli" || cmd.program == "systemctl" || cmd.program == "rfkill",
            })).collect::<Vec<_>>(),
            "spawn": { "hostapd": plan.hostapd, "dnsmasq": plan.dnsmasq, "avahi": plan.avahi },
            "device_id": device_id(cfg),
            "findings": findings,
            "blocking": blocking.len(),
        }));
    } else {
        print!("{}", ap::render(&plan, cfg, &findings));
    }
    if blocking.is_empty() {
        EXIT_OK
    } else {
        EXIT_BLOCKED
    }
}

fn cmd_ap_up(cli: &cli::Cli, cfg: &RuntimeConfig) -> u8 {
    let snapshot = system::collect(cfg);
    let report = preflight(cfg, &snapshot);
    if !report.ok() {
        if cli.json {
            emit_json(&report);
        } else {
            print!("{}", render_preflight(&report));
        }
        eprintln!("cp-native: ap-up refused, {} blocking finding(s)", report.blocking);
        return EXIT_BLOCKED;
    }
    let plan = ap::plan(cfg, &device_id(cfg), &cfg.airplay_pk, &cfg.airplay_pi, false);
    let mut launcher = ap::Launcher::new(cfg.clone(), plan);
    if cli.dry_run {
        println!("cp-native: ap-up --dry-run validated; nothing written, nothing spawned");
        return EXIT_OK;
    }
    launcher.supervise(&AtomicBool::new(false), |_| true) as u8
}

fn cmd_mfi_check(cli: &cli::Cli, cfg: &RuntimeConfig) -> u8 {
    let ac200_target = MfiTarget { bus: AC200_BUS, addr: 0x10 };
    let sysfs = system::collect_sysfs(&[cfg.mfi.target, ac200_target]);
    let decision = check_mfi_target(&sysfs, cfg.mfi.target);
    let ac200 = check_mfi_target(&sysfs, ac200_target);
    let mut code = if decision.allowed { EXIT_OK } else { EXIT_BLOCKED };
    let mut auth: Vec<MfiToolRun> = Vec::new();

    // --auth opens /dev/i2c-1 through the verified tool, so it needs root; the
    // sysfs policy check above does not.
    if cli.with_auth && decision.allowed && !require_root("mfi-check --auth") {
        return EXIT_RUNTIME;
    }
    if cli.with_auth && decision.allowed {
        // Read-only registers only: `info` and `selftest`. No challenge is
        // generated here; `auth3-native auth --random` stays an operator action.
        for command in ["info", "selftest"] {
            let argv = decision.tool_argv(&cfg.mfi.tool, command, true);
            let run = match system::run_argv(&argv, system::MFI_TOOL_TIMEOUT) {
                Some(output) => MfiToolRun {
                    command: command.to_string(),
                    argv: argv.join(" "),
                    success: output.success,
                    stdout: output.stdout.trim().to_string(),
                    stderr: output.stderr.trim().to_string(),
                },
                None => MfiToolRun {
                    command: command.to_string(),
                    argv: argv.join(" "),
                    success: false,
                    stdout: String::new(),
                    stderr: format!("'{}' did not run or timed out", cfg.mfi.tool),
                },
            };
            if !run.success {
                code = EXIT_RUNTIME;
            }
            auth.push(run);
        }
    }

    if cli.json {
        emit_json(&serde_json::json!({
            "dev_path": decision.target.dev_path(),
            "decision": decision,
            "ac200_guard": ac200,
            "auth": auth,
        }));
    } else {
        println!("mfi target   {} @ 0x{:02X}", decision.target.dev_path(), decision.target.addr);
        println!("mfi decision {}", decision.message);
        if !decision.adapter_identity.is_empty() {
            println!("adapter      {}", decision.adapter_identity.trim());
        }
        if let Some(warning) = &decision.identity_warning {
            println!("warning      {warning}");
        }
        println!("ac200 guard  {}", ac200.message);
        if cli.with_auth && !decision.allowed {
            println!("auth         skipped, the target is refused");
        }
        for run in &auth {
            let detail = if run.stdout.is_empty() { run.stderr.as_str() } else { run.stdout.as_str() };
            println!("{:<12} {} {}", run.command, if run.success { "ok     " } else { "FAILED " }, detail.replace('\n', " | "));
        }
    }
    code
}

/// The run-time ownership decision, kept separate from AP construction so a
/// development input-only run cannot even probe or construct radio/publisher
/// work when `--no-ap` was requested.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct RunPlan {
    starts_ap: bool,
    /// AP planning is separate from mandatory read-only safety. It is skipped
    /// for `--no-ap`, so that path cannot create radio or publisher work.
    checks_ap_mutation_preflight: bool,
    mutates_radio_or_publisher: bool,
}

#[derive(Debug, Clone, serde::Serialize)]
struct MandatorySafetyReport {
    findings: Vec<Finding>,
}

impl MandatorySafetyReport {
    fn ok(&self) -> bool {
        self.findings.iter().all(|finding| finding.severity != FindingSeverity::Blocking)
    }
}

/// Safety evidence required before *every* engine spawn. It is intentionally
/// narrower than AP preflight: it reads only fixed MFi/RNDIS evidence and never
/// invokes AP, mDNS, UDC, configfs, or radio mutation logic.
fn mandatory_safety_preflight(cfg: &RuntimeConfig, snapshot: &cp_harness::status::SystemSnapshot) -> MandatorySafetyReport {
    const REQUIRED_BUS: u32 = 1;
    const REQUIRED_ADDR: u8 = 0x10;
    const REQUIRED_RNDIS_IFACE: &str = "usb0";
    let required_rndis_ip = Ipv4Addr::new(192, 168, 77, 2);
    let required_target = MfiTarget { bus: REQUIRED_BUS, addr: REQUIRED_ADDR };
    let mut findings = Vec::new();
    let mut block = |id, message| findings.push(Finding::blocking(id, message));

    if cfg.mfi.target != required_target {
        block(
            "mfi-target-not-exact",
            format!(
                "CatPlay launch requires /dev/i2c-{REQUIRED_BUS} @ 0x{REQUIRED_ADDR:02X}, not {} @ 0x{:02X}",
                cfg.mfi.target.dev_path(),
                cfg.mfi.target.addr
            ),
        );
    }
    if !snapshot.file_exists(&required_target.dev_path()) {
        block("mfi-device-missing", format!("required {} is absent", required_target.dev_path()));
    }
    let allowed = check_mfi_target(&snapshot.sysfs, required_target);
    if !allowed.allowed {
        block("mfi-policy-refused", allowed.message);
    }
    let ac200 = check_mfi_target(&snapshot.sysfs, MfiTarget { bus: AC200_BUS, addr: REQUIRED_ADDR });
    if ac200.allowed {
        block("ac200-refusal-failed", "AC200 /dev/i2c-2 must be refused before engine spawn".to_string());
    }

    if cfg.usb_profile == UsbProfile::Development {
        if !cfg.rndis.required || cfg.rndis.iface != REQUIRED_RNDIS_IFACE || cfg.rndis.ip != required_rndis_ip {
            block(
                "rndis-config-not-exact",
                format!("development requires RNDIS {REQUIRED_RNDIS_IFACE}/{required_rndis_ip} with CP_REQUIRE_RNDIS=true"),
            );
        }
        match snapshot.net.iface(REQUIRED_RNDIS_IFACE) {
            Some(rndis)
                if rndis.up
                    && rndis
                        .ipv4
                        .iter()
                        .any(|address| address.split('/').next().unwrap_or(address) == required_rndis_ip.to_string()) => {}
            Some(_) => block("rndis-unavailable", format!("development requires live RNDIS {REQUIRED_RNDIS_IFACE}/{required_rndis_ip}")),
            None => block("rndis-unavailable", format!("development requires live RNDIS {REQUIRED_RNDIS_IFACE}/{required_rndis_ip}")),
        }
    }
    MandatorySafetyReport { findings }
}

fn run_plan(_profile: UsbProfile, no_ap: bool) -> RunPlan {
    if no_ap {
        RunPlan { starts_ap: false, checks_ap_mutation_preflight: false, mutates_radio_or_publisher: false }
    } else {
        RunPlan { starts_ap: true, checks_ap_mutation_preflight: true, mutates_radio_or_publisher: true }
    }
}

fn cmd_run(cli: &cli::Cli, cfg: &RuntimeConfig) -> u8 {
    let plan = run_plan(cfg.usb_profile, cli.no_ap);
    // This is deliberately before capability handshake/spawn and is required
    // even for development --no-ap. The collection is read-only and does not
    // construct an AP launcher or touch radio/publisher/UDC state.
    let mandatory = mandatory_safety_preflight(cfg, &system::collect_mandatory_safety());
    if !mandatory.ok() {
        if cli.json || cli.dry_run {
            emit_json(&mandatory);
        } else {
            eprintln!("cp-native: run refused by mandatory safety preflight: {mandatory:?}");
        }
        return EXIT_BLOCKED;
    }
    if plan.checks_ap_mutation_preflight {
        let snapshot = system::collect(cfg);
        let report = preflight(cfg, &snapshot);
        if cli.json || cli.dry_run {
            emit_json(&report);
        } else {
            print!("{}", render_preflight(&report));
        }
        if cli.dry_run {
            return if report.ok() { EXIT_OK } else { EXIT_BLOCKED };
        }
        if !report.ok() {
            eprintln!("cp-native: run refused, {} blocking finding(s)", report.blocking);
            return EXIT_BLOCKED;
        }
    } else if cli.dry_run {
        println!("cp-native: development --no-ap --dry-run runs only read-only MFi/RNDIS safety checks; it does not probe or mutate AP, UDC, configfs, or the Wi-Fi radio");
        return EXIT_OK;
    }
    // The capability command only proves support and the intended socket path.
    // A live listener is accepted later, from schema-v1 lifecycle telemetry.
    let mut engine = engine::EngineSupervisor::new();
    let telemetry_path = if cfg.engine == cp_harness::config::Engine::CatPlay {
        match engine::handshake(cfg) {
            Ok(Some(capabilities)) => match engine.spawn(cfg) {
                Ok(()) => Some(capabilities.telemetry_socket),
                Err(error) => {
                    eprintln!("cp-native: {error}");
                    return EXIT_RUNTIME;
                }
            },
            Ok(None) => None,
            Err(error) => {
                eprintln!("cp-native: CatPlay engine refused: {error}");
                return EXIT_BLOCKED;
            }
        }
    } else {
        None
    };
    let mut server = serve::Server::new(cfg.clone());
    let mut launcher = if plan.starts_ap {
        Some(ap::Launcher::new(cfg.clone(), ap::plan(cfg, &device_id(cfg), &cfg.airplay_pk, &cfg.airplay_pi, false)))
    } else {
        debug_assert!(!plan.mutates_radio_or_publisher);
        println!("cp-native: --no-ap, the Wi-Fi radio and mDNS publisher are left alone");
        None
    };
    match serve::serve(
        &mut server,
        launcher.as_mut(),
        telemetry_path.as_deref(),
        telemetry_path.as_ref().map(|_| &mut engine),
        &AtomicBool::new(false),
    ) {
        Ok(code) => code as u8,
        Err(error) => {
            engine.stop();
            eprintln!("cp-native: control socket failed: {error}");
            EXIT_RUNTIME
        }
    }
}

fn cmd_status(cli: &cli::Cli, cfg: &RuntimeConfig) -> u8 {
    if let Some(socket) = live_socket(cli, cfg) {
        return match serve::query(&socket, "{\"op\":\"status\",\"id\":1}", QUERY_TIMEOUT) {
            Ok(line) => {
                println!("{line}");
                if cli.follow {
                    follow_events(&socket)
                } else {
                    EXIT_OK
                }
            }
            Err(error) => {
                eprintln!("cp-native: cannot query {socket}: {error}");
                EXIT_RUNTIME
            }
        };
    }
    let snapshot = system::collect(cfg);
    let report = status_report(
        cfg,
        &snapshot,
        &system::hostapd_status(cfg),
        &system::stations(cfg),
        RuntimeCounters::default(),
        VERSION,
        cp_harness::telemetry::CarPlayTelemetry::default(),
    );
    if cli.json {
        emit_json(&report);
    } else {
        print!("{}", render_status(&report));
        println!("note: no sidecar is running at {}; this is a one-off live snapshot", cfg.core_socket);
    }
    EXIT_OK
}

fn cmd_diag(cli: &cli::Cli, cfg: &RuntimeConfig) -> u8 {
    if let Some(socket) = live_socket(cli, cfg) {
        return match serve::query(&socket, "{\"op\":\"diag\",\"id\":1}", QUERY_TIMEOUT) {
            Ok(line) => {
                println!("{line}");
                if cli.follow {
                    follow_events(&socket)
                } else {
                    EXIT_OK
                }
            }
            Err(error) => {
                eprintln!("cp-native: cannot query {socket}: {error}");
                EXIT_RUNTIME
            }
        };
    }
    // No running sidecar: report the bounds this process would use, so the
    // command is still useful for reviewing a configuration.
    let server = serve::Server::new(cfg.clone());
    let report = server.diag_report();
    if cli.json {
        emit_json(&report);
    } else {
        println!("cp-native {VERSION} (not running, planned bounds)");
        println!(
            "bounds     events={}/{} media={}/{} ipc-clients={}/{} ipc-request={}B ipc-reply={}B outbox={} command-log={}/{}",
            report.bounds.event_ring_used,
            report.bounds.event_ring_cap,
            report.bounds.media_cache_used,
            report.bounds.media_cache_cap,
            report.bounds.ipc_clients_used,
            report.bounds.ipc_clients_cap,
            report.bounds.ipc_line_bytes,
            report.bounds.ipc_reply_bytes,
            report.bounds.outbox_cap,
            report.bounds.command_log_used,
            report.bounds.command_log_cap
        );
        println!("memory     rss={}KiB available={}KiB", report.rss_kib, report.memory_available_kib);
        for event in &report.events {
            println!("event      #{} {} {}", event.seq, event.kind, event.detail);
        }
    }
    EXIT_OK
}

fn follow_events(socket: &str) -> u8 {
    match serve::follow(socket, "{\"op\":\"events\",\"id\":2,\"follow\":true}", |line| println!("{line}")) {
        Ok(()) => EXIT_OK,
        Err(error) => {
            eprintln!("cp-native: event stream ended: {error}");
            EXIT_RUNTIME
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    fn cfg() -> RuntimeConfig {
        RuntimeConfig::from_map(&[("CP_COUNTRY".to_string(), "US".to_string())].into_iter().collect::<BTreeMap<_, _>>()).expect("valid")
    }

    fn safe_development_snapshot() -> cp_harness::status::SystemSnapshot {
        let mut snapshot = cp_harness::status::SystemSnapshot::default();
        snapshot.files.insert("/dev/i2c-1".to_string(), true);
        snapshot.sysfs.insert("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@5002400\n");
        snapshot.net.ifaces.insert(
            "usb0".to_string(),
            cp_harness::net::IfaceState { ipv4: vec!["192.168.77.2/24".to_string()], up: true, ..cp_harness::net::IfaceState::default() },
        );
        snapshot
    }

    #[test]
    fn development_no_ap_plan_has_no_radio_or_publisher_work_but_keeps_mandatory_safety() {
        let plan = run_plan(UsbProfile::Development, true);
        assert!(!plan.starts_ap);
        assert!(!plan.checks_ap_mutation_preflight, "no AP run must not plan AP mutation work");
        assert!(!plan.mutates_radio_or_publisher, "no AP run must not mutate the radio or mDNS publisher");
        assert!(mandatory_safety_preflight(&cfg(), &safe_development_snapshot()).ok());

        let vehicle = run_plan(UsbProfile::Vehicle, true);
        assert!(!vehicle.starts_ap);
        assert!(!vehicle.checks_ap_mutation_preflight, "--no-ap never performs AP mutation preflight");
    }

    #[test]
    fn development_no_ap_mandatory_safety_refuses_unsafe_mfi_and_missing_rndis() {
        let mut unsafe_mfi = cfg();
        unsafe_mfi.mfi.target = MfiTarget { bus: AC200_BUS, addr: 0x10 };
        let report = mandatory_safety_preflight(&unsafe_mfi, &safe_development_snapshot());
        assert!(!report.ok());
        assert!(report.findings.iter().any(|finding| finding.id == "mfi-target-not-exact"));

        let mut absent_rndis = safe_development_snapshot();
        absent_rndis.net.ifaces.remove("usb0");
        let report = mandatory_safety_preflight(&cfg(), &absent_rndis);
        assert!(!report.ok());
        assert!(report.findings.iter().any(|finding| finding.id == "rndis-unavailable"));
    }

    #[test]
    fn config_rendering_redacts_the_passphrase_and_pins_the_bus() {
        let c = cfg();
        let text = render_config(&c);
        assert!(!text.contains(&c.ap.passphrase), "{text}");
        assert!(text.contains("mfi              /dev/i2c-1 @ 0x10"), "{text}");
        assert!(text.contains("debug link       iface=usb0 ip=192.168.77.2 required=true"), "{text}");
        assert!(text.contains("engine           selftest"), "{text}");
        assert!(text.contains("limits           events=128 media=4 ipc-line=1024B ipc-clients=2"), "{text}");
    }

    #[test]
    fn ac200_is_refused_by_the_check_command_path() {
        let c = cfg();
        let ac200 =
            check_mfi_target(&system::collect_sysfs(&[MfiTarget { bus: AC200_BUS, addr: 0x10 }]), MfiTarget { bus: AC200_BUS, addr: 0x10 });
        assert!(!ac200.allowed, "{ac200:?}");
        assert!(ac200.message.contains("AC200"), "{ac200:?}");
        assert_ne!(c.mfi.target.bus, AC200_BUS);
    }

    #[test]
    fn a_missing_socket_falls_back_to_a_live_snapshot() {
        let c = cfg();
        let cli = cli::parse(&["status".to_string()]).expect("ok");
        assert_eq!(live_socket(&cli, &c), None, "no sidecar is running during tests");
    }

    #[test]
    fn help_is_the_default_and_is_read_only() {
        let cli = cli::parse(&[]).expect("ok");
        assert_eq!(cli.command, cli::Command::Help);
        assert!(cli.command.read_only());
        assert_eq!(dispatch(&cli, &cfg()), EXIT_OK);
    }

    #[test]
    fn config_command_succeeds_for_a_valid_configuration() {
        let cli = cli::parse(&["config".to_string()]).expect("ok");
        assert_eq!(dispatch(&cli, &cfg()), EXIT_OK);
    }

    #[test]
    fn exit_codes_are_distinct() {
        assert_eq!([EXIT_OK, EXIT_BLOCKED, EXIT_USAGE, EXIT_RUNTIME], [0, 1, 2, 3]);
    }
}
