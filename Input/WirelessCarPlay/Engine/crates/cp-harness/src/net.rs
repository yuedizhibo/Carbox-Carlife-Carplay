//! Network policy: confine the CarPlay AP to the Wi-Fi radio and prove the USB
//! RNDIS debug link survives every step.
//!
//! The debug link is the only way to reach the board once `wlan0` becomes an AP,
//! so the checks here are deliberately strict: an operation that could drop
//! `usb0`, its 192.168.77.2 address, or its DHCP service is a blocking finding
//! and the launcher refuses to run.

use std::collections::BTreeMap;
use std::net::Ipv4Addr;

use serde::Serialize;

use crate::bounds::REPORT_ITEM_CAP;
use crate::config::RuntimeConfig;

/// UDP port a DHCP server binds.
pub const DHCP_PORT: u16 = 67;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum FindingSeverity {
    /// The launcher must not proceed.
    Blocking,
    /// The operator should know, but the launcher may proceed.
    Warning,
    /// Context for a report.
    Info,
}

impl FindingSeverity {
    pub fn as_str(self) -> &'static str {
        match self {
            FindingSeverity::Blocking => "blocking",
            FindingSeverity::Warning => "warning",
            FindingSeverity::Info => "info",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Finding {
    pub id: &'static str,
    pub severity: FindingSeverity,
    pub message: String,
}

impl Finding {
    pub fn blocking(id: &'static str, message: impl Into<String>) -> Self {
        Self { id, severity: FindingSeverity::Blocking, message: message.into() }
    }

    pub fn warning(id: &'static str, message: impl Into<String>) -> Self {
        Self { id, severity: FindingSeverity::Warning, message: message.into() }
    }

    pub fn info(id: &'static str, message: impl Into<String>) -> Self {
        Self { id, severity: FindingSeverity::Info, message: message.into() }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize)]
pub struct IfaceState {
    pub mac: String,
    pub ipv4: Vec<String>,
    pub ipv6_link_local: bool,
    pub up: bool,
    /// `None` when NetworkManager does not know the device.
    pub nm_managed: Option<bool>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct UnitState {
    pub active: bool,
    pub sub: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct UdpEndpoint {
    pub local_ip: Ipv4Addr,
    pub port: u16,
}

impl UdpEndpoint {
    pub fn wildcard(&self) -> bool {
        self.local_ip.is_unspecified()
    }
}

/// Everything the network policy needs, collected once by `cp-native`.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
pub struct NetSnapshot {
    pub ifaces: BTreeMap<String, IfaceState>,
    pub units: BTreeMap<String, UnitState>,
    pub udp_listeners: Vec<UdpEndpoint>,
    /// Interfaces listed in NetworkManager's `unmanaged-devices`.
    pub nm_unmanaged: Vec<String>,
    /// Country reported by `iw reg get`, empty when unknown.
    pub regulatory_country: String,
    /// Channels parsed from this interface's `iw phy ... info` output. Empty
    /// means the platform could not be queried, never that every channel works.
    pub supported_channels: Vec<u8>,
    /// `Some(false)` means the phy explicitly lacks VHT/80MHz support.
    pub supports_vht80: Option<bool>,
}

impl NetSnapshot {
    pub fn iface(&self, name: &str) -> Option<&IfaceState> {
        self.ifaces.get(name)
    }

    pub fn unit(&self, name: &str) -> Option<&UnitState> {
        self.units.get(name)
    }
}

/// Who currently owns UDP/67, from the AP's point of view.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum DhcpOwner {
    /// Nothing is bound to 67 on the AP address or on every address.
    Free,
    /// A server is bound to the address the AP will use, or to all addresses.
    Conflict,
    /// A server is bound to some other specific address, e.g. the RNDIS one.
    Elsewhere,
}

/// Classify the DHCP listeners in `snapshot` for the address the AP will own.
///
/// A wildcard or AP-address binding means the port is taken; a binding on
/// another address (the RNDIS dnsmasq on 192.168.77.2) coexists safely because
/// the AP config uses `bind-interfaces`.
pub fn dhcp_owner(snapshot: &NetSnapshot, ap_ip: Ipv4Addr) -> DhcpOwner {
    let mut elsewhere = false;
    for ep in snapshot.udp_listeners.iter().filter(|e| e.port == DHCP_PORT) {
        if ep.wildcard() || ep.local_ip == ap_ip {
            return DhcpOwner::Conflict;
        }
        elsewhere = true;
    }
    if elsewhere {
        DhcpOwner::Elsewhere
    } else {
        DhcpOwner::Free
    }
}

/// True once a DHCP server answers on the AP address itself. Used as the AP
/// readiness signal instead of LIVI's "anything listens on 67", which the RNDIS
/// dnsmasq would satisfy from the moment the board boots.
pub fn dhcp_ready_for_ap(snapshot: &NetSnapshot, ap_ip: Ipv4Addr) -> bool {
    snapshot.udp_listeners.iter().any(|e| e.port == DHCP_PORT && (e.wildcard() || e.local_ip == ap_ip))
}

/// Parse `/proc/net/udp`. Addresses are little-endian hex (`024DA8C0:0043` is
/// 192.168.77.2:67). The result is bounded by [`REPORT_ITEM_CAP`].
pub fn parse_proc_net_udp(text: &str) -> Vec<UdpEndpoint> {
    let mut out = Vec::new();
    for line in text.lines().skip(1) {
        if out.len() >= REPORT_ITEM_CAP {
            break;
        }
        let mut fields = line.split_whitespace();
        let Some(_sl) = fields.next() else { continue };
        let Some(local) = fields.next() else { continue };
        let state = fields.nth(1).unwrap_or("");
        // 07 = UDP "closed" (a bound server socket), 0A = listening.
        if state != "07" && state != "0A" {
            continue;
        }
        let Some((ip_hex, port_hex)) = local.split_once(':') else { continue };
        let Ok(raw) = u32::from_str_radix(ip_hex, 16) else { continue };
        let Ok(port) = u16::from_str_radix(port_hex, 16) else { continue };
        out.push(UdpEndpoint { local_ip: Ipv4Addr::from(raw.to_le_bytes()), port });
    }
    out
}

/// Evaluate the AP plan against the live network state. Called by `preflight`,
/// `ap-plan` and `ap-up`; the launcher refuses on any blocking finding.
pub fn check_ap_plan(cfg: &RuntimeConfig, snap: &NetSnapshot) -> Vec<Finding> {
    let mut findings: Vec<Finding> = Vec::new();
    let mut add = |f: Finding| {
        if findings.len() < REPORT_ITEM_CAP {
            findings.push(f);
        }
    };

    if cfg.ap.iface == cfg.rndis.iface {
        add(Finding::blocking("ap-iface-is-debug-link", format!("AP interface '{}' is also the USB RNDIS debug interface", cfg.ap.iface)));
    }
    if cfg.ap.iface == "lo" {
        add(Finding::blocking("ap-iface-is-loopback", "the AP cannot be hosted on 'lo'"));
    }
    if snap.supported_channels.is_empty() {
        // Selecting a legal regulatory channel is not enough: the actual PHY
        // may expose a narrower channel set. Refuse rather than guessing; the
        // operator can only continue after `iw phy` supplies board evidence.
        add(Finding::blocking(
            "phy-capability-unavailable",
            "could not read iw phy channel capabilities; refusing to guess a channel or VHT plan",
        ));
    } else if !snap.supported_channels.contains(&cfg.ap.channel) {
        add(Finding::blocking(
            "phy-channel-unsupported",
            format!("{} does not report AP channel {} as supported", cfg.ap.iface, cfg.ap.channel),
        ));
    }
    if cfg.ap.width_mhz >= 80 && snap.supports_vht80 == Some(false) {
        add(Finding::blocking("phy-vht80-unsupported", format!("{} explicitly lacks VHT/80MHz support", cfg.ap.iface)));
    }

    match snap.iface(&cfg.ap.iface) {
        None => add(Finding::blocking("ap-iface-missing", format!("interface '{}' does not exist", cfg.ap.iface))),
        Some(ap) => {
            if !ap.up {
                add(Finding::info("ap-iface-down", format!("'{}' is down; the launcher brings it up", cfg.ap.iface)));
            }
            if ap.mac.is_empty() {
                add(Finding::warning(
                    "ap-mac-unknown",
                    format!("'{}' reports no MAC address; mDNS deviceid cannot be derived", cfg.ap.iface),
                ));
            }
            for addr in ap.ipv4.iter() {
                if let Ok(ip) = addr.split('/').next().unwrap_or(addr).parse::<Ipv4Addr>() {
                    if same_subnet(ip, cfg.rndis.ip, 24) {
                        add(Finding::blocking(
                            "ap-iface-holds-debug-subnet",
                            format!(
                                "'{}' currently holds {} inside the RNDIS subnet {}/24; flushing it would break the debug link",
                                cfg.ap.iface, addr, cfg.rndis.ip
                            ),
                        ));
                    }
                }
            }
            if !ap.ipv4.is_empty() || ap.nm_managed == Some(true) {
                add(Finding::warning(
                    "ap-iface-in-use-as-client",
                    format!(
                        "'{}' is currently a NetworkManager client with addresses [{}]; ap-up releases it from NetworkManager and flushes global addresses, so any SSH session over those addresses is lost",
                        cfg.ap.iface,
                        ap.ipv4.join(", ")
                    ),
                ));
            }
            if ap.nm_managed == Some(true) && !snap.nm_unmanaged.iter().any(|i| i == &cfg.ap.iface) {
                add(Finding::info(
                    "ap-iface-nm-takeover",
                    format!("'{}' is NetworkManager-managed; the launcher writes an unmanaged-devices entry for it only", cfg.ap.iface),
                ));
            }
        }
    }

    // Debug link preservation.
    match snap.iface(&cfg.rndis.iface) {
        None if cfg.rndis.required => add(Finding::blocking(
            "rndis-iface-missing",
            format!("required USB RNDIS interface '{}' does not exist; refusing to touch the Wi-Fi radio", cfg.rndis.iface),
        )),
        None => add(Finding::warning(
            "rndis-iface-missing",
            format!("USB RNDIS interface '{}' does not exist and CP_REQUIRE_RNDIS is off", cfg.rndis.iface),
        )),
        Some(rndis) => {
            let holds_ip = rndis.ipv4.iter().any(|a| a.split('/').next().unwrap_or(a) == cfg.rndis.ip.to_string());
            if !holds_ip && cfg.rndis.required {
                add(Finding::blocking(
                    "rndis-address-missing",
                    format!("'{}' does not hold {}; the debug SSH path is not usable", cfg.rndis.iface, cfg.rndis.ip),
                ));
            }
        }
    }

    for unit in cfg.rndis.units.iter() {
        match snap.unit(unit) {
            Some(u) if u.active => add(Finding::info("rndis-unit-active", format!("{unit} is active ({})", u.sub))),
            Some(u) if cfg.rndis.required => add(Finding::blocking(
                "rndis-unit-inactive",
                format!("{unit} is not active ({}); refusing to start the AP without the debug link", u.sub),
            )),
            Some(u) => add(Finding::warning("rndis-unit-inactive", format!("{unit} is not active ({})", u.sub))),
            None if cfg.rndis.required => add(Finding::blocking(
                "rndis-unit-unknown",
                format!("{unit} is not known to systemd; refusing to start the AP without the debug link"),
            )),
            None => add(Finding::warning("rndis-unit-unknown", format!("{unit} is not known to systemd"))),
        }
    }

    if snap.nm_unmanaged.iter().any(|i| i == &cfg.rndis.iface) {
        // Expected on this board: the RNDIS gadget is configured by
        // zero2w-usb-rndis.service, not by NetworkManager. What must never happen
        // is our own drop-in naming it, and nm_unmanaged_conf() only ever names
        // the AP interface.
        add(Finding::info(
            "rndis-unmanaged-by-nm",
            format!(
                "'{}' is unmanaged by NetworkManager; the debug link is configured by its own service, and the generated drop-in names only '{}'",
                cfg.rndis.iface, cfg.ap.iface
            ),
        ));
    }
    for iface in snap.nm_unmanaged.iter() {
        if iface != &cfg.ap.iface && iface != &cfg.rndis.iface {
            add(Finding::info("nm-unmanaged-other", format!("'{iface}' is already unmanaged by NetworkManager")));
        }
    }

    match dhcp_owner(snap, cfg.ap.ap_ip) {
        DhcpOwner::Conflict => add(Finding::blocking(
            "dhcp-port-conflict",
            format!("UDP/{DHCP_PORT} is already bound to {} or to every address; only the AP dnsmasq may own it", cfg.ap.ap_ip),
        )),
        DhcpOwner::Elsewhere => add(Finding::info(
            "dhcp-coexists",
            format!(
                "another DHCP server is bound elsewhere (expected: the RNDIS one on {}); the AP config uses bind-interfaces",
                cfg.rndis.ip
            ),
        )),
        DhcpOwner::Free => add(Finding::info("dhcp-port-free", format!("UDP/{DHCP_PORT} is free for the AP address {}", cfg.ap.ap_ip))),
    }

    if snap.regulatory_country.is_empty() {
        add(Finding::warning("reg-domain-unknown", "the current regulatory domain could not be read; `iw reg set` will still run"));
    } else if snap.regulatory_country == "00" {
        add(Finding::warning(
            "reg-domain-world",
            format!("the regulatory domain is 00 (world); ap-up sets it to {} before starting hostapd", cfg.ap.country),
        ));
    } else if snap.regulatory_country != cfg.ap.country {
        add(Finding::info(
            "reg-domain-change",
            format!("the regulatory domain is {}; ap-up sets it to {}", snap.regulatory_country, cfg.ap.country),
        ));
    }

    findings
}

fn same_subnet(a: Ipv4Addr, b: Ipv4Addr, prefix: u8) -> bool {
    let shift = 32 - u32::from(prefix);
    (u32::from(a) >> shift) == (u32::from(b) >> shift)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::RuntimeConfig;
    use std::collections::BTreeMap;

    fn cfg() -> RuntimeConfig {
        RuntimeConfig::from_map(&[("CP_COUNTRY".to_string(), "US".to_string())].into_iter().collect::<BTreeMap<_, _>>())
            .expect("valid defaults")
    }

    /// The board as it is right now: wlan0 on the hotel Wi-Fi, usb0 up with the
    /// debug address, both RNDIS units active, RNDIS dnsmasq bound to 67.
    fn healthy_board() -> NetSnapshot {
        let mut ifaces = BTreeMap::new();
        ifaces.insert(
            "wlan0".to_string(),
            IfaceState {
                mac: "aa:bb:cc:dd:ee:ff".to_string(),
                ipv4: vec!["10.0.0.34/24".to_string()],
                ipv6_link_local: true,
                up: true,
                nm_managed: Some(true),
            },
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
        NetSnapshot {
            ifaces,
            units,
            udp_listeners: vec![UdpEndpoint { local_ip: Ipv4Addr::new(192, 168, 77, 2), port: 67 }],
            nm_unmanaged: vec![],
            regulatory_country: "00".to_string(),
            supported_channels: vec![36, 40, 44, 48],
            supports_vht80: Some(true),
        }
    }

    fn ids(findings: &[Finding]) -> Vec<String> {
        findings.iter().map(|f| format!("{}:{}", f.severity.as_str(), f.id)).collect()
    }

    fn blocking(findings: &[Finding]) -> Vec<String> {
        findings
            .iter()
            .filter(|f| f.severity == FindingSeverity::Blocking)
            .map(|f| f.id.to_string())
            .collect()
    }

    #[test]
    fn unavailable_phy_capabilities_block_the_ap() {
        let mut snap = healthy_board();
        snap.supported_channels.clear();
        assert!(blocking(&check_ap_plan(&cfg(), &snap)).contains(&"phy-capability-unavailable".to_string()));
    }

    #[test]
    fn rejects_a_channel_the_phy_does_not_report() {
        let mut snap = healthy_board();
        snap.supported_channels = vec![36];
        let mut c = cfg();
        c.ap.channel = 44;
        assert!(blocking(&check_ap_plan(&c, &snap)).contains(&"phy-channel-unsupported".to_string()));
    }

    #[test]
    fn rejects_explicitly_unsupported_vht80() {
        let mut snap = healthy_board();
        snap.supports_vht80 = Some(false);
        let mut c = cfg();
        c.ap.width_mhz = 80;
        assert!(blocking(&check_ap_plan(&c, &snap)).contains(&"phy-vht80-unsupported".to_string()));
    }

    #[test]
    fn healthy_board_has_no_blocking_findings() {
        let findings = check_ap_plan(&cfg(), &healthy_board());
        assert!(blocking(&findings).is_empty(), "{}", ids(&findings).join("\n"));
        // The operator must still be told that wlan0's client address goes away.
        assert!(ids(&findings).contains(&"warning:ap-iface-in-use-as-client".to_string()), "{}", ids(&findings).join("\n"));
        assert!(ids(&findings).contains(&"warning:reg-domain-world".to_string()), "{}", ids(&findings).join("\n"));
        assert!(ids(&findings).contains(&"info:dhcp-coexists".to_string()), "{}", ids(&findings).join("\n"));
    }

    #[test]
    fn missing_debug_link_blocks_the_ap() {
        let mut snap = healthy_board();
        snap.ifaces.remove("usb0");
        assert_eq!(blocking(&check_ap_plan(&cfg(), &snap)), vec!["rndis-iface-missing"]);
    }

    #[test]
    fn debug_link_without_its_address_blocks_the_ap() {
        let mut snap = healthy_board();
        snap.ifaces.get_mut("usb0").expect("usb0").ipv4.clear();
        assert_eq!(blocking(&check_ap_plan(&cfg(), &snap)), vec!["rndis-address-missing"]);
    }

    #[test]
    fn inactive_debug_unit_blocks_the_ap() {
        let mut snap = healthy_board();
        snap.units.get_mut("zero2w-usb-dhcp.service").expect("unit").active = false;
        assert_eq!(blocking(&check_ap_plan(&cfg(), &snap)), vec!["rndis-unit-inactive"]);
    }

    #[test]
    fn debug_link_can_be_optional() {
        let mut c = cfg();
        c.rndis.required = false;
        let mut snap = healthy_board();
        snap.ifaces.remove("usb0");
        snap.units.clear();
        let findings = check_ap_plan(&c, &snap);
        assert!(blocking(&findings).is_empty(), "{}", ids(&findings).join("\n"));
        assert!(ids(&findings).contains(&"warning:rndis-iface-missing".to_string()));
    }

    #[test]
    fn ap_on_the_debug_interface_blocks() {
        let mut c = cfg();
        c.ap.iface = "usb0".to_string();
        let findings = check_ap_plan(&c, &healthy_board());
        assert!(blocking(&findings).contains(&"ap-iface-is-debug-link".to_string()), "{}", ids(&findings).join("\n"));
    }

    #[test]
    fn missing_wifi_interface_blocks() {
        let mut snap = healthy_board();
        snap.ifaces.remove("wlan0");
        assert_eq!(blocking(&check_ap_plan(&cfg(), &snap)), vec!["ap-iface-missing"]);
    }

    #[test]
    fn wifi_interface_holding_the_debug_subnet_blocks() {
        let mut snap = healthy_board();
        snap.ifaces.get_mut("wlan0").expect("wlan0").ipv4 = vec!["192.168.77.9/24".to_string()];
        assert!(
            blocking(&check_ap_plan(&cfg(), &snap)).iter().any(|id| id == "ap-iface-holds-debug-subnet"),
            "{}",
            ids(&check_ap_plan(&cfg(), &snap)).join("\n")
        );
    }

    #[test]
    fn an_nm_unmanaged_debug_link_is_expected_and_does_not_block() {
        // The RNDIS gadget is configured by its own oneshot service, so the board
        // deliberately keeps NetworkManager off usb0.
        let mut snap = healthy_board();
        snap.nm_unmanaged = vec!["usb0".to_string()];
        let findings = check_ap_plan(&cfg(), &snap);
        assert!(blocking(&findings).is_empty(), "{}", ids(&findings).join("\n"));
        assert!(ids(&findings).contains(&"info:rndis-unmanaged-by-nm".to_string()), "{}", ids(&findings).join("\n"));
    }

    #[test]
    fn the_generated_dropin_never_names_the_debug_link() {
        let conf = crate::ap::nm_unmanaged_conf("wlan0");
        assert!(conf.contains("interface-name:wlan0"), "{conf}");
        assert!(!conf.contains("usb0"), "{conf}");
    }

    #[test]
    fn wildcard_dhcp_listener_blocks() {
        let mut snap = healthy_board();
        snap.udp_listeners.push(UdpEndpoint { local_ip: Ipv4Addr::UNSPECIFIED, port: 67 });
        assert_eq!(blocking(&check_ap_plan(&cfg(), &snap)), vec!["dhcp-port-conflict"]);
    }

    #[test]
    fn dhcp_owner_classification() {
        let snap = healthy_board();
        assert_eq!(dhcp_owner(&snap, Ipv4Addr::new(10, 10, 0, 1)), DhcpOwner::Elsewhere);
        let empty = NetSnapshot::default();
        assert_eq!(dhcp_owner(&empty, Ipv4Addr::new(10, 10, 0, 1)), DhcpOwner::Free);
        let bound =
            NetSnapshot { udp_listeners: vec![UdpEndpoint { local_ip: Ipv4Addr::new(10, 10, 0, 1), port: 67 }], ..NetSnapshot::default() };
        assert_eq!(dhcp_owner(&bound, Ipv4Addr::new(10, 10, 0, 1)), DhcpOwner::Conflict);
    }

    #[test]
    fn readiness_requires_the_ap_address_not_just_any_listener() {
        // The RNDIS dnsmasq listens on 67 from boot; that must not read as "AP ready".
        assert!(!dhcp_ready_for_ap(&healthy_board(), Ipv4Addr::new(10, 10, 0, 1)));
        let mut ready = healthy_board();
        ready.udp_listeners.push(UdpEndpoint { local_ip: Ipv4Addr::new(10, 10, 0, 1), port: 67 });
        assert!(dhcp_ready_for_ap(&ready, Ipv4Addr::new(10, 10, 0, 1)));
    }

    #[test]
    fn parses_proc_net_udp() {
        let text = "\
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops
 1234: 024DA8C0:0043 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 42424 2 ffffff80 0
 1235: 01000A0A:0043 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 42425 2 ffffff81 0
 1236: 00000000:0035 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 42426 2 ffffff82 0
 1237: 0100007F:0BB9 00000000:0000 01 00000000:00000000 00:00000000 00000000     0        0 42427 2 ffffff83 0
";
        let eps = parse_proc_net_udp(text);
        assert_eq!(eps.len(), 3, "{eps:?}");
        assert_eq!(eps[0].local_ip, Ipv4Addr::new(192, 168, 77, 2));
        assert_eq!(eps[0].port, 67);
        assert_eq!(eps[1].local_ip, Ipv4Addr::new(10, 10, 0, 1));
        assert_eq!(eps[2].port, 53);
        assert!(!eps[0].wildcard());
    }

    #[test]
    fn proc_parse_is_bounded() {
        let mut text = String::from("  sl  local_address rem_address   st\n");
        for i in 0..(REPORT_ITEM_CAP + 20) {
            text.push_str(&format!(
                " {i}: 01000A0A:{:04X} 00000000:0000 07 00000000:00000000 00:00000000 00000000 0 0 1 2 x 0\n",
                1024 + i
            ));
        }
        assert_eq!(parse_proc_net_udp(&text).len(), REPORT_ITEM_CAP);
    }

    #[test]
    fn findings_are_bounded() {
        let mut c = cfg();
        c.rndis.units = (0..200).map(|i| format!("unit-{i}.service")).collect();
        let snap = healthy_board();
        assert!(check_ap_plan(&c, &snap).len() <= REPORT_ITEM_CAP);
    }
}
