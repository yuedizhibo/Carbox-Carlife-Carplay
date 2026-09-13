//! Runtime configuration: environment keys, defaults, and strict validation.
//!
//! Every value the sidecar acts on is parsed here once, so the AP launcher, the
//! MFi policy and the preflight checks all see the same validated settings.
//! Parsing is total: an invalid environment yields a bounded list of
//! [`ConfigError`]s instead of a partially usable configuration.

use std::collections::BTreeMap;
use std::net::Ipv4Addr;

use serde::Serialize;

use crate::bounds::{EVENT_RING_CAP, FRAME_CACHE_CAP, IPC_CLIENT_CAP, IPC_LINE_CAP, REPORT_ITEM_CAP};
use crate::mfi::{MfiTarget, AC200_BUS, ALLOWED_MFI_ADDRS, ALLOWED_MFI_BUSES};

/// Non-DFS 5 GHz channels: no radar detection needed, and the board's radio
/// reports support for the UNII-1 half of them.
pub const ALLOWED_5G_CHANNELS: [u8; 8] = [36, 40, 44, 48, 149, 153, 157, 161];
/// 2.4 GHz channels permitted by ETSI/FCC without special casing.
pub const ALLOWED_2G_CHANNELS: std::ops::RangeInclusive<u8> = 1..=13;
/// Port the C++20 core serves its synthetic web UI on; never reused here.
pub const CORE_HTTP_PORT: u16 = 8080;

/// Which protocol engine the sidecar supervises. `CatPlay` is accepted only
/// after the executable capability handshake in `cp-native`; accepting the
/// configuration must not be confused with a negotiated CarPlay session.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum Engine {
    /// Exercises supervision, IPC, status and bounds without Apple traffic.
    SelfTest,
    /// External CatPlay dual-role executable, built from the owner-provided
    /// reference tree rather than copied into this workspace.
    CatPlay,
}

impl Engine {
    pub fn as_str(self) -> &'static str {
        match self {
            Engine::SelfTest => "selftest",
            Engine::CatPlay => "catplay",
        }
    }

    pub fn needs_protocol_listener(self) -> bool {
        matches!(self, Engine::CatPlay)
    }
}

/// USB gadget ownership profiles. They are deliberately mutually exclusive:
/// the Zero 2W's UDC cannot provide RNDIS debug and wired CarPlay at once.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum UsbProfile {
    Development,
    Vehicle,
}

impl UsbProfile {
    pub fn as_str(self) -> &'static str {
        match self {
            UsbProfile::Development => "development",
            UsbProfile::Vehicle => "vehicle",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ApSettings {
    pub iface: String,
    pub ssid: String,
    pub passphrase: String,
    pub channel: u8,
    pub width_mhz: u8,
    /// ISO 3166-1 alpha-2 regulatory domain. Never `00`.
    pub country: String,
    pub ap_ip: Ipv4Addr,
    /// Bounded number of associated stations.
    pub max_stations: u8,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct RndisSettings {
    pub iface: String,
    pub ip: Ipv4Addr,
    pub units: Vec<String>,
    /// When true, inactive debug-link units block `ap-up` and `run`.
    pub required: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct MfiSettings {
    pub target: MfiTarget,
    /// Verified native MFi utility reused for register access.
    pub tool: String,
    /// Power-line GPIO cycled before authentication, `-1` when externally powered.
    pub power_gpio: i32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Limits {
    pub event_ring: usize,
    pub frame_cache: usize,
    pub ipc_line_bytes: usize,
    pub ipc_clients: usize,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct RuntimeConfig {
    pub ap: ApSettings,
    pub rndis: RndisSettings,
    pub mfi: MfiSettings,
    pub airplay_port: u16,
    /// AirPlay pairing key (`pk` TXT record). Empty until a protocol engine
    /// exists; the publisher then omits the record, like the reference receiver.
    pub airplay_pk: String,
    /// AirPlay pairing identifier (`pi` TXT record).
    pub airplay_pi: String,
    pub source_version: String,
    pub device_name: String,
    pub runtime_dir: String,
    pub core_socket: String,
    pub engine: Engine,
    /// Path to the independently-built CatPlay engine executable. It runs
    /// input-only in development and bridge mode in vehicle profile; this is
    /// not consulted for selftest mode.
    pub catplay_engine_path: String,
    pub usb_profile: UsbProfile,
    /// Vehicle mode can only be selected with one of these explicit operator
    /// acknowledgements; neither is assumed from a network interface name.
    pub alternate_management_confirmed: bool,
    pub offline_deployment_confirmed: bool,
    pub limits: Limits,
}

/// One rejected configuration value.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ConfigError {
    pub key: String,
    pub message: String,
}

/// Bounded collection of every rejection found in one parse.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
pub struct ConfigErrors {
    pub items: Vec<ConfigError>,
}

impl ConfigErrors {
    pub fn push(&mut self, key: &str, message: impl Into<String>) {
        if self.items.len() >= REPORT_ITEM_CAP {
            return;
        }
        self.items.push(ConfigError { key: key.to_string(), message: message.into() });
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    pub fn len(&self) -> usize {
        self.items.len()
    }
}

impl std::fmt::Display for ConfigErrors {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        for (i, e) in self.items.iter().enumerate() {
            if i > 0 {
                writeln!(f)?;
            }
            write!(f, "{}: {}", e.key, e.message)?;
        }
        Ok(())
    }
}

impl RuntimeConfig {
    /// Defaults for the Orange Pi Zero 2W board as it is wired today: MFi on
    /// `/dev/i2c-1`, debug link on `usb0`, AP on `wlan0`.
    ///
    /// `country` is intentionally empty: the regulatory domain is a legal
    /// property of the location and must be set by the operator.
    pub fn defaults() -> Self {
        Self {
            ap: ApSettings {
                iface: "wlan0".to_string(),
                ssid: "ZERO2W-CarPlay".to_string(),
                passphrase: "zero2w-carplay".to_string(),
                channel: 36,
                width_mhz: 40,
                country: String::new(),
                ap_ip: Ipv4Addr::new(10, 10, 0, 1),
                max_stations: 4,
            },
            rndis: RndisSettings {
                iface: "usb0".to_string(),
                ip: Ipv4Addr::new(192, 168, 77, 2),
                units: vec!["zero2w-usb-rndis.service".to_string(), "zero2w-usb-dhcp.service".to_string()],
                required: true,
            },
            mfi: MfiSettings { target: MfiTarget { bus: 1, addr: 0x10 }, tool: "/root/MFIAuth/auth3-native".to_string(), power_gpio: -1 },
            airplay_port: 7000,
            airplay_pk: String::new(),
            airplay_pi: String::new(),
            source_version: "950.7.1".to_string(),
            device_name: "ZERO2W".to_string(),
            runtime_dir: "/run/zero2w".to_string(),
            core_socket: "/run/zero2w/cp-native.sock".to_string(),
            engine: Engine::SelfTest,
            catplay_engine_path: "/root/zero2w/bin/cp-catplay-bridge".to_string(),
            usb_profile: UsbProfile::Development,
            alternate_management_confirmed: false,
            offline_deployment_confirmed: false,
            limits: Limits { event_ring: 128, frame_cache: FRAME_CACHE_CAP, ipc_line_bytes: 1024, ipc_clients: IPC_CLIENT_CAP },
        }
    }

    /// Parse and validate against an explicit key/value map.
    pub fn from_map(env: &BTreeMap<String, String>) -> Result<Self, ConfigErrors> {
        let mut cfg = Self::defaults();
        let mut errors = ConfigErrors::default();
        let get = |key: &str| env.get(key).map(String::as_str).filter(|v| !v.is_empty());

        if let Some(v) = get("CP_AP_IFACE") {
            cfg.ap.iface = v.to_string();
        }
        if let Some(v) = get("CP_AP_SSID") {
            cfg.ap.ssid = v.to_string();
        }
        if let Some(v) = get("CP_AP_PASSPHRASE") {
            cfg.ap.passphrase = v.to_string();
        }
        if let Some(v) = get("CP_AP_CHANNEL") {
            cfg.ap.channel = parse_u8(&mut errors, "CP_AP_CHANNEL", v, u8::MAX);
        }
        if let Some(v) = get("CP_AP_WIDTH") {
            cfg.ap.width_mhz = parse_u8(&mut errors, "CP_AP_WIDTH", v, 160);
        }
        if let Some(v) = get("CP_COUNTRY") {
            cfg.ap.country = v.trim().to_uppercase();
        }
        if let Some(v) = get("CP_AP_IP") {
            match v.parse::<Ipv4Addr>() {
                Ok(ip) => cfg.ap.ap_ip = ip,
                Err(_) => errors.push("CP_AP_IP", format!("'{v}' is not an IPv4 address")),
            }
        }
        if let Some(v) = get("CP_AP_MAX_STA") {
            cfg.ap.max_stations = parse_u8(&mut errors, "CP_AP_MAX_STA", v, 16);
        }
        if let Some(v) = get("CP_RNDIS_IFACE") {
            cfg.rndis.iface = v.to_string();
        }
        if let Some(v) = get("CP_RNDIS_IP") {
            match v.parse::<Ipv4Addr>() {
                Ok(ip) => cfg.rndis.ip = ip,
                Err(_) => errors.push("CP_RNDIS_IP", format!("'{v}' is not an IPv4 address")),
            }
        }
        if let Some(v) = get("CP_RNDIS_UNITS") {
            cfg.rndis.units = v.split(',').map(str::trim).filter(|s| !s.is_empty()).take(4).map(str::to_string).collect();
        }
        if let Some(v) = get("CP_REQUIRE_RNDIS") {
            match parse_bool(v) {
                Some(b) => cfg.rndis.required = b,
                None => errors.push("CP_REQUIRE_RNDIS", format!("'{v}' is not 0/1/true/false")),
            }
        }
        if let Some(v) = get("CP_MFI_BUS") {
            cfg.mfi.target.bus = parse_u32(&mut errors, "CP_MFI_BUS", v, 9999);
        }
        if let Some(v) = get("CP_MFI_ADDR") {
            match parse_addr(v) {
                Some(a) => cfg.mfi.target.addr = a,
                None => errors.push("CP_MFI_ADDR", format!("'{v}' is not a 7-bit decimal or 0x hex address")),
            }
        }
        if let Some(v) = get("CP_MFI_TOOL") {
            cfg.mfi.tool = v.to_string();
        }
        if let Some(v) = get("CP_MFI_POWER_GPIO") {
            match v.trim().parse::<i32>() {
                Ok(g) if (-1..=512).contains(&g) => cfg.mfi.power_gpio = g,
                _ => errors.push("CP_MFI_POWER_GPIO", format!("'{v}' must be -1 (external power) or a GPIO line 0..=512")),
            }
        }
        if let Some(v) = get("CP_AIRPLAY_PORT") {
            cfg.airplay_port = parse_u16(&mut errors, "CP_AIRPLAY_PORT", v);
        }
        if let Some(v) = get("CP_AIRPLAY_PK") {
            cfg.airplay_pk = v.trim().to_string();
        }
        if let Some(v) = get("CP_AIRPLAY_PI") {
            cfg.airplay_pi = v.trim().to_string();
        }
        if let Some(v) = get("CP_SOURCE_VERSION") {
            cfg.source_version = v.to_string();
        }
        if let Some(v) = get("CP_DEVICE_NAME") {
            cfg.device_name = v.to_string();
        }
        if let Some(v) = get("CP_RUNTIME_DIR") {
            cfg.runtime_dir = v.to_string();
            cfg.core_socket = format!("{}/cp-native.sock", v.trim_end_matches('/'));
        }
        if let Some(v) = get("CP_CORE_SOCKET") {
            cfg.core_socket = v.to_string();
        }
        if let Some(v) = get("CP_ENGINE") {
            match v.trim().to_ascii_lowercase().as_str() {
                "selftest" => cfg.engine = Engine::SelfTest,
                "catplay" => cfg.engine = Engine::CatPlay,
                other => errors.push("CP_ENGINE", format!("'{other}' is unknown, expected 'selftest' or 'catplay'")),
            }
        }
        if let Some(v) = get("CP_CATPLAY_ENGINE_PATH") {
            cfg.catplay_engine_path = v.to_string();
        }
        if let Some(v) = get("CP_USB_PROFILE") {
            match v.trim().to_ascii_lowercase().as_str() {
                "development" => cfg.usb_profile = UsbProfile::Development,
                "vehicle" => cfg.usb_profile = UsbProfile::Vehicle,
                other => errors.push("CP_USB_PROFILE", format!("'{other}' is unknown, expected 'development' or 'vehicle'")),
            }
        }
        if let Some(v) = get("CP_ALT_MANAGEMENT_CONFIRMED") {
            match parse_bool(v) {
                Some(b) => cfg.alternate_management_confirmed = b,
                None => errors.push("CP_ALT_MANAGEMENT_CONFIRMED", format!("'{v}' is not 0/1/true/false")),
            }
        }
        if let Some(v) = get("CP_OFFLINE_DEPLOYMENT_CONFIRMED") {
            match parse_bool(v) {
                Some(b) => cfg.offline_deployment_confirmed = b,
                None => errors.push("CP_OFFLINE_DEPLOYMENT_CONFIRMED", format!("'{v}' is not 0/1/true/false")),
            }
        }
        if let Some(v) = get("CP_IPC_CLIENTS") {
            cfg.limits.ipc_clients = parse_usize(&mut errors, "CP_IPC_CLIENTS", v, IPC_CLIENT_CAP);
        }
        if let Some(v) = get("CP_EVENT_RING") {
            cfg.limits.event_ring = parse_usize(&mut errors, "CP_EVENT_RING", v, EVENT_RING_CAP);
        }
        if let Some(v) = get("CP_FRAME_CACHE") {
            cfg.limits.frame_cache = parse_usize(&mut errors, "CP_FRAME_CACHE", v, FRAME_CACHE_CAP);
        }
        if let Some(v) = get("CP_IPC_LINE_BYTES") {
            cfg.limits.ipc_line_bytes = parse_usize(&mut errors, "CP_IPC_LINE_BYTES", v, IPC_LINE_CAP);
        }

        cfg.validate(&mut errors);
        if errors.is_empty() {
            Ok(cfg)
        } else {
            Err(errors)
        }
    }

    /// Parse from the process environment.
    pub fn from_std_env() -> Result<Self, ConfigErrors> {
        let env: BTreeMap<String, String> = std::env::vars().collect();
        Self::from_map(&env)
    }

    /// Cross-field and range validation. Errors accumulate so one run reports
    /// every mistake an operator has to fix.
    pub fn validate(&self, errors: &mut ConfigErrors) {
        let ap = &self.ap;
        if ap.iface.is_empty() || ap.iface.len() > 15 || ap.iface.contains('/') || ap.iface.contains(char::is_whitespace) {
            errors.push("CP_AP_IFACE", format!("'{}' is not a usable interface name", ap.iface));
        }
        if ap.iface == self.rndis.iface {
            errors.push("CP_AP_IFACE", format!("'{}' is the USB RNDIS debug interface; the CarPlay AP must use the Wi-Fi radio", ap.iface));
        }
        if ap.ssid.is_empty() || ap.ssid.len() > 32 {
            errors.push("CP_AP_SSID", format!("SSID must be 1..=32 bytes, got {}", ap.ssid.len()));
        }
        if ap.passphrase.len() < 8 || ap.passphrase.len() > 63 {
            errors.push("CP_AP_PASSPHRASE", format!("WPA2-PSK passphrase must be 8..=63 bytes, got {}", ap.passphrase.len()));
        }
        if !channel_allowed(ap.channel) {
            errors.push(
                "CP_AP_CHANNEL",
                format!(
                    "channel {} is not allowed; use {} or a non-DFS 5 GHz channel {:?}",
                    ap.channel, ALLOWED_2G_CHANNELS_STR, ALLOWED_5G_CHANNELS
                ),
            );
        }
        if !matches!(ap.width_mhz, 20 | 40 | 80) {
            errors.push("CP_AP_WIDTH", format!("width must be 20, 40 or 80 MHz, got {}", ap.width_mhz));
        }
        if !country_valid(&ap.country) {
            errors
                .push("CP_COUNTRY", format!("'{}' is not an ISO 3166-1 alpha-2 domain; the world domain 00 cannot host an AP", ap.country));
        }
        if !is_private(ap.ap_ip) {
            errors.push("CP_AP_IP", format!("{} is not a private IPv4 address", ap.ap_ip));
        }
        if same_subnet(ap.ap_ip, self.rndis.ip, 24) || ap.ap_ip == self.rndis.ip {
            errors.push("CP_AP_IP", format!("{} collides with the USB RNDIS debug subnet {}/24", ap.ap_ip, self.rndis.ip));
        }
        if ap.max_stations == 0 {
            errors.push("CP_AP_MAX_STA", "max_stations must be at least 1");
        }
        if self.rndis.iface.is_empty() || self.rndis.iface.len() > 15 {
            errors.push("CP_RNDIS_IFACE", format!("'{}' is not a usable interface name", self.rndis.iface));
        }
        if self.rndis.units.is_empty() {
            errors.push("CP_RNDIS_UNITS", "at least one debug-link unit must be listed");
        }
        if !ALLOWED_MFI_BUSES.contains(&self.mfi.target.bus) {
            let detail = if self.mfi.target.bus == AC200_BUS {
                format!("bus {} is the on-board AC200 (i2c@5002c00) and is refused unconditionally", AC200_BUS)
            } else {
                format!("bus {} is not allowlisted; the external MFi adapter is {:?}", self.mfi.target.bus, ALLOWED_MFI_BUSES)
            };
            errors.push("CP_MFI_BUS", detail);
        }
        if !ALLOWED_MFI_ADDRS.contains(&self.mfi.target.addr) {
            errors.push(
                "CP_MFI_ADDR",
                format!("address 0x{:02X} is not allowlisted; the coprocessor answers at 0x10 or 0x11", self.mfi.target.addr),
            );
        }
        if self.mfi.tool.trim().is_empty() {
            errors.push("CP_MFI_TOOL", "path to the native MFi utility must not be empty");
        }
        if self.airplay_port < 1024 {
            errors.push("CP_AIRPLAY_PORT", format!("port {} is privileged or reserved", self.airplay_port));
        } else if self.airplay_port == CORE_HTTP_PORT {
            errors.push("CP_AIRPLAY_PORT", format!("port {} belongs to the C++20 core web UI", CORE_HTTP_PORT));
        }
        if self.source_version.len() > 32 {
            errors.push("CP_SOURCE_VERSION", format!("must be at most 32 bytes, got {}", self.source_version.len()));
        }
        for (key, value) in [("CP_AIRPLAY_PK", &self.airplay_pk), ("CP_AIRPLAY_PI", &self.airplay_pi)] {
            if value.len() > 128 || !value.bytes().all(|b| b.is_ascii_alphanumeric()) {
                errors.push(key, format!("'{value}' must be at most 128 alphanumeric bytes"));
            }
        }
        if self.device_name.is_empty() || self.device_name.len() > 32 {
            errors.push("CP_DEVICE_NAME", format!("must be 1..=32 bytes, got {}", self.device_name.len()));
        }
        if !self.runtime_dir.starts_with('/') || self.runtime_dir.len() > 128 {
            errors.push("CP_RUNTIME_DIR", format!("'{}' must be an absolute path of at most 128 bytes", self.runtime_dir));
        }
        if !self.core_socket.starts_with('/') || self.core_socket.len() > 108 {
            // 108 is the sun_path limit of a Linux AF_UNIX socket address.
            errors.push("CP_CORE_SOCKET", format!("'{}' must be an absolute path of at most 108 bytes", self.core_socket));
        }
        if self.catplay_engine_path.is_empty() || !self.catplay_engine_path.starts_with('/') || self.catplay_engine_path.len() > 256 {
            errors.push("CP_CATPLAY_ENGINE_PATH", "must be an absolute path of at most 256 bytes");
        }
        if self.usb_profile == UsbProfile::Vehicle && !self.alternate_management_confirmed && !self.offline_deployment_confirmed {
            errors.push(
                "CP_USB_PROFILE",
                "vehicle owns the UDC and disables RNDIS; set CP_ALT_MANAGEMENT_CONFIRMED=1 or CP_OFFLINE_DEPLOYMENT_CONFIRMED=1 deliberately",
            );
        }
        if self.usb_profile == UsbProfile::Vehicle && self.rndis.required {
            errors.push("CP_REQUIRE_RNDIS", "vehicle USB profile cannot require RNDIS; select an alternate management path first");
        }
        if self.limits.event_ring == 0 || self.limits.event_ring > EVENT_RING_CAP {
            errors.push("CP_EVENT_RING", format!("must be 1..={EVENT_RING_CAP}"));
        }
        if self.limits.frame_cache == 0 || self.limits.frame_cache > FRAME_CACHE_CAP {
            errors.push("CP_FRAME_CACHE", format!("must be 1..={FRAME_CACHE_CAP}"));
        }
        if self.limits.ipc_line_bytes < 128 || self.limits.ipc_line_bytes > IPC_LINE_CAP {
            errors.push("CP_IPC_LINE_BYTES", format!("must be 128..={IPC_LINE_CAP}"));
        }
        if self.limits.ipc_clients == 0 || self.limits.ipc_clients > IPC_CLIENT_CAP {
            errors.push("CP_IPC_CLIENTS", format!("must be 1..={IPC_CLIENT_CAP}"));
        }
    }

    /// Redacted view for logs and status output: never prints the passphrase.
    pub fn summary(&self) -> ConfigSummary {
        ConfigSummary {
            ap_iface: self.ap.iface.clone(),
            ssid: self.ap.ssid.clone(),
            passphrase_len: self.ap.passphrase.len(),
            channel: self.ap.channel,
            width_mhz: self.ap.width_mhz,
            country: self.ap.country.clone(),
            ap_ip: self.ap.ap_ip.to_string(),
            max_stations: self.ap.max_stations,
            rndis_iface: self.rndis.iface.clone(),
            rndis_ip: self.rndis.ip.to_string(),
            rndis_required: self.rndis.required,
            mfi_bus: self.mfi.target.bus,
            mfi_addr: format!("0x{:02X}", self.mfi.target.addr),
            mfi_tool: self.mfi.tool.clone(),
            airplay_port: self.airplay_port,
            airplay_pk_len: self.airplay_pk.len(),
            airplay_pi_len: self.airplay_pi.len(),
            engine: self.engine.as_str().to_string(),
            catplay_engine_path: self.catplay_engine_path.clone(),
            usb_profile: self.usb_profile.as_str().to_string(),
            alternate_management_confirmed: self.alternate_management_confirmed,
            offline_deployment_confirmed: self.offline_deployment_confirmed,
            core_socket: self.core_socket.clone(),
            limits: self.limits.clone(),
        }
    }
}

const ALLOWED_2G_CHANNELS_STR: &str = "1..=13";

fn channel_allowed(channel: u8) -> bool {
    ALLOWED_2G_CHANNELS.contains(&channel) || ALLOWED_5G_CHANNELS.contains(&channel)
}

fn country_valid(country: &str) -> bool {
    country.len() == 2 && country.bytes().all(|b| b.is_ascii_alphabetic()) && country != "00"
}

fn is_private(ip: Ipv4Addr) -> bool {
    let [a, b, ..] = ip.octets();
    a == 10 || (a == 172 && (16..=31).contains(&b)) || (a == 192 && b == 168)
}

fn same_subnet(a: Ipv4Addr, b: Ipv4Addr, prefix: u8) -> bool {
    let shift = 32 - u32::from(prefix);
    (u32::from(a) >> shift) == (u32::from(b) >> shift)
}

fn parse_bool(value: &str) -> Option<bool> {
    match value.trim().to_ascii_lowercase().as_str() {
        "1" | "true" | "yes" | "on" => Some(true),
        "0" | "false" | "no" | "off" => Some(false),
        _ => None,
    }
}

fn parse_addr(value: &str) -> Option<u8> {
    let v = value.trim();
    let n = if let Some(hex) = v.strip_prefix("0x").or_else(|| v.strip_prefix("0X")) {
        u8::from_str_radix(hex, 16).ok()?
    } else {
        v.parse::<u8>().ok()?
    };
    if n > 0 && n <= 0x7F {
        Some(n)
    } else {
        None
    }
}

fn parse_u8(errors: &mut ConfigErrors, key: &str, value: &str, max: u8) -> u8 {
    match value.trim().parse::<u16>() {
        Ok(n) if n <= u16::from(max) => n as u8,
        _ => {
            errors.push(key, format!("'{value}' must be a decimal value 0..={max}"));
            0
        }
    }
}

fn parse_u16(errors: &mut ConfigErrors, key: &str, value: &str) -> u16 {
    match value.trim().parse::<u16>() {
        Ok(n) => n,
        Err(_) => {
            errors.push(key, format!("'{value}' must be a decimal port number"));
            0
        }
    }
}

fn parse_u32(errors: &mut ConfigErrors, key: &str, value: &str, max: u32) -> u32 {
    match value.trim().parse::<u64>() {
        Ok(n) if n <= u64::from(max) => n as u32,
        _ => {
            errors.push(key, format!("'{value}' must be a decimal value 0..={max}"));
            u32::MAX
        }
    }
}

fn parse_usize(errors: &mut ConfigErrors, key: &str, value: &str, max: usize) -> usize {
    match value.trim().parse::<usize>() {
        Ok(n) if n <= max => n,
        _ => {
            errors.push(key, format!("'{value}' must be a decimal value 0..={max}"));
            0
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ConfigSummary {
    pub ap_iface: String,
    pub ssid: String,
    pub passphrase_len: usize,
    pub channel: u8,
    pub width_mhz: u8,
    pub country: String,
    pub ap_ip: String,
    pub max_stations: u8,
    pub rndis_iface: String,
    pub rndis_ip: String,
    pub rndis_required: bool,
    pub mfi_bus: u32,
    pub mfi_addr: String,
    pub mfi_tool: String,
    pub airplay_port: u16,
    pub airplay_pk_len: usize,
    pub airplay_pi_len: usize,
    pub engine: String,
    pub catplay_engine_path: String,
    pub usb_profile: String,
    pub alternate_management_confirmed: bool,
    pub offline_deployment_confirmed: bool,
    pub core_socket: String,
    pub limits: Limits,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn env(pairs: &[(&str, &str)]) -> BTreeMap<String, String> {
        pairs.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()
    }

    fn valid_env() -> BTreeMap<String, String> {
        env(&[("CP_COUNTRY", "US")])
    }

    #[test]
    fn defaults_need_only_a_country() {
        let cfg = RuntimeConfig::from_map(&valid_env()).expect("valid");
        assert_eq!(cfg.ap.iface, "wlan0");
        assert_eq!(cfg.rndis.iface, "usb0");
        assert_eq!(cfg.mfi.target, MfiTarget { bus: 1, addr: 0x10 });
        assert_eq!(cfg.airplay_port, 7000);
        assert_eq!(cfg.engine, Engine::SelfTest);
        assert_eq!(cfg.core_socket, "/run/zero2w/cp-native.sock");
    }

    #[test]
    fn missing_country_is_rejected() {
        let errors = RuntimeConfig::from_map(&env(&[])).unwrap_err();
        assert!(errors.items.iter().any(|e| e.key == "CP_COUNTRY" && e.message.contains("world domain 00")), "{errors}");
    }

    #[test]
    fn world_domain_zero_zero_is_rejected() {
        // The board currently reports `country 00`; that must not reach hostapd.
        let errors = RuntimeConfig::from_map(&env(&[("CP_COUNTRY", "00")])).unwrap_err();
        assert!(errors.items.iter().any(|e| e.key == "CP_COUNTRY"), "{errors}");
    }

    #[test]
    fn mfi_bus_two_is_refused_even_when_explicitly_configured() {
        let mut base = valid_env();
        base.insert("CP_MFI_BUS".to_string(), "2".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        let bus = errors.items.iter().find(|e| e.key == "CP_MFI_BUS").expect("bus error");
        assert!(bus.message.contains("AC200"), "{bus:?}");
        assert!(bus.message.contains("unconditionally"), "{bus:?}");
    }

    #[test]
    fn mfi_bus_outside_allowlist_is_refused() {
        let mut base = valid_env();
        base.insert("CP_MFI_BUS".to_string(), "0".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        assert!(errors.items.iter().any(|e| e.key == "CP_MFI_BUS" && e.message.contains("not allowlisted")), "{errors}");
    }

    #[test]
    fn mfi_address_outside_allowlist_is_refused() {
        let mut base = valid_env();
        base.insert("CP_MFI_ADDR".to_string(), "0x36".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        assert!(errors.items.iter().any(|e| e.key == "CP_MFI_ADDR"), "{errors}");
    }

    #[test]
    fn mfi_address_accepts_hex_and_decimal() {
        for (text, expected) in [("0x10", 0x10), ("0X11", 0x11), ("16", 0x10), ("17", 0x11)] {
            let mut base = valid_env();
            base.insert("CP_MFI_ADDR".to_string(), text.to_string());
            let cfg = RuntimeConfig::from_map(&base).unwrap_or_else(|e| panic!("{text}: {e}"));
            assert_eq!(cfg.mfi.target.addr, expected, "{text}");
        }
    }

    #[test]
    fn ap_iface_must_not_be_the_debug_link() {
        let mut base = valid_env();
        base.insert("CP_AP_IFACE".to_string(), "usb0".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        let hit = errors.items.iter().find(|e| e.key == "CP_AP_IFACE").expect("iface error");
        assert!(hit.message.contains("USB RNDIS"), "{hit:?}");
    }

    #[test]
    fn ap_ip_must_not_collide_with_the_debug_subnet() {
        let mut base = valid_env();
        base.insert("CP_AP_IP".to_string(), "192.168.77.1".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        assert!(errors.items.iter().any(|e| e.key == "CP_AP_IP" && e.message.contains("RNDIS")), "{errors}");
    }

    #[test]
    fn ap_ip_must_be_private() {
        let mut base = valid_env();
        base.insert("CP_AP_IP".to_string(), "8.8.8.8".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        assert!(errors.items.iter().any(|e| e.message.contains("not a private IPv4")), "{errors}");
    }

    #[test]
    fn dfs_channels_are_refused() {
        for channel in ["52", "100", "144"] {
            let mut base = valid_env();
            base.insert("CP_AP_CHANNEL".to_string(), channel.to_string());
            let errors = RuntimeConfig::from_map(&base).unwrap_err();
            assert!(errors.items.iter().any(|e| e.key == "CP_AP_CHANNEL"), "channel {channel}: {errors}");
        }
    }

    #[test]
    fn non_dfs_channels_are_accepted() {
        for channel in ["1", "6", "13", "36", "44", "149", "161"] {
            let mut base = valid_env();
            base.insert("CP_AP_CHANNEL".to_string(), channel.to_string());
            assert!(RuntimeConfig::from_map(&base).is_ok(), "channel {channel} should be accepted");
        }
    }

    #[test]
    fn airplay_port_must_not_steal_the_core_port() {
        let mut base = valid_env();
        base.insert("CP_AIRPLAY_PORT".to_string(), "8080".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        assert!(errors.items.iter().any(|e| e.key == "CP_AIRPLAY_PORT" && e.message.contains("core web UI")), "{errors}");
    }

    #[test]
    fn engine_catplay_is_accepted_but_requires_a_runtime_handshake() {
        let mut base = valid_env();
        base.insert("CP_ENGINE".to_string(), "catplay".to_string());
        let cfg = RuntimeConfig::from_map(&base).expect("configuration is not a capability claim");
        assert_eq!(cfg.engine, Engine::CatPlay);
        assert!(cfg.engine.needs_protocol_listener());
    }

    #[test]
    fn ipc_client_limit_is_parsed_and_hard_capped() {
        let mut base = valid_env();
        base.insert("CP_IPC_CLIENTS".to_string(), "1".to_string());
        assert_eq!(RuntimeConfig::from_map(&base).expect("valid").limits.ipc_clients, 1);
        base.insert("CP_IPC_CLIENTS".to_string(), "99".to_string());
        assert!(RuntimeConfig::from_map(&base).is_err());
    }

    #[test]
    fn vehicle_profile_requires_an_explicit_management_plan() {
        let mut base = valid_env();
        base.insert("CP_USB_PROFILE".to_string(), "vehicle".to_string());
        base.insert("CP_REQUIRE_RNDIS".to_string(), "0".to_string());
        assert!(RuntimeConfig::from_map(&base).is_err());
        base.insert("CP_ALT_MANAGEMENT_CONFIRMED".to_string(), "1".to_string());
        let cfg = RuntimeConfig::from_map(&base).expect("explicit alternate path");
        assert_eq!(cfg.usb_profile, UsbProfile::Vehicle);
    }

    #[test]
    fn runtime_dir_moves_the_socket() {
        let mut base = valid_env();
        base.insert("CP_RUNTIME_DIR".to_string(), "/run/carplay".to_string());
        let cfg = RuntimeConfig::from_map(&base).expect("valid");
        assert_eq!(cfg.core_socket, "/run/carplay/cp-native.sock");
    }

    #[test]
    fn socket_path_longer_than_sun_path_is_refused() {
        let mut base = valid_env();
        base.insert("CP_CORE_SOCKET".to_string(), format!("/run/zero2w/{}.sock", "x".repeat(120)));
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        assert!(errors.items.iter().any(|e| e.key == "CP_CORE_SOCKET"), "{errors}");
    }

    #[test]
    fn bounds_cannot_be_raised_past_their_caps() {
        let mut base = valid_env();
        base.insert("CP_EVENT_RING".to_string(), "100000".to_string());
        base.insert("CP_FRAME_CACHE".to_string(), "100000".to_string());
        base.insert("CP_IPC_LINE_BYTES".to_string(), "1000000".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        for key in ["CP_EVENT_RING", "CP_FRAME_CACHE", "CP_IPC_LINE_BYTES"] {
            assert!(errors.items.iter().any(|e| e.key == key), "missing rejection for {key}: {errors}");
        }
    }

    #[test]
    fn passphrase_length_is_enforced() {
        let mut base = valid_env();
        base.insert("CP_AP_PASSPHRASE".to_string(), "short".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        assert!(errors.items.iter().any(|e| e.key == "CP_AP_PASSPHRASE"), "{errors}");
    }

    #[test]
    fn errors_are_bounded() {
        let mut errors = ConfigErrors::default();
        for i in 0..(REPORT_ITEM_CAP + 10) {
            errors.push("K", format!("m{i}"));
        }
        assert_eq!(errors.len(), REPORT_ITEM_CAP);
    }

    #[test]
    fn airplay_pairing_values_must_be_alphanumeric() {
        let mut base = valid_env();
        base.insert("CP_AIRPLAY_PK".to_string(), "has space".to_string());
        let errors = RuntimeConfig::from_map(&base).unwrap_err();
        assert!(errors.items.iter().any(|e| e.key == "CP_AIRPLAY_PK"), "{errors}");

        let mut base = valid_env();
        base.insert("CP_AIRPLAY_PK".to_string(), "abc123".to_string());
        base.insert("CP_AIRPLAY_PI".to_string(), "def456".to_string());
        let cfg = RuntimeConfig::from_map(&base).expect("valid");
        assert_eq!(cfg.airplay_pk, "abc123");
        assert_eq!(cfg.summary().airplay_pk_len, 6);
    }

    #[test]
    fn summary_never_leaks_the_passphrase() {
        let cfg = RuntimeConfig::from_map(&valid_env()).expect("valid");
        let json = serde_json::to_string(&cfg.summary()).expect("json");
        assert!(!json.contains(&cfg.ap.passphrase), "{json}");
        assert!(json.contains("\"passphrase_len\":14"), "{json}");
    }

    #[test]
    fn private_and_subnet_helpers() {
        assert!(is_private(Ipv4Addr::new(10, 10, 0, 1)));
        assert!(is_private(Ipv4Addr::new(172, 20, 0, 1)));
        assert!(is_private(Ipv4Addr::new(192, 168, 1, 1)));
        assert!(!is_private(Ipv4Addr::new(172, 32, 0, 1)));
        assert!(same_subnet(Ipv4Addr::new(192, 168, 77, 1), Ipv4Addr::new(192, 168, 77, 2), 24));
        assert!(!same_subnet(Ipv4Addr::new(192, 168, 78, 1), Ipv4Addr::new(192, 168, 77, 2), 24));
    }
}
