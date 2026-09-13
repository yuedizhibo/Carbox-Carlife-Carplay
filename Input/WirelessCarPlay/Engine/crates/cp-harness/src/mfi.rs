//! MFi coprocessor access policy.
//!
//! This is the Rust counterpart of the verified `Input/WirelessCarPlay/MFI/auth3_safety.cpp` guard and
//! keeps its exact refusal precedence and message wording, so a board that
//! passes `auth3-native` also passes `cp-native mfi-check`.
//!
//! Board facts (see `Input/WirelessCarPlay/MFI/README.md`):
//!
//! * `/dev/i2c-1` @ `0x10` — external MFi Auth 3.0 coprocessor on PI7/PI8,
//!   device-tree node `i2c@5002400`. Verified: protocol 3.0, self-test 0xC0,
//!   608-byte certificate, random challenge / 64-byte response.
//! * `/dev/i2c-2` @ `0x10` — on-board AC200 (`i2c@5002c00`, `2-0010/name=ac200`).
//!   Touching it can break the board's own wireless/Bluetooth combo chip.
//!
//! There is no override: the allowlist is a compile-time constant and bus 2 is
//! refused before any filesystem or device access happens.

use std::collections::BTreeMap;

use serde::Serialize;

/// The only I2C adapter the sidecar may ever open.
pub const ALLOWED_MFI_BUSES: [u32; 1] = [1];
/// 7-bit addresses the coprocessor is known to answer at.
pub const ALLOWED_MFI_ADDRS: [u8; 2] = [0x10, 0x11];
/// Adapter number of the on-board AC200; refused unconditionally.
pub const AC200_BUS: u32 = 2;
/// Device-tree marker of the on-board AC200 controller.
pub const AC200_ADAPTER_NODE: &str = "5002c00";
/// Device name the AC200 reports in sysfs.
pub const AC200_DEVICE_NAME: &str = "ac200";
/// Device-tree marker of the external PI7/PI8 controller the MFi chip sits on.
pub const EXTERNAL_ADAPTER_NODE: &str = "5002400";

/// Read-only view of the sysfs paths the policy inspects. Implemented over the
/// real filesystem by `cp-native` and over a map by the tests.
pub trait Sysfs {
    fn exists(&self, path: &str) -> bool;
    fn read_text(&self, path: &str) -> Option<String>;
}

/// In-memory [`Sysfs`] used by tests and by `--json` dry runs.
///
/// Paths are matched exactly, like the `FakeSysfs` in `Input/WirelessCarPlay/MFI/auth3_tests.cpp`: a
/// directory such as `/sys/bus/i2c/devices/1-0010` counts as present only when
/// it is inserted explicitly, even if files below it are.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
pub struct SysfsView {
    texts: BTreeMap<String, String>,
}

impl SysfsView {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn insert(&mut self, path: impl Into<String>, text: impl Into<String>) {
        self.texts.insert(path.into(), text.into());
    }

    pub fn len(&self) -> usize {
        self.texts.len()
    }

    pub fn is_empty(&self) -> bool {
        self.texts.is_empty()
    }
}

impl Sysfs for SysfsView {
    fn exists(&self, path: &str) -> bool {
        self.texts.contains_key(path)
    }

    fn read_text(&self, path: &str) -> Option<String> {
        self.texts.get(path).cloned()
    }
}

/// Sysfs paths the policy reads, so `preflight` can report them without
/// duplicating the construction rules.
pub struct MfiPaths {
    pub adapter_node: String,
    pub target_base: String,
    pub target_name: String,
    pub target_driver: String,
    pub target_of_node: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum MfiRefusal {
    /// Adapter is not on the compile-time allowlist.
    BusNotAllowed,
    /// Adapter is the AC200 controller itself.
    Ac200Adapter,
    /// A device at this address names itself AC200.
    Ac200Device,
    /// A kernel driver already owns the address.
    KernelBound,
    /// The address belongs to a device-tree node.
    DeviceTreeNode,
    /// The address is present in sysfs for another reason.
    SysfsPresent,
}

impl MfiRefusal {
    pub fn as_str(self) -> &'static str {
        match self {
            MfiRefusal::BusNotAllowed => "bus-not-allowed",
            MfiRefusal::Ac200Adapter => "ac200-adapter",
            MfiRefusal::Ac200Device => "ac200-device",
            MfiRefusal::KernelBound => "kernel-bound",
            MfiRefusal::DeviceTreeNode => "device-tree-node",
            MfiRefusal::SysfsPresent => "sysfs-present",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct MfiTarget {
    pub bus: u32,
    pub addr: u8,
}

impl MfiTarget {
    pub fn dev_path(&self) -> String {
        format!("/dev/i2c-{}", self.bus)
    }

    pub fn sysfs_paths(&self) -> MfiPaths {
        // The kernel names i2c clients `<adapter-decimal>-<addr-hex>`, matching
        // the layout Input/WirelessCarPlay/MFI/auth3_safety.cpp already relies on.
        let base = format!("/sys/bus/i2c/devices/{}-{:04x}", self.bus, self.addr);
        MfiPaths {
            adapter_node: format!("/sys/class/i2c-dev/i2c-{}/device/of_node/name", self.bus),
            target_name: format!("{base}/name"),
            target_driver: format!("{base}/driver"),
            target_of_node: format!("{base}/of_node"),
            target_base: base,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct MfiDecision {
    pub allowed: bool,
    pub target: MfiTarget,
    pub refusal: Option<MfiRefusal>,
    pub message: String,
    pub adapter_identity: String,
    /// Set when the adapter answered but is not the verified external one.
    pub identity_warning: Option<String>,
}

impl MfiDecision {
    /// Argument vector for the verified native utility. The bus and address are
    /// pinned from the checked target so no caller can widen them.
    pub fn tool_argv(&self, tool: &str, command: &str, json: bool) -> Vec<String> {
        let mut argv = Vec::with_capacity(8);
        argv.push(tool.to_string());
        argv.push("--bus".to_string());
        argv.push(self.target.bus.to_string());
        argv.push("--addr".to_string());
        argv.push(format!("0x{:02X}", self.target.addr));
        if json {
            argv.push("--json".to_string());
        }
        argv.push(command.to_string());
        argv
    }
}

/// Decide whether `target` may be touched at all. No I/O beyond the sysfs reads
/// happens here, and the bus allowlist is checked first so an operator cannot
/// reach the AC200 by editing the environment.
pub fn check_mfi_target(sysfs: &dyn Sysfs, target: MfiTarget) -> MfiDecision {
    let paths = target.sysfs_paths();
    let adapter_identity = sysfs.read_text(&paths.adapter_node).map(|t| t.trim().to_string()).unwrap_or_default();

    if !ALLOWED_MFI_BUSES.contains(&target.bus) {
        let message = if target.bus == AC200_BUS {
            format!("refusing access: /dev/i2c-{AC200_BUS} is the on-board AC200 ({AC200_ADAPTER_NODE}) and is refused unconditionally")
        } else {
            format!(
                "refusing access: /dev/i2c-{} is not allowlisted; the external MFi adapter is /dev/i2c-{}",
                target.bus, ALLOWED_MFI_BUSES[0]
            )
        };
        return MfiDecision {
            allowed: false,
            target,
            refusal: Some(MfiRefusal::BusNotAllowed),
            message,
            adapter_identity,
            identity_warning: None,
        };
    }

    let lower = adapter_identity.to_ascii_lowercase();
    if lower.contains(AC200_DEVICE_NAME) || lower.contains(AC200_ADAPTER_NODE) {
        return MfiDecision {
            allowed: false,
            target,
            refusal: Some(MfiRefusal::Ac200Adapter),
            message: "refusing access: selected adapter/address is AC200 (internal bus is unsafe)".to_string(),
            adapter_identity,
            identity_warning: None,
        };
    }

    if sysfs.exists(&paths.target_base) {
        let name = sysfs.read_text(&paths.target_name).unwrap_or_default();
        let (refusal, message) = if contains_case_insensitive(&name, AC200_DEVICE_NAME) {
            (MfiRefusal::Ac200Device, "refusing access: selected adapter/address is AC200 (internal bus is unsafe)".to_string())
        } else if sysfs.exists(&paths.target_driver) {
            (MfiRefusal::KernelBound, "refusing access: selected I2C address is bound to a kernel driver".to_string())
        } else if sysfs.exists(&paths.target_of_node) {
            (MfiRefusal::DeviceTreeNode, "refusing access: selected I2C address belongs to a device-tree node".to_string())
        } else {
            (MfiRefusal::SysfsPresent, "refusing access: selected I2C address already exists in sysfs".to_string())
        };
        return MfiDecision { allowed: false, target, refusal: Some(refusal), message, adapter_identity, identity_warning: None };
    }

    let identity_warning = if adapter_identity.is_empty() {
        Some(format!(
            "adapter identity unknown: {} is missing; confirm the mapping with `i2cdetect -l` before authenticating",
            paths.adapter_node
        ))
    } else if !lower.contains(EXTERNAL_ADAPTER_NODE) {
        Some(format!(
            "adapter identity '{}' does not contain the verified external controller marker '{EXTERNAL_ADAPTER_NODE}'",
            adapter_identity
        ))
    } else {
        None
    };

    MfiDecision {
        allowed: true,
        target,
        refusal: None,
        message: format!("allowed: {} @ 0x{:02X} is not claimed by the kernel", target.dev_path(), target.addr),
        adapter_identity,
        identity_warning,
    }
}

fn contains_case_insensitive(haystack: &str, needle: &str) -> bool {
    if needle.is_empty() {
        return false;
    }
    let hay = haystack.as_bytes();
    let ndl = needle.as_bytes();
    hay.windows(ndl.len()).any(|w| w.eq_ignore_ascii_case(ndl))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn external_board() -> SysfsView {
        let mut sysfs = SysfsView::new();
        sysfs.insert("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@5002400\n");
        sysfs.insert("/sys/class/i2c-dev/i2c-2/device/of_node/name", "i2c@5002c00\n");
        sysfs.insert("/sys/bus/i2c/devices/2-0010", "");
        sysfs.insert("/sys/bus/i2c/devices/2-0010/name", "ac200\n");
        sysfs
    }

    /// A board where something already sits at bus 1 / 0x10.
    fn claimed_board(name: &str, driver: bool, of_node: bool) -> SysfsView {
        let mut sysfs = SysfsView::new();
        sysfs.insert("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@5002400\n");
        sysfs.insert("/sys/bus/i2c/devices/1-0010", "");
        sysfs.insert("/sys/bus/i2c/devices/1-0010/name", name);
        if driver {
            sysfs.insert("/sys/bus/i2c/devices/1-0010/driver", "");
        }
        if of_node {
            sysfs.insert("/sys/bus/i2c/devices/1-0010/of_node", "");
        }
        sysfs
    }

    #[test]
    fn allowed_external_adapter() {
        let decision = check_mfi_target(&external_board(), MfiTarget { bus: 1, addr: 0x10 });
        assert!(decision.allowed, "{decision:?}");
        assert_eq!(decision.refusal, None);
        assert!(decision.adapter_identity.contains(EXTERNAL_ADAPTER_NODE));
        assert_eq!(decision.identity_warning, None);
        assert_eq!(decision.target.dev_path(), "/dev/i2c-1");
    }

    #[test]
    fn bus_two_is_refused_before_any_target_lookup() {
        // Even with a clean-looking sysfs (no 2-0010 node) bus 2 must be refused.
        let decision = check_mfi_target(&SysfsView::new(), MfiTarget { bus: 2, addr: 0x10 });
        assert!(!decision.allowed);
        assert_eq!(decision.refusal, Some(MfiRefusal::BusNotAllowed));
        assert!(decision.message.contains("AC200"), "{decision:?}");
        assert!(decision.message.contains("unconditionally"), "{decision:?}");
    }

    #[test]
    fn ac200_adapter_identity_is_refused() {
        let mut sysfs = SysfsView::new();
        sysfs.insert("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@5002c00\n");
        let decision = check_mfi_target(&sysfs, MfiTarget { bus: 1, addr: 0x10 });
        assert_eq!(decision.refusal, Some(MfiRefusal::Ac200Adapter));
        assert!(decision.message.contains("AC200"), "{decision:?}");
    }

    #[test]
    fn ac200_device_name_is_refused() {
        let decision = check_mfi_target(&claimed_board("AC200\n", false, false), MfiTarget { bus: 1, addr: 0x10 });
        assert_eq!(decision.refusal, Some(MfiRefusal::Ac200Device));
        assert!(decision.message.contains("AC200"), "{decision:?}");
    }

    #[test]
    fn kernel_bound_address_is_refused() {
        let decision = check_mfi_target(&claimed_board("something-else\n", true, false), MfiTarget { bus: 1, addr: 0x10 });
        assert_eq!(decision.refusal, Some(MfiRefusal::KernelBound));
        assert!(decision.message.contains("kernel driver"), "{decision:?}");
    }

    #[test]
    fn device_tree_address_is_refused() {
        let decision = check_mfi_target(&claimed_board("mfi\n", false, true), MfiTarget { bus: 1, addr: 0x10 });
        assert_eq!(decision.refusal, Some(MfiRefusal::DeviceTreeNode));
    }

    #[test]
    fn ac200_name_wins_over_driver_and_of_node() {
        // Same precedence as Input/WirelessCarPlay/MFI/auth3_safety.cpp: the AC200 identity is reported
        // even when the node is also driver-bound.
        let decision = check_mfi_target(&claimed_board("ac200\n", true, true), MfiTarget { bus: 1, addr: 0x10 });
        assert_eq!(decision.refusal, Some(MfiRefusal::Ac200Device));
    }

    #[test]
    fn unexplained_sysfs_presence_is_refused() {
        let mut sysfs = SysfsView::new();
        sysfs.insert("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@5002400\n");
        sysfs.insert("/sys/bus/i2c/devices/1-0011", "");
        sysfs.insert("/sys/bus/i2c/devices/1-0011/name", "mystery\n");
        let decision = check_mfi_target(&sysfs, MfiTarget { bus: 1, addr: 0x11 });
        assert_eq!(decision.refusal, Some(MfiRefusal::SysfsPresent));
    }

    #[test]
    fn a_clean_target_directory_still_blocks() {
        // Only the directory exists, no name/driver/of_node: the address is taken.
        let mut sysfs = SysfsView::new();
        sysfs.insert("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@5002400\n");
        sysfs.insert("/sys/bus/i2c/devices/1-0010", "");
        let decision = check_mfi_target(&sysfs, MfiTarget { bus: 1, addr: 0x10 });
        assert_eq!(decision.refusal, Some(MfiRefusal::SysfsPresent));
        assert!(!decision.allowed);
    }

    #[test]
    fn missing_adapter_identity_warns_but_allows() {
        let decision = check_mfi_target(&SysfsView::new(), MfiTarget { bus: 1, addr: 0x10 });
        assert!(decision.allowed);
        let warning = decision.identity_warning.expect("warning");
        assert!(warning.contains("i2cdetect -l"), "{warning}");
    }

    #[test]
    fn unexpected_adapter_identity_warns() {
        let mut sysfs = SysfsView::new();
        sysfs.insert("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@deadbeef\n");
        let decision = check_mfi_target(&sysfs, MfiTarget { bus: 1, addr: 0x10 });
        assert!(decision.allowed);
        assert!(decision.identity_warning.expect("warning").contains(EXTERNAL_ADAPTER_NODE));
    }

    #[test]
    fn unallowlisted_bus_is_refused() {
        let decision = check_mfi_target(&SysfsView::new(), MfiTarget { bus: 7, addr: 0x10 });
        assert_eq!(decision.refusal, Some(MfiRefusal::BusNotAllowed));
        assert!(decision.message.contains("/dev/i2c-1"), "{decision:?}");
    }

    #[test]
    fn tool_argv_pins_bus_and_address() {
        let decision = check_mfi_target(&external_board(), MfiTarget { bus: 1, addr: 0x10 });
        let argv = decision.tool_argv("/root/MFIAuth/auth3-native", "info", true);
        assert_eq!(
            argv,
            vec![
                "/root/MFIAuth/auth3-native".to_string(),
                "--bus".to_string(),
                "1".to_string(),
                "--addr".to_string(),
                "0x10".to_string(),
                "--json".to_string(),
                "info".to_string(),
            ]
        );
        assert!(!argv.iter().any(|a| a == "2"), "{argv:?}");
    }

    #[test]
    fn sysfs_paths_follow_the_verified_layout() {
        let paths = MfiTarget { bus: 2, addr: 0x10 }.sysfs_paths();
        assert_eq!(paths.target_base, "/sys/bus/i2c/devices/2-0010");
        assert_eq!(paths.target_name, "/sys/bus/i2c/devices/2-0010/name");
        assert_eq!(paths.adapter_node, "/sys/class/i2c-dev/i2c-2/device/of_node/name");
    }
}
