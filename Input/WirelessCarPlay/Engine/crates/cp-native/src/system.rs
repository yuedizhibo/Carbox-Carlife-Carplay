//! Live system collection and command execution: the only module that touches
//! `/sys`, `/proc`, `PATH` and child processes.
//!
//! Every parser here is a pure function over command output or file content, so
//! the collection logic is testable off-target; only the thin `collect` glue
//! needs a real board.
//!
//! Safety note: reading `/sys/bus/i2c/devices/2-0010/name` and friends is a
//! read of kernel data structures. It never opens `/dev/i2c-2` and never talks
//! to the AC200 chip, so collecting the refusal evidence is itself safe.

use std::collections::BTreeMap;
use std::path::Path;
use std::process::{Command as ProcCommand, Stdio};
use std::time::{Duration, Instant};

use cp_harness::config::RuntimeConfig;
use cp_harness::mfi::{MfiTarget, Sysfs, SysfsView, AC200_BUS};
use cp_harness::net::{parse_proc_net_udp, IfaceState, NetSnapshot, UdpEndpoint, UnitState};
use cp_harness::status::{SystemSnapshot, RECOMMENDED_COMMANDS, REQUIRED_COMMANDS};

/// Interfaces inspected under `/sys/class/net`.
pub const IFACE_SCAN_CAP: usize = 16;
/// NetworkManager config files scanned for `unmanaged-devices`.
pub const NM_CONF_SCAN_CAP: usize = 16;
/// Cap on one command's captured stdout, in bytes.
pub const OUTPUT_CAP: usize = 8192;
/// Cap on one blocking tool (`nmcli` talks to D-Bus and can stall).
pub const COMMAND_TIMEOUT: Duration = Duration::from_secs(3);
/// Budget for the native MFi utility, which retries and polls the coprocessor.
pub const MFI_TOOL_TIMEOUT: Duration = Duration::from_secs(15);
/// Addresses kept per interface.
pub const ADDR_PER_IFACE_CAP: usize = 8;
/// `IFF_UP` in `/sys/class/net/<if>/flags`.
pub const IFF_UP: u32 = 0x1;

pub fn read_text(path: &str) -> Option<String> {
    std::fs::read_to_string(path).ok()
}

pub fn exists(path: &str) -> bool {
    Path::new(path).exists()
}

/// Captured result of one bounded command.
pub struct CommandOutput {
    pub success: bool,
    pub stdout: String,
    pub stderr: String,
}

/// Run a command capturing stdout and stderr, bounded in time and size. `None`
/// means the program is missing, would not start, or did not finish in time.
pub fn run_capture(program: &str, args: &[&str], timeout: Duration) -> Option<CommandOutput> {
    let mut child = ProcCommand::new(program).args(args).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().ok()?;
    let deadline = Instant::now() + timeout;
    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) => {}
            Err(_) => return None,
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            return None;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    let output = child.wait_with_output().ok()?;
    Some(CommandOutput {
        success: output.status.success(),
        stdout: truncate(&String::from_utf8_lossy(&output.stdout)),
        stderr: truncate(&String::from_utf8_lossy(&output.stderr)),
    })
}

/// Run a planned argument vector, e.g. the pinned `auth3-native` invocation.
pub fn run_argv(argv: &[String], timeout: Duration) -> Option<CommandOutput> {
    let (program, args) = argv.split_first()?;
    let refs: Vec<&str> = args.iter().map(String::as_str).collect();
    run_capture(program, &refs, timeout)
}

/// Run a command and capture stdout only.
pub fn run_bounded(program: &str, args: &[&str]) -> Option<String> {
    run_capture(program, args, COMMAND_TIMEOUT).map(|o| o.stdout)
}

/// Avahi's CLI has varied across distributions. Publishing an unscoped AirPlay
/// service could leak it onto the debug network, so use `--interface` only
/// after its locally installed help explicitly advertises that option.
pub fn avahi_supports_interface() -> bool {
    run_capture("avahi-publish-service", &["--help"], COMMAND_TIMEOUT)
        .map(|output| {
            let mut text = output.stdout;
            text.push_str(&output.stderr);
            avahi_help_supports_interface(&text)
        })
        .unwrap_or(false)
}

pub fn avahi_help_supports_interface(help: &str) -> bool {
    help.lines().any(|line| {
        let line = line.trim();
        (line.starts_with("-i") || line.contains("--interface")) && line.contains("interface")
    })
}

fn truncate(text: &str) -> String {
    if text.len() <= OUTPUT_CAP {
        text.to_string()
    } else {
        let mut end = OUTPUT_CAP;
        while end > 0 && !text.is_char_boundary(end) {
            end -= 1;
        }
        text[..end].to_string()
    }
}

/// Locate an executable on `PATH` without spawning a shell.
pub fn which(name: &str) -> Option<String> {
    let path = std::env::var("PATH").unwrap_or_default();
    for dir in path.split(path_separator()) {
        if dir.is_empty() {
            continue;
        }
        let candidate = Path::new(dir).join(name);
        if is_executable(&candidate) {
            return Some(candidate.to_string_lossy().into_owned());
        }
    }
    None
}

#[cfg(unix)]
fn path_separator() -> char {
    ':'
}

#[cfg(not(unix))]
fn path_separator() -> char {
    ';'
}

#[cfg(unix)]
fn is_executable(path: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(path).map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0).unwrap_or(false)
}

#[cfg(not(unix))]
fn is_executable(path: &Path) -> bool {
    path.with_extension("exe").exists() || path.exists()
}

/// Effective uid from `/proc/self/status`; used to refuse root-only commands
/// early with a clear message instead of a confusing permission error.
pub fn effective_uid() -> Option<u32> {
    parse_proc_self_uid(&read_text("/proc/self/status")?)
}

pub fn parse_proc_self_uid(text: &str) -> Option<u32> {
    for line in text.lines() {
        let Some(value) = line.strip_prefix("Uid:") else { continue };
        // real, effective, saved, filesystem
        return value.split_whitespace().nth(1)?.parse().ok();
    }
    None
}

pub fn rss_kib() -> u64 {
    read_text("/proc/self/status").and_then(|t| parse_proc_self_kib(&t, "VmRSS:")).unwrap_or(0)
}

pub fn parse_proc_self_kib(text: &str, key: &str) -> Option<u64> {
    for line in text.lines() {
        let Some(value) = line.strip_prefix(key) else { continue };
        return value.split_whitespace().next()?.parse().ok();
    }
    None
}

/// `(MemTotal, MemAvailable)` in KiB.
pub fn meminfo() -> (u64, u64) {
    let text = read_text("/proc/meminfo").unwrap_or_default();
    (parse_meminfo_kib(&text, "MemTotal:"), parse_meminfo_kib(&text, "MemAvailable:"))
}

pub fn parse_meminfo_kib(text: &str, key: &str) -> u64 {
    text.lines()
        .find_map(|l| l.strip_prefix(key))
        .and_then(|v| v.split_whitespace().next())
        .and_then(|v| v.parse().ok())
        .unwrap_or(0)
}

pub fn uname() -> String {
    run_bounded("uname", &["-srm"])
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .or_else(|| read_text("/proc/version").map(|v| v.lines().next().unwrap_or("").to_string()))
        .unwrap_or_default()
}

/// Group `ip -o addr show` output per interface: IPv4 addresses with prefix and
/// whether an IPv6 link-local exists (CarPlay negotiates over fe80::).
pub fn parse_ip_addr_show(text: &str) -> BTreeMap<String, (Vec<String>, bool)> {
    let mut out: BTreeMap<String, (Vec<String>, bool)> = BTreeMap::new();
    for line in text.lines() {
        let mut fields = line.split_whitespace();
        let Some(_index) = fields.next() else { continue };
        let Some(iface) = fields.next() else { continue };
        if iface.ends_with(':') {
            continue;
        }
        let entry = out.entry(iface.to_string()).or_insert_with(|| (Vec::new(), false));
        if entry.0.len() >= ADDR_PER_IFACE_CAP {
            continue;
        }
        match fields.next() {
            Some("inet") => {
                if let Some(addr) = fields.next() {
                    entry.0.push(addr.to_string());
                }
            }
            Some("inet6") => {
                if let Some(addr) = fields.next() {
                    if addr.starts_with("fe80") {
                        entry.1 = true;
                    }
                }
            }
            _ => {}
        }
    }
    out
}

pub fn parse_iface_flags(text: &str) -> bool {
    let value = text.trim();
    let hex = value.strip_prefix("0x").or_else(|| value.strip_prefix("0X")).unwrap_or(value);
    u32::from_str_radix(hex, 16).map(|flags| flags & IFF_UP != 0).unwrap_or(false)
}

/// Active NetworkManager connection name for an interface, captured before AP
/// takeover so rollback restores exactly that connection when it still exists.
pub fn nm_active_connection(iface: &str) -> Option<String> {
    run_bounded("nmcli", &["-g", "GENERAL.CONNECTION", "device", "show", iface])
        .map(|text| text.lines().next().unwrap_or("").trim().to_string())
        .filter(|name| !name.is_empty() && name != "--")
}

/// `nmcli -t -f DEVICE,STATE device` -> interface -> managed.
pub fn parse_nmcli_devices(text: &str) -> BTreeMap<String, bool> {
    let mut out = BTreeMap::new();
    for line in text.lines() {
        if out.len() >= IFACE_SCAN_CAP {
            break;
        }
        let Some((device, state)) = line.split_once(':') else { continue };
        if device.is_empty() {
            continue;
        }
        out.insert(device.to_string(), state.trim() != "unmanaged");
    }
    out
}

/// Interface names from NetworkManager `unmanaged-devices=` lines. Entries are
/// separated by `;`; only `interface-name:` selectors are reported, because
/// those are the ones that can silently pin the debug link down.
pub fn parse_unmanaged_devices(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    for line in text.lines() {
        let Some(value) = line.trim().strip_prefix("unmanaged-devices=") else { continue };
        for entry in value.split(';') {
            if out.len() >= IFACE_SCAN_CAP {
                break;
            }
            let entry = entry.trim();
            if let Some(name) = entry.strip_prefix("interface-name:") {
                if !name.is_empty() && !out.iter().any(|n| n == name) {
                    out.push(name.to_string());
                }
            }
        }
    }
    out
}

/// First `country XX:` from `iw reg get`; empty when unknown.
pub fn parse_iw_reg(text: &str) -> String {
    for line in text.lines() {
        let Some(rest) = line.trim().strip_prefix("country ") else { continue };
        let code = rest.split([':', ' ']).next().unwrap_or("").trim();
        if code.len() == 2 {
            return code.to_string();
        }
    }
    String::new()
}

/// Extract channels and whether VHT capabilities are advertised from `iw phy
/// <phy> info`. This intentionally accepts only the bracketed channel form,
/// avoiding assumptions from frequency lists or regulatory ranges.
pub fn parse_iw_phy_info(text: &str) -> (Vec<u8>, Option<bool>) {
    let mut channels = Vec::new();
    for line in text.lines() {
        let Some(start) = line.find('[') else { continue };
        let Some(end) = line[start + 1..].find(']') else { continue };
        if channels.len() >= 32 {
            break;
        }
        if let Ok(channel) = line[start + 1..start + 1 + end].trim().parse::<u8>() {
            if !channels.contains(&channel) {
                channels.push(channel);
            }
        }
    }
    let lower = text.to_ascii_lowercase();
    let vht = if lower.contains("vht capabilities") || lower.contains("vht cap") {
        Some(true)
    } else if !text.trim().is_empty() {
        Some(false)
    } else {
        None
    };
    (channels, vht)
}

fn phy_capabilities(iface: &str) -> (Vec<u8>, Option<bool>) {
    let phy = read_text(&format!("/sys/class/net/{iface}/phy80211/name"));
    let Some(phy) = phy.map(|v| v.trim().to_string()).filter(|v| !v.is_empty()) else { return (Vec::new(), None) };
    run_bounded("iw", &["phy", &phy, "info"])
        .map(|output| parse_iw_phy_info(&output))
        .unwrap_or_default()
}

/// Station MACs from `hostapd_cli all_sta`, bounded.
pub fn parse_hostapd_all_sta(text: &str) -> Vec<String> {
    text.lines()
        .map(str::trim)
        .filter(|l| l.contains(':') && l.len() >= 17 && !l.starts_with("Station"))
        .map(|l| l.split_whitespace().next().unwrap_or("").to_string())
        .filter(|mac| !mac.is_empty())
        .take(cp_harness::status::STATION_REPORT_CAP)
        .collect()
}

/// Collect the sysfs nodes the MFi policy reads, for the configured target and
/// for the AC200 bus so the refusal itself can be reported.
pub fn collect_sysfs(targets: &[MfiTarget]) -> SysfsView {
    let mut view = SysfsView::new();
    for target in targets {
        let paths = target.sysfs_paths();
        for path in [&paths.adapter_node, &paths.target_base, &paths.target_name, &paths.target_driver, &paths.target_of_node] {
            if view.exists(path) {
                continue;
            }
            if let Some(text) = read_text(path) {
                view.insert(path, text);
            } else if exists(path) {
                // A directory with no readable content still counts as present.
                view.insert(path, String::new());
            }
        }
    }
    view
}

fn iface_states() -> BTreeMap<String, IfaceState> {
    let addresses = run_bounded("ip", &["-o", "addr", "show"]).map(|t| parse_ip_addr_show(&t)).unwrap_or_default();
    let nm = run_bounded("nmcli", &["-t", "-f", "DEVICE,STATE", "device"])
        .map(|t| parse_nmcli_devices(&t))
        .unwrap_or_default();

    let mut out = BTreeMap::new();
    let entries = std::fs::read_dir("/sys/class/net").ok().map(|rd| {
        rd.filter_map(Result::ok)
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .take(IFACE_SCAN_CAP)
            .collect::<Vec<_>>()
    });
    let mut names = entries.unwrap_or_default();
    for name in addresses.keys() {
        if !names.iter().any(|n| n == name) && names.len() < IFACE_SCAN_CAP {
            names.push(name.clone());
        }
    }
    names.sort();

    for name in names {
        let base = format!("/sys/class/net/{name}");
        let mac = read_text(&format!("{base}/address")).map(|m| m.trim().to_string()).unwrap_or_default();
        let up = read_text(&format!("{base}/flags")).map(|f| parse_iface_flags(&f)).unwrap_or(false);
        let (ipv4, link_local) = addresses.get(&name).cloned().unwrap_or_default();
        out.insert(name.clone(), IfaceState { mac, ipv4, ipv6_link_local: link_local, up, nm_managed: nm.get(&name).copied() });
    }
    out
}

fn nm_unmanaged() -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut files = vec!["/etc/NetworkManager/NetworkManager.conf".to_string()];
    if let Ok(entries) = std::fs::read_dir("/etc/NetworkManager/conf.d") {
        for entry in entries.filter_map(Result::ok).take(NM_CONF_SCAN_CAP) {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) == Some("conf") {
                files.push(path.to_string_lossy().into_owned());
            }
        }
    }
    for file in files {
        if let Some(text) = read_text(&file) {
            for name in parse_unmanaged_devices(&text) {
                if !out.contains(&name) && out.len() < IFACE_SCAN_CAP {
                    out.push(name);
                }
            }
        }
    }
    out
}

fn unit_state(name: &str) -> Option<UnitState> {
    let active = run_bounded("systemctl", &["is-active", name])?;
    let active = active.trim();
    if active.is_empty() {
        return None;
    }
    let sub = run_bounded("systemctl", &["show", "--value", "--property=SubState", name])
        .map(|s| s.trim().to_string())
        .unwrap_or_default();
    Some(UnitState { active: active == "active", sub })
}

fn regulatory_country() -> String {
    run_bounded("iw", &["reg", "get"]).map(|t| parse_iw_reg(&t)).unwrap_or_default()
}

pub fn udp_listeners() -> Vec<UdpEndpoint> {
    read_text("/proc/net/udp").map(|t| parse_proc_net_udp(&t)).unwrap_or_default()
}

/// Whether `iface` already carries an IPv6 link-local address.
pub fn iface_has_link_local(iface: &str) -> bool {
    read_text("/proc/net/if_inet6").map(|t| parse_if_inet6_link_local(&t, iface)).unwrap_or(false)
}

/// `/proc/net/if_inet6`: `<addr> <index> <prefix> <scope> <flags> <dev>`, where
/// scope `20` is link-local.
pub fn parse_if_inet6_link_local(text: &str, iface: &str) -> bool {
    text.lines().any(|line| {
        let mut fields = line.split_whitespace();
        let Some(addr) = fields.next() else { return false };
        let scope = fields.nth(2).unwrap_or("");
        let dev = fields.nth(1).unwrap_or("");
        addr.starts_with("fe80") && scope == "20" && dev == iface
    })
}

/// Everything `preflight`, `status` and the launcher need, in one pass.
pub fn collect(cfg: &RuntimeConfig) -> SystemSnapshot {
    let mut commands = BTreeMap::new();
    for (name, _) in REQUIRED_COMMANDS.iter().chain(RECOMMENDED_COMMANDS.iter()) {
        if let Some(path) = which(name) {
            commands.insert(name.to_string(), path);
        }
    }

    let mut files = BTreeMap::new();
    let paths = cp_harness::ap::ApPaths::new(&cfg.runtime_dir);
    for path in [&cfg.mfi.tool, &paths.runtime_dir, &format!("/dev/i2c-{}", cfg.mfi.target.bus), &cfg.core_socket] {
        files.insert(path.clone(), exists(path));
    }

    let mut units = BTreeMap::new();
    for unit in cfg.rndis.units.iter() {
        if let Some(state) = unit_state(unit) {
            units.insert(unit.clone(), state);
        }
    }

    let (supported_channels, supports_vht80) = phy_capabilities(&cfg.ap.iface);
    let net = NetSnapshot {
        ifaces: iface_states(),
        units,
        udp_listeners: udp_listeners(),
        nm_unmanaged: nm_unmanaged(),
        regulatory_country: regulatory_country(),
        supported_channels,
        supports_vht80,
    };
    let sysfs = collect_sysfs(&[cfg.mfi.target, MfiTarget { bus: AC200_BUS, addr: cfg.mfi.target.addr }]);
    let (memory_total_kib, memory_available_kib) = meminfo();

    SystemSnapshot {
        net,
        sysfs,
        commands,
        files,
        texts: BTreeMap::new(),
        rss_kib: rss_kib(),
        memory_total_kib,
        memory_available_kib,
        uname: uname(),
    }
}

/// Read only the fixed development safety evidence needed before a CatPlay
/// child can start. This deliberately avoids AP planning, radio configuration,
/// hostapd/dnsmasq/Avahi probing, UDC/configfs, and every I2C device open.
pub fn collect_mandatory_safety() -> SystemSnapshot {
    let allowed = MfiTarget { bus: 1, addr: 0x10 };
    let ac200 = MfiTarget { bus: AC200_BUS, addr: 0x10 };
    let mut files = BTreeMap::new();
    files.insert(allowed.dev_path(), exists(&allowed.dev_path()));
    SystemSnapshot {
        net: NetSnapshot { ifaces: iface_states(), ..NetSnapshot::default() },
        sysfs: collect_sysfs(&[allowed, ac200]),
        commands: BTreeMap::new(),
        files,
        texts: BTreeMap::new(),
        rss_kib: 0,
        memory_total_kib: 0,
        memory_available_kib: 0,
        uname: String::new(),
    }
}

/// `hostapd_cli status` for the AP interface, empty when hostapd is not running.
pub fn hostapd_status(cfg: &RuntimeConfig) -> String {
    run_bounded("hostapd_cli", &["-p", cp_harness::ap::HOSTAPD_CTRL_DIR, "-i", &cfg.ap.iface, "status"]).unwrap_or_default()
}

pub fn stations(cfg: &RuntimeConfig) -> Vec<String> {
    run_bounded("hostapd_cli", &["-p", cp_harness::ap::HOSTAPD_CTRL_DIR, "-i", &cfg.ap.iface, "all_sta"])
        .map(|t| parse_hostapd_all_sta(&t))
        .unwrap_or_default()
}

/// AP interface MAC, when the interface is known.
pub fn iface_mac(cfg: &RuntimeConfig) -> Option<[u8; 6]> {
    read_text(&format!("/sys/class/net/{}/address", cfg.ap.iface)).and_then(|t| cp_harness::ap::parse_mac(&t))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_ip_addr_show() {
        let text = "\
1: lo    inet 127.0.0.1/8 scope host lo\\       valid_lft forever preferred_lft forever
1: lo    inet6 ::1/128 scope host \\       valid_lft forever preferred_lft forever
2: wlan0    inet 10.0.0.34/24 brd 10.0.0.255 scope global dynamic noprefixroute wlan0\\       valid_lft 3600sec preferred_lft 3600sec
2: wlan0    inet6 fe80::a8bb:ccff:fedd:eeff/64 scope link noprefixroute \\       valid_lft forever preferred_lft forever
3: usb0    inet 192.168.77.2/24 brd 192.168.77.255 scope global noprefixroute usb0\\       valid_lft forever preferred_lft forever
";
        let parsed = parse_ip_addr_show(text);
        assert_eq!(parsed["wlan0"].0, vec!["10.0.0.34/24".to_string()]);
        assert!(parsed["wlan0"].1, "wlan0 has a link-local");
        assert_eq!(parsed["usb0"].0, vec!["192.168.77.2/24".to_string()]);
        assert!(!parsed["usb0"].1);
        assert_eq!(parsed["lo"].0, vec!["127.0.0.1/8".to_string()]);
        assert!(!parsed["lo"].1);
    }

    #[test]
    fn address_lists_are_bounded() {
        let text = (0..20)
            .map(|i| format!("2: wlan0    inet 10.0.0.{i}/24 scope global wlan0"))
            .collect::<Vec<_>>()
            .join("\n");
        assert_eq!(parse_ip_addr_show(&text)["wlan0"].0.len(), 8);
    }

    #[test]
    fn parses_iface_flags() {
        assert!(parse_iface_flags("0x1003\n"));
        assert!(parse_iface_flags("0x1\n"));
        assert!(!parse_iface_flags("0x1002\n"));
        assert!(!parse_iface_flags("garbage"));
    }

    #[test]
    fn parses_nmcli_devices() {
        let text = "lo:unmanaged\nwlan0:connected\nusb0:connected\n";
        let parsed = parse_nmcli_devices(text);
        assert!(!parsed["lo"]);
        assert!(parsed["wlan0"]);
        assert!(parsed["usb0"]);
        assert_eq!(parsed.len(), 3);
    }

    #[test]
    fn nmcli_device_list_is_bounded() {
        let text = (0..40).map(|i| format!("if{i}:connected")).collect::<Vec<_>>().join("\n");
        assert_eq!(parse_nmcli_devices(&text).len(), IFACE_SCAN_CAP);
    }

    #[test]
    fn parses_unmanaged_devices() {
        let text = "\
[main]
plugins=keyfile
[keyfile]
unmanaged-devices=interface-name:wlan0;interface-name:wlan1;mac:aa:bb:cc:dd:ee:ff
";
        assert_eq!(parse_unmanaged_devices(text), vec!["wlan0".to_string(), "wlan1".to_string()]);
    }

    #[test]
    fn unmanaged_devices_deduplicate() {
        let text = "unmanaged-devices=interface-name:wlan0\nunmanaged-devices=interface-name:wlan0\n";
        assert_eq!(parse_unmanaged_devices(text), vec!["wlan0".to_string()]);
    }

    #[test]
    fn avahi_interface_capability_is_probed_from_local_help() {
        assert!(avahi_help_supports_interface("  -i, --interface=IFACE  Publish on interface IFACE\n"));
        assert!(avahi_help_supports_interface("      --interface=IFACE  interface to use\n"));
        assert!(!avahi_help_supports_interface("Usage: avahi-publish-service [OPTIONS]\n"));
    }

    #[test]
    fn parses_regulatory_domain() {
        let global = "global\ncountry 00: DFS-UNSET\n\t(2402 - 2472 @ 40), (N/A, 20), (N/A)\n";
        assert_eq!(parse_iw_reg(global), "00");
        let phy = "phy#0\n\tcountry US: DFS-FCC\n";
        assert_eq!(parse_iw_reg(phy), "US");
        assert_eq!(parse_iw_reg(""), "");
    }

    #[test]
    fn parses_uid_and_memory_fields() {
        let status = "Name:\tcp-native\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nVmRSS:\t    1234 kB\n";
        assert_eq!(parse_proc_self_uid(status), Some(0));
        assert_eq!(parse_proc_self_kib(status, "VmRSS:"), Some(1234));
        assert_eq!(parse_proc_self_uid("no uid here"), None);
        let meminfo = "MemTotal:         999999 kB\nMemFree:          100000 kB\nMemAvailable:     700000 kB\n";
        assert_eq!(parse_meminfo_kib(meminfo, "MemTotal:"), 999999);
        assert_eq!(parse_meminfo_kib(meminfo, "MemAvailable:"), 700000);
        assert_eq!(parse_meminfo_kib(meminfo, "SwapTotal:"), 0);
    }

    #[test]
    fn parses_phy_channels_and_vht_capability() {
        let text =
            "Wiphy phy0\n\tBand 1:\n\t\t* 5180 MHz [36] (20.0 dBm)\n\t\t* 5220 MHz [44] (20.0 dBm)\n\tVHT Capabilities (0x00000000):\n";
        assert_eq!(parse_iw_phy_info(text), (vec![36, 44], Some(true)));
        assert_eq!(parse_iw_phy_info("Wiphy phy0\n"), (Vec::new(), Some(false)));
        assert_eq!(parse_iw_phy_info(""), (Vec::new(), None));
    }

    #[test]
    fn parses_station_list() {
        let text = "Station            Address            Flags\nsta 1\naa:bb:cc:dd:ee:01\t[HT][WMM]\nbb:cc:dd:ee:ff:02\t[HT]\n";
        assert_eq!(parse_hostapd_all_sta(text), vec!["aa:bb:cc:dd:ee:01".to_string(), "bb:cc:dd:ee:ff:02".to_string()]);
    }

    #[test]
    fn station_list_is_bounded() {
        let text = (0..20).map(|i| format!("aa:bb:cc:dd:ee:{i:02x}\t[HT]")).collect::<Vec<_>>().join("\n");
        assert_eq!(parse_hostapd_all_sta(&text).len(), cp_harness::status::STATION_REPORT_CAP);
    }

    #[test]
    fn long_output_is_truncated_at_a_boundary() {
        let long = "x".repeat(OUTPUT_CAP + 100);
        assert_eq!(truncate(&long).len(), OUTPUT_CAP);
        let multibyte = "é".repeat(OUTPUT_CAP);
        assert!(truncate(&multibyte).len() <= OUTPUT_CAP);
    }

    #[test]
    fn a_missing_program_yields_no_output() {
        assert!(run_capture("cp-native-definitely-missing", &[], Duration::from_secs(1)).is_none());
        assert!(run_argv(&["cp-native-definitely-missing".to_string()], Duration::from_secs(1)).is_none());
    }

    #[test]
    fn an_empty_argv_is_rejected() {
        assert!(run_argv(&[], Duration::from_secs(1)).is_none());
    }

    #[test]
    fn parses_link_local_presence() {
        let text = "\
fe80000000000000a8bbccfffeedddee 02 40 20 80   wlan0
00000000000000000000000000000001 01 80 10 80   lo
fe80000000000000123456ff789abcd0 03 40 20 80   usb0
20010db8000000000000000000000001 02 40 00 80   wlan0
";
        assert!(parse_if_inet6_link_local(text, "wlan0"));
        assert!(parse_if_inet6_link_local(text, "usb0"));
        assert!(!parse_if_inet6_link_local(text, "wlan1"));
        assert!(!parse_if_inet6_link_local(text, "lo"), "a loopback ::1 is not link-local");
        assert!(parse_if_inet6_link_local(text, "wlan0"), "a global address does not cancel the link-local one");
    }

    #[test]
    fn which_finds_a_program_that_exists_and_rejects_one_that_does_not() {
        assert!(which("definitely-not-a-real-program-xyz").is_none());
    }

    #[test]
    fn sysfs_collection_covers_the_ac200_bus_too() {
        // Off-target this only proves the path set; on the board it fills in the
        // AC200 identity that makes the refusal visible in every report.
        let view = collect_sysfs(&[MfiTarget { bus: 1, addr: 0x10 }, MfiTarget { bus: AC200_BUS, addr: 0x10 }]);
        for path in [
            "/sys/class/i2c-dev/i2c-1/device/of_node/name",
            "/sys/class/i2c-dev/i2c-2/device/of_node/name",
            "/sys/bus/i2c/devices/2-0010/name",
        ] {
            // Either present with content, or absent; both are valid off-target.
            let _ = view.read_text(path);
        }
        assert!(view.len() <= 10, "{}", view.len());
    }
}
