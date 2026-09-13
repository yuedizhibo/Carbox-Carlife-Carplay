# `Input/WirelessCarPlay/Engine` — headless native Wireless-CarPlay sidecar

`cp-native` is a small static ARM64 binary that owns the parts of a real wireless
CarPlay head-unit that are **not** protocol: the CarPlay access point, the MFi
bus safety guard, the USB RNDIS debug-link preservation checks, the control
socket, and preflight/status/diagnostics.

It runs as its own process. The C++20 core (`core/`, `wirelesscarplay/`, web UI
on `:8080`) is untouched and unlinked; the two only ever exchange line-JSON over
a Unix socket. See [`LICENSE-NOTICES.md`](LICENSE-NOTICES.md) for why that
separation is also a license requirement.

**This slice is the harness and external-engine boundary, not a claim of a
working CarPlay receiver.** The default `selftest` engine implements no Apple
protocol and does not advertise AirPlay. `CP_ENGINE=catplay` is accepted only
at runtime after an independently-built CatPlay bridge proves a bounded,
versioned capability handshake; the supplied upstream snapshot has not exposed
that bridge API yet and is rejected rather than faked. See
[`FEATURE-MATRIX.md`](FEATURE-MATRIX.md) for exact feature status and hardware
limits.

## Layout

```
Input/WirelessCarPlay/Engine/
├── Cargo.toml                  workspace, size-lean release profile
├── LICENSE-NOTICES.md          LIVI GPL-3.0 derivation + catplay provenance
├── licenses/GPL-3.0.txt        verbatim copy of Reference/LIVI/LICENSE
├── build/build.sh              fmt | clippy | test | check-arm64 | arm64 | all
├── build/build-catplay-c2a.sh  external CatPlay C2A build, no source copy
├── FEATURE-MATRIX.md           evidence-based feature status
├── deploy/
│   ├── README.md               installation and USB profile safety
│   ├── zero2w-cp-native.env    EnvironmentFile template (all CP_* keys)
│   ├── zero2w-cp-native.service
│   └── install.sh              plan-only by default, --apply to install
└── crates/
    ├── cp-harness/             portable policy library (no OS calls)
    │   └── src/{lib,bounds,config,mfi,net,ap,ipc,status,telemetry}.rs
    └── cp-native/              the binary (all OS interaction)
        └── src/{main,cli,system,ap,serve,engine}.rs
```

`cp-harness` never reads a file, spawns a process or opens a socket. It takes
snapshots as data and returns decisions, config text and command plans as data.
`cp-native` is the only place that touches `/sys`, `/proc`, `PATH` and child
processes, which is why the safety rules are testable off-target.

## Build

Host tests and linting (any OS with the Rust toolchain):

```sh
Input/WirelessCarPlay/Engine/build/build.sh test       # cargo test --workspace
Input/WirelessCarPlay/Engine/build/build.sh clippy     # -D warnings
Input/WirelessCarPlay/Engine/build/build.sh fmt
```

ARM64 target binary — static, no libc coupling (the board runs Debian 12 /
glibc 2.36, so a static musl binary cannot drift from it):

```sh
Input/WirelessCarPlay/Engine/build/build.sh arm64
# -> target/aarch64-unknown-linux-musl/release/cp-native
#    ELF 64-bit LSB executable, ARM aarch64, statically linked, stripped
#    628168 bytes (613 KiB) with the size-lean release profile
```

Requirements: `rustup target add aarch64-unknown-linux-musl`, plus a linker
driver that can target musl. Verified on Windows with LLVM `clang` and rustup's
self-contained musl sysroot. Two details the script handles:

* the sysroot path stays inside `RUSTFLAGS`, so no shell translates the
  Windows-style path;
* when a Windows-side `cargo.exe` is driven from WSL, `WSLENV` must list
  `RUSTFLAGS` or the variable never reaches the Windows process — cargo then
  silently falls back to `cc` (mingw) and fails on ELF-only linker options. The
  script sets that automatically when `$CARGO` ends in `.exe`.

`build.sh check-arm64` type-checks for arm64 without any linker, and
`CARGO`/`RUSTC`/`CLANG` can be pointed at a toolchain on the other side of a
WSL/Windows boundary:

```sh
CARGO=/mnt/c/Users/<you>/.cargo/bin/cargo.exe \
RUSTC=/mnt/c/Users/<you>/.cargo/bin/rustc.exe \
  bash build/build.sh arm64
```

Lint the Unix-only code paths (the control-socket loop is `#[cfg(unix)]`, so a
host clippy run never sees it):

```sh
cargo clippy --workspace --all-targets --target aarch64-unknown-linux-musl -- -D warnings
```

There is no `rust-toolchain.toml` pin on purpose: MSRV is declared in
`Cargo.toml` (`rust-version = "1.85"`) and any newer stable works.
`rustfmt.toml` keeps the dense style of this repository's C++ core, matching
`Reference/CatPlaySource/rustfmt.toml`'s width, so `cargo fmt --check` is a gate
rather than a churn generator.

## Deploy

```sh
# on the board, over the USB debug link
ssh root@192.168.77.2
/root/zero2w/Input/WirelessCarPlay/Engine/deploy/install.sh /path/to/cp-native        # prints the plan
/root/zero2w/Input/WirelessCarPlay/Engine/deploy/install.sh /path/to/cp-native --apply
```

`install.sh` installs missing packages (`hostapd iw dnsmasq rfkill avahi-utils
bluez alsa-utils i2c-tools`), the binary to `/root/zero2w/bin/cp-native`, the env
file to `/etc/zero2w/cp-native.env` (never overwriting an existing one) and the
unit. It does **not** enable or start anything and does **not** touch `wlan0`.

Before the first start:

1. set `CP_COUNTRY` in `/etc/zero2w/cp-native.env` — an empty or `00` domain
   cannot host an AP, and choosing a domain you are not entitled to is illegal;
2. `cp-native ap-plan --env-file /etc/zero2w/cp-native.env` — review every file
   and command it would use;
3. `cp-native preflight --env-file ...` — must report `PASS`;
4. `cp-native mfi-check --env-file ...` — must show the bus-1 decision and the
   AC200 refusal;
5. only then `systemctl enable --now zero2w-cp-native.service`.

Starting the service releases `wlan0` from NetworkManager and flushes its client
address, so any SSH session over Wi-Fi dies. SSH over USB (`192.168.77.2`) is
checked before the radio is touched and is preserved: the unit
`Requires=zero2w-usb-rndis.service`, and the binary refuses to start when the
debug link is down.

## Commands

```
cp-native config      effective configuration, passphrase redacted
cp-native preflight   dependencies + MFi safety + network safety   (exit 1 = blocked)
cp-native ap-plan     every file and command the launcher would use (exit 1 = blocked)
cp-native ap-up       bring the AP up in the foreground and supervise it
cp-native mfi-check   MFi bus policy; --auth also runs auth3-native info+selftest
cp-native run         daemon: control socket + engine + AP (unless --no-ap)
cp-native status      running sidecar's status, or a one-off live snapshot
cp-native diag        counters, bounds, recent events, command log
```

Global options: `--json`, `--dry-run`, `--env-file PATH`, `--socket PATH`,
`--follow`, `--auth`, `--no-ap`. Exit codes: `0` ok, `1` blocked or invalid
configuration, `2` usage, `3` runtime.

`--dry-run` is honoured by every mutating command: it validates and prints, and
never writes a file, spawns a process or configures an interface. With
`CP_USB_PROFILE=development`, `cp-native run --no-ap` starts CatPlay only with
`--cp-input-only`; it skips AP/RNDIS/MFi preflight collection and leaves
USB/UDC/configfs, the Wi-Fi radio, and the mDNS publisher untouched while still
supervising the engine telemetry socket. Vehicle uses `--cp-bridge` and retains
the explicit alternate-management, no-RNDIS safety requirements.

## Safety model

### MFi: bus 1 only, AC200 refused unconditionally

`/dev/i2c-1 @ 0x10` is the external MFi Auth 3.0 coprocessor (PI7/PI8,
device-tree `i2c@5002400`). `/dev/i2c-2 @ 0x10` is the on-board **AC200**
(`i2c@5002c00`, sysfs `2-0010/name=ac200`); touching it can break the board's
own wireless/Bluetooth combo chip.

`cp_harness::mfi` mirrors the refusal precedence and message wording of the
verified `Input/WirelessCarPlay/MFI/auth3_safety.cpp`, and adds a compile-time allowlist in front of
it:

1. `ALLOWED_MFI_BUSES = [1]` — bus 2 is refused **before any sysfs or device
   access**, whatever `CP_MFI_BUS` says; `CP_MFI_BUS=2` is also rejected by
   configuration validation, so the mistake surfaces twice;
2. adapter identity `i2c@5002c00` or a name containing `ac200` → refused;
3. an existing `/sys/bus/i2c/devices/1-00XX` node → refused as `ac200-device`,
   `kernel-bound`, `device-tree-node` or `sysfs-present`;
4. otherwise allowed, with a warning when the adapter identity is missing or is
   not the verified `i2c@5002400`.

`mfi-check --auth` reuses the already-verified `/root/MFIAuth/auth3-native`
binary with `--bus 1 --addr 0x10` pinned from the checked target, and runs only
read-only commands (`info`, `selftest`). No I2C transaction is issued by
`cp-native` itself.

### USB RNDIS debug link is preserved

The AP is confined to the Wi-Fi radio and the debug link must survive:

* `CP_AP_IFACE` equal to `CP_RNDIS_IFACE` is rejected at configuration time;
* an AP address inside `192.168.77.0/24` is rejected;
* the Wi-Fi interface holding an address in the debug subnet blocks the launch
  (flushing it would kill SSH);
* `usb0` missing, missing `192.168.77.2`, or an inactive
  `zero2w-usb-rndis.service` / `zero2w-usb-dhcp.service` blocks the launch while
  `CP_REQUIRE_RNDIS=1` (the default);
* `usb0` being unmanaged by NetworkManager is expected — its own oneshot service
  configures the gadget — so it is reported as context, not as a blocker; the
  drop-in this slice generates names the AP interface only, so the debug link is
  never pinned by it;
* hostapd, dnsmasq and `avahi-publish-service` are all bound to the AP interface
  alone (`bind-interfaces`, `--interface`), so they coexist with the existing
  RNDIS dnsmasq;
* AP readiness requires a DHCP listener **on the AP address**, not merely on
  port 67: the RNDIS dnsmasq already owns port 67 from boot, so the reference
  check would report a false "ready".

### Bounded resources

Every cap is a compile-time constant in `cp_harness::bounds` and the
configuration can only lower it:

| Resource | Default | Hard cap |
| --- | --- | --- |
| event ring | 128 | 256 |
| per-client IPC outbox | — | 8 |
| concurrent IPC clients | 2 | 2 |
| IPC request line | 1024 B | 4096 B |
| IPC reply line | — | 16 KiB |
| events in one `diag` reply | — | 32 |
| media frame cache | 4 | 4 |
| report items (checks/findings) | — | 64 |
| command log | — | 64 |
| captured command output | — | 8 KiB |
| addresses per interface | — | 8 |
| stations per report | — | 4 |
| DHCP leases | — | 41 |
| associated stations | — | `CP_AP_MAX_STA` (default 4) |

Worst case resident in the control socket is therefore
`2 clients x 8 queued replies x 16 KiB = 256 KiB`, and that only if a client
stops reading entirely — such a client is dropped instead.

The main loop is single-threaded: one 200 ms tick services the control socket,
the AP supervision and the self-test heartbeat. No executor, no thread pool, no
unbounded queue. Replies larger than the line cap are replaced by an error
reply; a client that stops reading is dropped instead of being buffered.

## Control socket protocol

`CP_CORE_SOCKET` (default `/run/zero2w/cp-native.sock`, mode `0600`). One JSON
object per line, one reply line per request:

```
{"op":"status","id":1}
{"id":1,"ok":true,"payload":{...}}
```

Operations: `config`, `preflight`, `ap-state`, `status`, `telemetry`, `events`
(`"follow":true` streams, `"replay":N` replays ring entries), `diag`,
`shutdown`. `telemetry` is schema-versioned and reports unsupported/not-offered
states rather than synthetic success. Errors: `line-too-long`, `malformed`, `unknown-op`, `reply-too-large`,
`too-many-clients`.

Manual query without any tooling:

```sh
printf '{"op":"status","id":1}\n' | socat - UNIX-CONNECT:/run/zero2w/cp-native.sock
```

## Status

### Implemented and verified by automated tests (off-target)

| Area | Where |
| --- | --- |
| configuration parsing/validation for every `CP_*` key | `cp-harness/src/config.rs` |
| MFi bus allowlist, AC200 refusal, pinned tool argv | `cp-harness/src/mfi.rs` |
| RNDIS preservation, AP-iface policy, `/proc/net/udp` parsing | `cp-harness/src/net.rs` |
| hostapd/dnsmasq/NetworkManager/avahi/ip plan generation, Apple vendor element, channel maths, EUI-64 link-local | `cp-harness/src/ap.rs` |
| IPC framing, bounds, client slots, outbox | `cp-harness/src/ipc.rs` |
| preflight/status/diag model, capability statement | `cp-harness/src/status.rs` |
| CLI surface and `EnvironmentFile` parsing | `cp-native/src/cli.rs` |
| parsers for `ip addr`, `nmcli`, `iw reg`, `if_inet6`, `/proc/self/status`, `/proc/meminfo`, `hostapd_cli all_sta` | `cp-native/src/system.rs` |
| AP plan rendering, command log bounds, best-effort classification | `cp-native/src/ap.rs` |
| engine event ring, heartbeat rate limiting, per-op replies | `cp-native/src/serve.rs` |
| configuration rendering, root gate, dispatch and exit codes | `cp-native/src/main.rs` |
| static ARM64 musl release build | `build/build.sh arm64` |

### Implemented, not yet exercised on the board

These need the parent's hardware run; nothing here claims they work yet:

* `ap-up` / `run`: writing the drop-in, NetworkManager takeover, `iw reg set`,
  address setup, hostapd + dnsmasq + avahi supervision, restart on child exit;
* live snapshot collection on the real board (`iface_states`, `nm_unmanaged`,
  `unit_state`, `regulatory_country`, `udp_listeners`, `collect_sysfs`);
* `mfi-check --auth` invoking `/root/MFIAuth/auth3-native`;
* the control socket under `socat`, and `--follow` streaming;
* the systemd unit, including `ExecStartPre` gating and `MemoryMax`;
* whether an iPhone offers this SSID as a CarPlay car — that additionally needs
  the protocol work listed below, so it is **not** expected to pass yet.

### Verified on this board earlier, outside this slice

* MFi Auth 3.0 on `/dev/i2c-1 @ 0x10`: protocol 3.0, self-test `0xC0`, 608-byte
  certificate, random challenge / 64-byte response (`Input/WirelessCarPlay/MFI/auth3-native`);
* C++20 core + web UI on `:8080`, token auth, synthetic backend;
* USB-C RNDIS debug link: `192.168.77.2` ↔ `192.168.77.14`, SSH and HTTP.

### Pending (not implemented)

* Bluetooth: adapter power-up, CarPlay accessory class/EIR broadcast, BlueZ
  pairing agent, iAP2-over-RFCOMM.
* iAP2 identification and the wireless handoff: MFi auth messages, subscribe,
  `AccessoryWiFiConfigurationInformation` reply to `0x5702`,
  `CarPlayStartSession` reply to `0x4300` carrying the fe80 link-local and port.
* AirPlay/RTSP control session, HAP pair-setup/pair-verify, timesync.
* Media: H.264 screen receive and decode, PCM audio out, microphone uplink,
  touch/HID reports back to the phone.
* The sink contract a real engine must satisfy is `AirPlayReceiverSink`
  (`Reference/CatPlaySource/carplay/catplay_carplay/src/carplay_rx/receiver_api.rs`):
  `open_audio` → PCM, `open_screen` → encoded H.264 + PTS, `open_microphone` →
  capture, `send_hid_report` → touch. The wireless start-up state machine is
  `Reference/CatPlaySource/catplay_carplay_rx_gadget/src/server_wireless.rs`.
* C++ core media ingest: `SessionCore::submit_video/submit_audio` are
  in-process only, so a socket or local-HTTP ingest endpoint has to be added to
  the core before a real engine can hand frames to it.
* catplay integration itself, gated on the license question in
  [`LICENSE-NOTICES.md`](LICENSE-NOTICES.md).

## Known gaps and residual risks

* **Default AP passphrase.** `CP_AP_PASSPHRASE` empty falls back to a built-in
  default. Set a unique one before using the AP anywhere public.
* **Regulatory domain.** `CP_COUNTRY` has no default on purpose; the board
  currently reports `country 00`, so the first `ap-up` will fail until a legal
  domain is set.
* **Radio capability.** Only 5 GHz channels 36/40/44/48 were confirmed
  supported on this board; 149–161 are allowed by the config validator but not
  yet confirmed on the hardware.
* **80 MHz.** `CP_AP_WIDTH=80` is generated but unverified on this chip; 40 MHz
  is the default.
* **No signal handling.** `run` exits on `SIGTERM` by default action; systemd's
  control-group kill mode takes hostapd/dnsmasq/avahi with it. A graceful
  in-process handler would need a signal crate or `libc`, neither of which is
  worth adding for this.
* **`ap-up` and `run` both supervise.** They share one implementation
  (`Launcher::poll`); never run both at once — the second one's preflight will
  see the AP address already bound and refuse.
