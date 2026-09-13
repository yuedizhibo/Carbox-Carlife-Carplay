// CarPlay AP plan: hostapd, dnsmasq, NetworkManager takeover and mDNS records.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from LIVI's native AP owner, Reference/LIVI/native/livi-helperd/crates/
// livi-runtime/src/wifi_ap.rs and .../src/bonjour.rs, (C) LIVI contributors,
// GPL-3.0-or-later. See ../../../LICENSE-NOTICES.md and ../../../licenses/GPL-3.0.txt.
//
// The hostapd/dnsmasq knowledge that matters for CarPlay is preserved verbatim:
// the Apple vendor element `dd0800a04000000200{band}` in both
// `vendor_elements` and `assocresp_elements`, the non-DFS 80 MHz centre segments
// (42 / 155), and the HT40 secondary-channel sign. Everything is generated as
// data here so the launcher in `cp-native` can be dry-run and the rules can be
// unit-tested off-target.
//
// Deliberate differences from LIVI, all needed on this board:
// * configs live under the configured runtime dir (tmpfs, root-owned) instead of
//   /tmp, so two instances can never fight over one path;
// * dnsmasq binds only the AP interface, suppresses router/DNS options and
//   caps the lease pool, because a second dnsmasq already serves the USB RNDIS
//   debug link on 192.168.77.2 and the AP has no upstream;
// * mDNS is published on the AP interface only;
// * the EUI-64 link-local formatter keeps two hex digits for the first byte
//   (LIVI's `{:x}` drops a leading zero for MACs whose first byte XOR 0x02 is
//   below 0x10, producing a malformed address).

use serde::Serialize;

use crate::config::RuntimeConfig;

/// CarPlay service types, as published by the receiver.
pub const AIRPLAY_SERVICE: &str = "_airplay._tcp";
/// The phone advertises its control endpoint under this type.
pub const CARPLAY_CTRL_SERVICE: &str = "_carplay-ctrl._tcp";
/// AirPlay TXT record set the phone filters on.
pub const AIRPLAY_FEATURES: &str = "0x44540380,0x61";
pub const AIRPLAY_FLAGS: &str = "0x4";
pub const AIRPLAY_PROTOVERS: &str = "1.1";
/// hostapd control interface used by `hostapd_cli`.
pub const HOSTAPD_CTRL_DIR: &str = "/var/run/hostapd";
/// First and last address of the bounded DHCP pool handed to phones.
pub const DHCP_RANGE_FIRST: u8 = 10;
pub const DHCP_RANGE_LAST: u8 = 50;

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Command {
    pub program: String,
    pub args: Vec<String>,
}

impl Command {
    pub fn new(program: &str, args: &[&str]) -> Self {
        Self { program: program.to_string(), args: args.iter().map(|a| a.to_string()).collect() }
    }

    pub fn display(&self) -> String {
        let mut out = self.program.clone();
        for a in &self.args {
            out.push(' ');
            out.push_str(a);
        }
        out
    }
}

/// Runtime file layout, derived from the configured runtime directory.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ApPaths {
    pub runtime_dir: String,
    pub hostapd_conf: String,
    pub hostapd_log: String,
    pub dnsmasq_conf: String,
    pub dnsmasq_leases: String,
    pub nm_unmanaged_conf: String,
    pub pid_dir: String,
}

impl ApPaths {
    pub fn new(runtime_dir: &str) -> Self {
        let dir = runtime_dir.trim_end_matches('/');
        Self {
            runtime_dir: dir.to_string(),
            hostapd_conf: format!("{dir}/cp-hostapd.conf"),
            hostapd_log: format!("{dir}/cp-hostapd.log"),
            dnsmasq_conf: format!("{dir}/cp-dnsmasq.conf"),
            dnsmasq_leases: format!("{dir}/cp-dnsmasq.leases"),
            nm_unmanaged_conf: "/etc/NetworkManager/conf.d/99-zero2w-cp-ap-unmanaged.conf".to_string(),
            pid_dir: dir.to_string(),
        }
    }
}

/// Centre segment for 80 MHz VHT. Non-DFS blocks only: UNII-1 -> 42, UNII-3 -> 155.
pub fn vht_centre(primary: u8) -> Option<u8> {
    match primary {
        36..=48 => Some(42),
        149..=161 => Some(155),
        _ => None,
    }
}

/// HT40 secondary position: the lower channel of each pair uses '+', the upper '-'.
pub fn ht40_secondary(primary: u8) -> char {
    if (primary / 4) % 2 == 1 {
        '+'
    } else {
        '-'
    }
}

/// Apple's vendor element. Without it the SSID is not offered as a CarPlay car.
///
/// `dd 08` + OUI `00:a0:40` + type `00` + `00 02 00` + band, where band is
/// `0x21` on 5 GHz and `0x22` on 2.4 GHz.
pub fn apple_vendor_element(channel: u8) -> String {
    let band_bit: u8 = if channel >= 36 { 0x01 } else { 0x02 };
    format!("dd0800a04000000200{:02x}", 0x20 | band_bit)
}

/// Radio section: 2.4 GHz stays HT20 in `g`, 5 GHz enables 11n/11ac and only
/// widens where the chip and the regulatory domain can follow.
pub fn radio_section(cfg: &RuntimeConfig) -> String {
    let ch = cfg.ap.channel;
    if ch < 36 {
        return format!("hw_mode=g\nchannel={ch}\nieee80211n=1\n");
    }
    let mut s = format!("hw_mode=a\nchannel={ch}\nieee80211n=1\nieee80211ac=1\n");
    let want80 = cfg.ap.width_mhz >= 80 && vht_centre(ch).is_some();
    if cfg.ap.width_mhz >= 40 || want80 {
        s.push_str(&format!("ht_capab=[HT40{}]\n", ht40_secondary(ch)));
    }
    if want80 {
        let centre = vht_centre(ch).expect("checked above");
        s.push_str(&format!("vht_capab=[SHORT-GI-80]\nvht_oper_chwidth=1\nvht_oper_centr_freq_seg0_idx={centre}\n"));
    }
    s
}

pub fn hostapd_conf(cfg: &RuntimeConfig) -> String {
    let ie = apple_vendor_element(cfg.ap.channel);
    format!(
        "interface={iface}\ndriver=nl80211\nctrl_interface={ctrl}\nssid={ssid}\n\
         country_code={country}\nieee80211d=1\nieee80211h=0\n{radio}\
         ignore_broadcast_ssid=0\nwmm_enabled=1\nmax_num_sta={max_sta}\nap_max_inactivity=60\n\
         vendor_elements={ie}\nassocresp_elements={ie}\n\
         wpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\nwpa_passphrase={pass}\n",
        iface = cfg.ap.iface,
        ctrl = HOSTAPD_CTRL_DIR,
        ssid = cfg.ap.ssid,
        country = cfg.ap.country,
        radio = radio_section(cfg),
        max_sta = cfg.ap.max_stations,
        ie = ie,
        pass = cfg.ap.passphrase,
    )
}

/// DHCP and nothing else, bound to the AP interface alone. The RNDIS dnsmasq
/// keeps 192.168.77.0/24; `bind-interfaces` is what makes the two coexist.
///
/// `port=0` disables the DNS half (the AP has no upstream and CarPlay discovers
/// over mDNS), and `dhcp-option=3`/`dhcp-option=6` suppress the router and DNS
/// offers so the phone keeps its cellular default route — the same pair the
/// verified `zero2w-usb-dhcp.service` already sends.
pub fn dnsmasq_conf(cfg: &RuntimeConfig, paths: &ApPaths) -> String {
    let base = cfg.ap.ap_ip.to_string();
    let base = base.rsplit_once('.').map(|(b, _)| b.to_string()).unwrap_or_default();
    let leases = usize::from(DHCP_RANGE_LAST) - usize::from(DHCP_RANGE_FIRST) + 1;
    format!(
        "interface={iface}\nbind-interfaces\nexcept-interface=lo\nport=0\n\
         dhcp-range={base}.{first},{base}.{last},255.255.255.0,12h\ndhcp-lease-max={leases}\n\
         dhcp-leasefile={leasefile}\ndhcp-option=3\ndhcp-option=6\n",
        iface = cfg.ap.iface,
        first = DHCP_RANGE_FIRST,
        last = DHCP_RANGE_LAST,
        leases = leases,
        leasefile = paths.dnsmasq_leases,
    )
}

/// Keep NetworkManager off the AP interface only. The RNDIS interface must
/// never appear here or the debug link does not come back after a reboot.
pub fn nm_unmanaged_conf(ap_iface: &str) -> String {
    format!("[keyfile]\nunmanaged-devices=interface-name:{ap_iface}\n")
}

pub fn parse_mac(text: &str) -> Option<[u8; 6]> {
    let mut mac = [0u8; 6];
    let mut seen = 0;
    for part in text.trim().split(':') {
        if seen == 6 || part.len() != 2 {
            return None;
        }
        mac[seen] = u8::from_str_radix(part, 16).ok()?;
        seen += 1;
    }
    if seen == 6 {
        Some(mac)
    } else {
        None
    }
}

/// EUI-64 link-local address for `mac`, with the universal/local bit flipped.
pub fn eui64_link_local(mac: [u8; 6]) -> String {
    format!("fe80::{:02x}{:02x}:{:02x}ff:fe{:02x}:{:02x}{:02x}", mac[0] ^ 0x02, mac[1], mac[2], mac[3], mac[4], mac[5])
}

/// AirPlay `deviceid`: the interface MAC in colon form.
pub fn device_id_from_mac(mac: [u8; 6]) -> String {
    format!("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5])
}

/// TXT records for `_airplay._tcp`, in the order the reference receiver uses.
/// `pk`/`pi` are the AirPlay pairing key/identifier and are omitted when unset.
pub fn airplay_txt_records(device_id: &str, cfg: &RuntimeConfig, pk: &str, pi: &str) -> Vec<String> {
    let mut txt = vec![
        format!("deviceid={device_id}"),
        format!("features={AIRPLAY_FEATURES}"),
        format!("flags={AIRPLAY_FLAGS}"),
        format!("model={}", cfg.device_name),
        format!("srcvers={}", cfg.source_version),
        format!("protovers={AIRPLAY_PROTOVERS}"),
    ];
    if !pi.is_empty() {
        txt.push(format!("pi={pi}"));
    }
    if !pk.is_empty() {
        txt.push(format!("pk={pk}"));
    }
    txt
}

/// Publish the receiver on the AP interface only.
pub fn avahi_publish_argv(cfg: &RuntimeConfig, device_id: &str, pk: &str, pi: &str) -> Vec<String> {
    let mut argv =
        vec!["avahi-publish-service".to_string(), cfg.device_name.clone(), AIRPLAY_SERVICE.to_string(), cfg.airplay_port.to_string()];
    argv.push("--interface".to_string());
    argv.push(cfg.ap.iface.clone());
    argv.extend(airplay_txt_records(device_id, cfg, pk, pi));
    argv
}

/// Look for the phone's control endpoint, again scoped to the AP interface.
pub fn avahi_browse_argv(cfg: &RuntimeConfig) -> Vec<String> {
    vec![
        "avahi-browse".to_string(),
        "--parsable".to_string(),
        "--terminate".to_string(),
        "--ignore-local".to_string(),
        "--interface".to_string(),
        cfg.ap.iface.clone(),
        CARPLAY_CTRL_SERVICE.to_string(),
    ]
}

/// Address and regulatory setup, in order. `iw reg set` runs first because
/// hostapd refuses a 5 GHz channel while the domain is still the world one.
pub fn interface_setup_commands(cfg: &RuntimeConfig) -> Vec<Command> {
    vec![
        Command::new("iw", &["reg", "set", &cfg.ap.country]),
        Command::new("ip", &["link", "set", &cfg.ap.iface, "up"]),
        Command::new("ip", &["addr", "flush", "dev", &cfg.ap.iface, "scope", "global"]),
        Command::new("ip", &["addr", "add", &format!("{}/24", cfg.ap.ap_ip), "dev", &cfg.ap.iface]),
    ]
}

/// CarPlay negotiates over IPv6 link-local, so the AP interface needs one even
/// when the kernel did not generate it.
pub fn link_local_commands(cfg: &RuntimeConfig, mac: Option<[u8; 6]>, has_link_local: bool) -> Vec<Command> {
    let mut out = vec![
        Command::new("sysctl", &["-qw", &format!("net.ipv6.conf.{}.disable_ipv6=0", cfg.ap.iface)]),
        Command::new("sysctl", &["-qw", &format!("net.ipv6.conf.{}.addr_gen_mode=0", cfg.ap.iface)]),
    ];
    if !has_link_local {
        if let Some(mac) = mac {
            out.push(Command::new(
                "ip",
                &["-6", "addr", "add", &format!("{}/64", eui64_link_local(mac)), "dev", &cfg.ap.iface, "scope", "link", "nodad"],
            ));
        }
    }
    out
}

/// Release the AP interface from NetworkManager. Scoped to one interface: the
/// RNDIS debug link stays managed and keeps its DHCP service.
pub fn nm_release_commands(cfg: &RuntimeConfig) -> Vec<Command> {
    vec![
        Command::new("nmcli", &["device", "set", &cfg.ap.iface, "managed", "no"]),
        Command::new("nmcli", &["device", "disconnect", &cfg.ap.iface]),
        Command::new("systemctl", &["stop", &format!("wpa_supplicant@{}.service", cfg.ap.iface)]),
        Command::new("rfkill", &["unblock", "wifi"]),
    ]
}

pub fn hostapd_argv(paths: &ApPaths) -> Vec<String> {
    vec!["hostapd".to_string(), paths.hostapd_conf.clone()]
}

pub fn dnsmasq_argv(paths: &ApPaths) -> Vec<String> {
    vec!["dnsmasq".to_string(), "--keep-in-foreground".to_string(), format!("--conf-file={}", paths.dnsmasq_conf)]
}

pub fn hostapd_status_command(cfg: &RuntimeConfig) -> Command {
    Command::new("hostapd_cli", &["-p", HOSTAPD_CTRL_DIR, "-i", &cfg.ap.iface, "status"])
}

/// Parse `state=` out of `hostapd_cli status`.
pub fn hostapd_state(status_output: &str) -> String {
    status_output.lines().find_map(|l| l.strip_prefix("state=")).unwrap_or("").trim().to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    fn cfg(channel: u8, width: u8) -> RuntimeConfig {
        let mut env = BTreeMap::new();
        env.insert("CP_COUNTRY".to_string(), "US".to_string());
        env.insert("CP_AP_CHANNEL".to_string(), channel.to_string());
        env.insert("CP_AP_WIDTH".to_string(), width.to_string());
        RuntimeConfig::from_map(&env).unwrap_or_else(|e| panic!("{channel}/{width}: {e}"))
    }

    #[test]
    fn width_80_uses_a_non_dfs_vht_block() {
        let r = radio_section(&cfg(36, 80));
        assert!(r.contains("ht_capab=[HT40+]"), "{r}");
        assert!(r.contains("vht_oper_chwidth=1"), "{r}");
        assert!(r.contains("vht_oper_centr_freq_seg0_idx=42"), "{r}");
    }

    #[test]
    fn width_80_degrades_outside_a_usable_block() {
        let r = radio_section(&cfg(44, 80));
        assert!(r.contains("ht_capab=[HT40+]"), "{r}");
        assert!(r.contains("vht_oper_centr_freq_seg0_idx=42"), "{r}");
        let r = radio_section(&cfg(161, 80));
        assert!(r.contains("vht_oper_centr_freq_seg0_idx=155"), "{r}");
    }

    #[test]
    fn width_40_signs_the_secondary_channel() {
        assert!(radio_section(&cfg(36, 40)).contains("[HT40+]"));
        assert!(radio_section(&cfg(40, 40)).contains("[HT40-]"));
        assert!(radio_section(&cfg(149, 40)).contains("[HT40+]"));
        assert!(radio_section(&cfg(153, 40)).contains("[HT40-]"));
    }

    #[test]
    fn width_20_stays_narrow() {
        let r = radio_section(&cfg(36, 20));
        assert!(!r.contains("ht_capab"), "{r}");
        assert!(!r.contains("vht"), "{r}");
    }

    #[test]
    fn band_24ghz_is_ht20_g() {
        let r = radio_section(&cfg(6, 80));
        assert!(r.contains("hw_mode=g"), "{r}");
        assert!(!r.contains("ht_capab"), "{r}");
    }

    #[test]
    fn apple_vendor_element_encodes_the_band() {
        assert_eq!(apple_vendor_element(36), "dd0800a0400000020021");
        assert_eq!(apple_vendor_element(149), "dd0800a0400000020021");
        assert_eq!(apple_vendor_element(6), "dd0800a0400000020022");
        // dd = element id, 08 = payload length, then exactly 8 payload bytes.
        assert_eq!(apple_vendor_element(36).len(), 20);
    }

    #[test]
    fn hostapd_conf_carries_the_apple_element_on_beacon_and_assoc_response() {
        let conf = hostapd_conf(&cfg(36, 40));
        assert_eq!(conf.matches("dd0800a0400000020021").count(), 2, "{conf}");
        assert!(conf.contains("interface=wlan0\n"), "{conf}");
        assert!(conf.contains("country_code=US\n"), "{conf}");
        assert!(conf.contains("ieee80211d=1\n"), "{conf}");
        assert!(conf.contains("max_num_sta=4\n"), "{conf}");
        assert!(conf.contains("wpa=2\n"), "{conf}");
        assert!(conf.contains("ssid=ZERO2W-CarPlay\n"), "{conf}");
        assert!(!conf.contains('\r'), "{conf}");
    }

    #[test]
    fn dnsmasq_conf_is_scoped_to_the_ap_interface_and_bounded() {
        let paths = ApPaths::new("/run/zero2w");
        let conf = dnsmasq_conf(&cfg(36, 40), &paths);
        assert!(conf.contains("interface=wlan0\n"), "{conf}");
        assert!(conf.contains("bind-interfaces\n"), "{conf}");
        assert!(conf.contains("dhcp-range=10.10.0.10,10.10.0.50,255.255.255.0,12h\n"), "{conf}");
        assert!(conf.contains("dhcp-lease-max=41\n"), "{conf}");
        assert!(conf.contains("dhcp-leasefile=/run/zero2w/cp-dnsmasq.leases\n"), "{conf}");
        assert!(!conf.contains("usb0"), "{conf}");
        assert!(!conf.contains("192.168.77"), "{conf}");
    }

    #[test]
    fn nm_unmanaged_conf_names_only_the_ap_interface() {
        let conf = nm_unmanaged_conf("wlan0");
        assert_eq!(conf, "[keyfile]\nunmanaged-devices=interface-name:wlan0\n");
        assert!(!conf.contains("usb0"), "{conf}");
    }

    #[test]
    fn paths_follow_the_runtime_dir() {
        let paths = ApPaths::new("/run/zero2w/");
        assert_eq!(paths.hostapd_conf, "/run/zero2w/cp-hostapd.conf");
        assert_eq!(paths.dnsmasq_conf, "/run/zero2w/cp-dnsmasq.conf");
        assert_eq!(paths.runtime_dir, "/run/zero2w");
    }

    #[test]
    fn setup_order_sets_the_domain_before_addresses() {
        let cmds = interface_setup_commands(&cfg(36, 40));
        assert_eq!(cmds[0].display(), "iw reg set US");
        assert_eq!(cmds[1].display(), "ip link set wlan0 up");
        assert_eq!(cmds[2].display(), "ip addr flush dev wlan0 scope global");
        assert_eq!(cmds[3].display(), "ip addr add 10.10.0.1/24 dev wlan0");
        assert!(!cmds.iter().any(|c| c.display().contains("usb0")), "the debug link must not be touched");
    }

    #[test]
    fn link_local_is_only_added_when_missing() {
        let mac = parse_mac("aa:bb:cc:dd:ee:ff").expect("mac");
        let with = link_local_commands(&cfg(36, 40), Some(mac), true);
        assert_eq!(with.len(), 2, "{with:?}");
        let without = link_local_commands(&cfg(36, 40), Some(mac), false);
        assert_eq!(without.len(), 3);
        assert_eq!(without[2].display(), "ip -6 addr add fe80::a8bb:ccff:fedd:eeff/64 dev wlan0 scope link nodad");
    }

    #[test]
    fn eui64_keeps_two_digits_for_a_small_first_byte() {
        // 00 ^ 0x02 = 0x02: LIVI's `{:x}` would emit "211:22ff:..." here.
        let mac = parse_mac("00:11:22:33:44:55").expect("mac");
        assert_eq!(eui64_link_local(mac), "fe80::0211:22ff:fe33:4455");
        let mac = parse_mac("AA:BB:CC:DD:EE:FF").expect("mac");
        assert_eq!(eui64_link_local(mac), "fe80::a8bb:ccff:fedd:eeff");
    }

    #[test]
    fn device_id_and_txt_records_match_the_reference_set() {
        let mac = parse_mac("aa:bb:cc:dd:ee:ff").expect("mac");
        let device_id = device_id_from_mac(mac);
        assert_eq!(device_id, "AA:BB:CC:DD:EE:FF");
        let cfg = cfg(36, 40);
        let txt = airplay_txt_records(&device_id, &cfg, "pkvalue", "");
        assert_eq!(
            txt,
            vec![
                "deviceid=AA:BB:CC:DD:EE:FF".to_string(),
                "features=0x44540380,0x61".to_string(),
                "flags=0x4".to_string(),
                "model=ZERO2W".to_string(),
                "srcvers=950.7.1".to_string(),
                "protovers=1.1".to_string(),
                "pk=pkvalue".to_string(),
            ]
        );
    }

    #[test]
    fn avahi_publish_is_scoped_to_the_ap_interface() {
        let cfg = cfg(36, 40);
        let argv = avahi_publish_argv(&cfg, "AA:BB:CC:DD:EE:FF", "", "");
        assert_eq!(argv[0], "avahi-publish-service");
        assert_eq!(argv[1], "ZERO2W");
        assert_eq!(argv[2], AIRPLAY_SERVICE);
        assert_eq!(argv[3], "7000");
        let i = argv.iter().position(|a| a == "--interface").expect("scoped");
        assert_eq!(argv[i + 1], "wlan0");
        assert!(!argv.iter().any(|a| a.contains("usb0")), "{argv:?}");
    }

    #[test]
    fn avahi_browse_targets_the_control_service() {
        let argv = avahi_browse_argv(&cfg(36, 40));
        assert_eq!(argv[0], "avahi-browse");
        assert!(argv.iter().any(|a| a == CARPLAY_CTRL_SERVICE), "{argv:?}");
        assert!(argv.iter().any(|a| a == "--terminate"), "{argv:?}");
    }

    #[test]
    fn nm_release_touches_only_the_ap_interface() {
        let cmds = nm_release_commands(&cfg(36, 40));
        assert!(cmds.iter().all(|c| !c.display().contains("usb0")), "{cmds:?}");
        assert!(cmds.iter().any(|c| c.display() == "nmcli device set wlan0 managed no"));
        assert!(cmds.iter().any(|c| c.display() == "rfkill unblock wifi"));
    }

    #[test]
    fn hostapd_state_is_parsed_from_status_output() {
        assert_eq!(hostapd_state("mode=AP\nstate=ENABLED\nchannel=36\n"), "ENABLED");
        assert_eq!(hostapd_state("mode=AP\n"), "");
        assert_eq!(hostapd_status_command(&cfg(36, 40)).display(), "hostapd_cli -p /var/run/hostapd -i wlan0 status");
    }

    #[test]
    fn mac_parsing_rejects_garbage() {
        assert_eq!(parse_mac("aa:bb:cc:dd:ee:ff"), Some([0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff]));
        assert_eq!(parse_mac("AA:BB:CC:DD:EE:FF\n"), Some([0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff]));
        assert_eq!(parse_mac("aa:bb:cc:dd:ee:zz"), None);
        assert_eq!(parse_mac("aa:bb:cc:dd:ee"), None);
        assert_eq!(parse_mac("aa:bb:cc:dd:ee:ff:00"), None);
        assert_eq!(parse_mac(""), None);
    }
}
