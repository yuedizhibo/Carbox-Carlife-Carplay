//! Portable policy, configuration, AP-plan and IPC logic for the Zero2W real
//! Wireless-CarPlay sidecar (`cp-native`).
//!
//! Everything in this crate is pure: it takes snapshots of the outside world as
//! data and returns decisions and command plans as data. The `cp-native` binary
//! is the only place that reads `/sys`, spawns `hostapd` or binds a socket, so
//! the safety rules are testable on any host.
//!
//! Hard safety rules encoded here (mirroring the verified `Input/WirelessCarPlay/MFI/auth3_safety.cpp`):
//!
//! * MFi coprocessor access is allowlisted to `/dev/i2c-1` at `0x10`/`0x11`.
//! * Bus 2 is the on-board AC200 (`i2c@5002c00`, `2-0010/name=ac200`) and is
//!   refused unconditionally, whatever the configuration says.
//! * The CarPlay AP is confined to the Wi-Fi interface; the USB RNDIS debug
//!   link (`usb0`, 192.168.77.0/24) must survive every AP operation.

pub mod ap;
pub mod bounds;
pub mod config;
pub mod ipc;
pub mod mfi;
pub mod net;
pub mod status;
pub mod telemetry;

pub use bounds::Ring;
pub use config::{ConfigError, ConfigErrors, RuntimeConfig};
pub use mfi::{MfiDecision, MfiRefusal, MfiTarget, SysfsView};
pub use net::{Finding, FindingSeverity, NetSnapshot};
pub use status::{Check, CheckStatus, SystemSnapshot};

/// Identifier the sidecar reports itself with, and the source id it would
/// register in the C++ core once the media ingest seam exists.
pub const HARNESS_NAME: &str = "cp-native";

/// Source id used for the real wireless CarPlay input. Distinct from the
/// synthetic MVP's `synthetic-wireless` so the core can never confuse the two.
pub const REAL_SOURCE_ID: &str = "wireless-carplay-real";
