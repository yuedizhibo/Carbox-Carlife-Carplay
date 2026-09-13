//! Command line surface of `cp-native`.
//!
//! Parsing is hand-rolled on purpose: the sidecar must not pull an argument
//! parser crate into a binary that runs next to a 1 GB budget, and the surface
//! is small enough to test exhaustively.

use std::collections::BTreeMap;

/// Cap on entries read from one `--env-file`.
pub const ENV_FILE_ENTRY_CAP: usize = 64;
/// Cap on one `--env-file` line.
pub const ENV_FILE_LINE_CAP: usize = 512;

pub const USAGE: &str = "\
cp-native - headless Zero2W Wireless-CarPlay sidecar

USAGE:
    cp-native <command> [options]

COMMANDS:
    config        print the effective configuration (passphrase redacted)
    preflight     dependency, MFi-safety and network-safety checks
    ap-plan       print every file and command the AP launcher would use
    ap-up         bring the CarPlay AP up on the Wi-Fi radio and supervise it
    mfi-check     evaluate the MFi bus policy (--auth also runs the verified tool)
    run           serve the control socket, run the engine, and the AP unless --no-ap
    status        report a running sidecar's state, or take a live one-off snapshot
    diag          counters, bounds and the recent event ring
    help          this text

OPTIONS:
    --json            machine-readable output
    --dry-run         plan and validate only; never write, spawn or configure
    --env-file PATH   merge KEY=VALUE lines over the process environment
    --socket PATH     control socket to query (default CP_CORE_SOCKET)
    --follow          keep the connection open and stream events
    --auth            with mfi-check: run auth3-native info and selftest (read-only)
    --no-ap           with run: do not touch the Wi-Fi radio

SAFETY:
    MFi access is allowlisted to /dev/i2c-1; bus 2 (on-board AC200) is refused.
    The AP is confined to the Wi-Fi interface; the USB RNDIS debug link is
    checked before and preserved during every operation.

EXIT CODES:
    0 ok    1 blocked or invalid configuration    2 usage error    3 runtime error
";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Command {
    Config,
    Preflight,
    ApPlan,
    ApUp,
    MfiCheck,
    Run,
    Status,
    Diag,
    Help,
}

impl Command {
    pub fn parse(text: &str) -> Option<Self> {
        Some(match text {
            "config" => Command::Config,
            "preflight" => Command::Preflight,
            "ap-plan" => Command::ApPlan,
            "ap-up" => Command::ApUp,
            "mfi-check" => Command::MfiCheck,
            "run" => Command::Run,
            "status" => Command::Status,
            "diag" => Command::Diag,
            "help" | "--help" | "-h" => Command::Help,
            _ => return None,
        })
    }

    /// Commands that only read state and may therefore run without root.
    ///
    /// `mfi-check` reads sysfs only; its `--auth` mode runs the native MFi tool,
    /// which opens `/dev/i2c-1` and is gated separately in `cmd_mfi_check`.
    pub fn read_only(self) -> bool {
        matches!(
            self,
            Command::Config | Command::Preflight | Command::ApPlan | Command::MfiCheck | Command::Status | Command::Diag | Command::Help
        )
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Cli {
    pub command: Command,
    pub json: bool,
    pub dry_run: bool,
    pub follow: bool,
    pub with_auth: bool,
    pub no_ap: bool,
    pub env_file: Option<String>,
    pub socket: Option<String>,
}

impl Default for Cli {
    fn default() -> Self {
        Self {
            command: Command::Help,
            json: false,
            dry_run: false,
            follow: false,
            with_auth: false,
            no_ap: false,
            env_file: None,
            socket: None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UsageError {
    pub message: String,
}

impl std::fmt::Display for UsageError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.message)
    }
}

pub fn parse(args: &[String]) -> Result<Cli, UsageError> {
    let mut cli = Cli::default();
    let mut command: Option<Command> = None;
    let mut index = 0;
    let err = |message: String| Err(UsageError { message });

    while index < args.len() {
        let arg = args[index].as_str();
        index += 1;
        match arg {
            "--json" => cli.json = true,
            "--dry-run" => cli.dry_run = true,
            "--follow" => cli.follow = true,
            "--auth" => cli.with_auth = true,
            "--no-ap" => cli.no_ap = true,
            "--env-file" => {
                let Some(value) = args.get(index) else { return err("--env-file needs a path".to_string()) };
                index += 1;
                if value.is_empty() {
                    return err("--env-file path must not be empty".to_string());
                }
                cli.env_file = Some(value.clone());
            }
            "--socket" => {
                let Some(value) = args.get(index) else { return err("--socket needs a path".to_string()) };
                index += 1;
                if !value.starts_with('/') || value.len() > 108 {
                    return err(format!("--socket '{value}' must be an absolute path of at most 108 bytes"));
                }
                cli.socket = Some(value.clone());
            }
            "--help" | "-h" => command = Some(Command::Help),
            other if other.starts_with('-') => return err(format!("unknown option '{other}'")),
            other => {
                if command.is_some() {
                    return err(format!("unexpected argument '{other}': only one command is allowed"));
                }
                let Some(parsed) = Command::parse(other) else { return err(format!("unknown command '{other}'")) };
                command = Some(parsed);
            }
        }
    }

    cli.command = command.unwrap_or(Command::Help);
    if cli.dry_run && matches!(cli.command, Command::Status | Command::Diag | Command::Config) {
        return err(format!("--dry-run does not apply to '{}'; those commands never mutate", cli.command_name()));
    }
    if cli.with_auth && cli.command != Command::MfiCheck {
        return err("--auth only applies to mfi-check".to_string());
    }
    if cli.no_ap && cli.command != Command::Run {
        return err("--no-ap only applies to run".to_string());
    }
    Ok(cli)
}

impl Cli {
    pub fn command_name(&self) -> &'static str {
        match self.command {
            Command::Config => "config",
            Command::Preflight => "preflight",
            Command::ApPlan => "ap-plan",
            Command::ApUp => "ap-up",
            Command::MfiCheck => "mfi-check",
            Command::Run => "run",
            Command::Status => "status",
            Command::Diag => "diag",
            Command::Help => "help",
        }
    }
}

/// Merge the process environment with an optional `KEY=VALUE` file, file wins.
/// The same file format systemd uses for `EnvironmentFile=`, so one file drives
/// both the unit and a manual dry run.
pub fn collect_env(file: Option<&str>) -> Result<BTreeMap<String, String>, UsageError> {
    let mut env: BTreeMap<String, String> = std::env::vars().collect();
    if let Some(path) = file {
        let text = std::fs::read_to_string(path).map_err(|e| UsageError { message: format!("cannot read --env-file '{path}': {e}") })?;
        for (key, value) in parse_env_file(&text) {
            env.insert(key, value);
        }
    }
    Ok(env)
}

/// Parse `EnvironmentFile` content: comments and blanks ignored, one optional
/// `export ` prefix, optional surrounding quotes, bounded entry count.
pub fn parse_env_file(text: &str) -> Vec<(String, String)> {
    let mut out = Vec::new();
    for line in text.lines() {
        if out.len() >= ENV_FILE_ENTRY_CAP {
            break;
        }
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') || line.len() > ENV_FILE_LINE_CAP {
            continue;
        }
        let body = line.strip_prefix("export ").unwrap_or(line).trim();
        let Some((key, value)) = body.split_once('=') else { continue };
        let key = key.trim();
        if key.is_empty() || !key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
            continue;
        }
        let value = value.trim();
        let value = value.strip_prefix('"').and_then(|v| v.strip_suffix('"')).unwrap_or(value);
        out.push((key.to_string(), value.to_string()));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn no_arguments_means_help() {
        let cli = parse(&args(&[])).expect("ok");
        assert_eq!(cli.command, Command::Help);
        assert!(!cli.json);
    }

    #[test]
    fn every_command_parses() {
        for (text, expected) in [
            ("config", Command::Config),
            ("preflight", Command::Preflight),
            ("ap-plan", Command::ApPlan),
            ("ap-up", Command::ApUp),
            ("mfi-check", Command::MfiCheck),
            ("run", Command::Run),
            ("status", Command::Status),
            ("diag", Command::Diag),
            ("help", Command::Help),
        ] {
            assert_eq!(parse(&args(&[text])).expect(text).command, expected, "{text}");
        }
    }

    #[test]
    fn flags_are_collected() {
        let cli = parse(&args(&["ap-up", "--json", "--dry-run"])).expect("ok");
        assert_eq!(cli.command, Command::ApUp);
        assert!(cli.json);
        assert!(cli.dry_run);
    }

    #[test]
    fn options_with_values_are_collected() {
        let cli = parse(&args(&["status", "--env-file", "/etc/zero2w/cp.env", "--socket", "/run/zero2w/cp-native.sock"])).expect("ok");
        assert_eq!(cli.env_file.as_deref(), Some("/etc/zero2w/cp.env"));
        assert_eq!(cli.socket.as_deref(), Some("/run/zero2w/cp-native.sock"));
    }

    #[test]
    fn missing_option_value_is_a_usage_error() {
        for flag in ["--env-file", "--socket"] {
            let error = parse(&args(&["status", flag])).unwrap_err();
            assert!(error.message.contains(flag), "{error}");
        }
    }

    #[test]
    fn unknown_command_and_option_are_rejected() {
        assert!(parse(&args(&["reboot"])).unwrap_err().message.contains("unknown command"));
        assert!(parse(&args(&["status", "--verbose"])).unwrap_err().message.contains("unknown option"));
    }

    #[test]
    fn only_one_command_is_allowed() {
        let error = parse(&args(&["status", "diag"])).unwrap_err();
        assert!(error.message.contains("only one command"), "{error}");
    }

    #[test]
    fn socket_must_be_absolute_and_short() {
        assert!(parse(&args(&["status", "--socket", "relative.sock"])).is_err());
        let long = format!("/run/{}.sock", "x".repeat(120));
        assert!(parse(&args(&["status", "--socket", &long])).is_err());
    }

    #[test]
    fn flags_are_bound_to_their_command() {
        assert!(parse(&args(&["status", "--auth"])).unwrap_err().message.contains("mfi-check"));
        assert!(parse(&args(&["status", "--no-ap"])).unwrap_err().message.contains("run"));
        assert!(parse(&args(&["config", "--dry-run"])).unwrap_err().message.contains("never mutate"));
    }

    #[test]
    fn read_only_commands_are_identified() {
        assert!(Command::Preflight.read_only());
        assert!(Command::ApPlan.read_only());
        assert!(Command::MfiCheck.read_only(), "sysfs reads need no root; --auth is gated separately");
        assert!(!Command::ApUp.read_only());
        assert!(!Command::Run.read_only());
    }

    #[test]
    fn env_file_parsing_handles_systemd_syntax() {
        let text = "\
# comment
CP_COUNTRY=US

  CP_AP_SSID = ZERO2W-Test
export CP_AP_CHANNEL=44
CP_AP_PASSPHRASE=\"quoted pass\"
not-an-assignment
=novalue
BAD KEY=x
";
        let parsed: BTreeMap<String, String> = parse_env_file(text).into_iter().collect();
        assert_eq!(parsed.get("CP_COUNTRY").map(String::as_str), Some("US"));
        assert_eq!(parsed.get("CP_AP_SSID").map(String::as_str), Some("ZERO2W-Test"));
        assert_eq!(parsed.get("CP_AP_CHANNEL").map(String::as_str), Some("44"));
        assert_eq!(parsed.get("CP_AP_PASSPHRASE").map(String::as_str), Some("quoted pass"));
        assert_eq!(parsed.len(), 4, "{parsed:?}");
    }

    #[test]
    fn env_file_is_bounded() {
        let text = (0..(ENV_FILE_ENTRY_CAP + 20)).map(|i| format!("CP_KEY_{i}=v{i}")).collect::<Vec<_>>().join("\n");
        assert_eq!(parse_env_file(&text).len(), ENV_FILE_ENTRY_CAP);
    }

    #[test]
    fn overlong_env_file_lines_are_skipped() {
        let text = format!("CP_BIG={}\nCP_OK=1\n", "x".repeat(ENV_FILE_LINE_CAP));
        let parsed: BTreeMap<String, String> = parse_env_file(&text).into_iter().collect();
        assert!(!parsed.contains_key("CP_BIG"));
        assert_eq!(parsed.get("CP_OK").map(String::as_str), Some("1"));
    }

    #[test]
    fn usage_documents_the_safety_rules() {
        assert!(USAGE.contains("/dev/i2c-1"), "{USAGE}");
        assert!(USAGE.contains("AC200"), "{USAGE}");
        assert!(USAGE.contains("RNDIS"), "{USAGE}");
        assert!(USAGE.contains("--dry-run"), "{USAGE}");
    }
}
