# License notices for `Input/WirelessCarPlay/Engine`

## 1. LIVI (GPL-3.0-or-later) — code is derived, license preserved

`Reference/LIVI/LICENSE` is the GNU General Public License, version 3 or later. A verbatim
copy is kept at [`licenses/GPL-3.0.txt`](licenses/GPL-3.0.txt).

Derived from LIVI's native helper (attribution retained in the file headers):

| Our file | Derived from | What was taken |
| --- | --- | --- |
| `crates/cp-harness/src/ap.rs` | `Reference/LIVI/native/livi-helperd/crates/livi-runtime/src/wifi_ap.rs` | hostapd/dnsmasq configuration shape, the Apple vendor element `dd0800a04000000200{band}`, non-DFS 80 MHz centre segments (42/155), HT40 secondary-channel sign, NetworkManager takeover, IPv6 link-local requirement |
| `crates/cp-harness/src/ap.rs` (`airplay_txt_records`, `avahi_*`) | `.../livi-runtime/src/bonjour.rs` | `_airplay._tcp` TXT record set (`deviceid`, `features`, `flags`, `model`, `srcvers`, `protovers`, `pk`, `pi`) and the `_carplay-ctrl._tcp` browse target |
| `crates/cp-native/src/ap.rs`, `crates/cp-native/src/serve.rs` | `.../livi-helperd/src/linux_main.rs`, `.../livi-runtime/src/wifi_ap.rs` | bring-up ordering and the supervise/restart shape |

Consequence: **`cp-harness` and `cp-native` are licensed GPL-3.0-or-later** (see
the `license` field in `Cargo.toml`). Both crates compile into the single
`cp-native` binary, which is therefore a GPL-3.0-or-later work.

### Process separation from the C++20 core

The GPL binary never links to, and is never linked by, the project's C++20 core
(`Core/`, `Input/WirelessCarPlay/`):

* separate process, separate address space, separate systemd unit;
* the only channel is a line-JSON Unix socket at `CP_CORE_SOCKET`
  (`/run/zero2w/cp-native.sock`, mode 0600);
* no shared memory, no plugin loading, no common build target — `CMakeLists.txt`
  does not reference this workspace and this workspace does not reference it.

The C++20 core therefore keeps its own licensing terms unaffected.

## 2. catplay — no license file in the checkout, nothing copied

`Reference/CatPlaySource/` ships a `README.md` and a `Cargo.lock` but **no `LICENSE`**,
so its terms are unknown and it is treated as all-rights-reserved until the owner
confirms them.

* **No catplay source is copied into this tree.**
* **No catplay crate is compiled by this workspace.** `Cargo.toml` has no path
  dependency into `Reference/CatPlaySource`, and there is no feature flag that would
  add one.
* The project owner has authorised using catplay and LIVI for this project.
  Before any binary derived from catplay is deployed or distributed, the parent
  must confirm its license terms; that confirmation is an open item, not a
  decision taken here. The external build helper can be used for local
  evaluation only after that owner confirmation.

The integration remains external: [`catplay-patch/`](catplay-patch/) carries
the owner-authorized **local evaluation only** patch rather than copying CatPlay
source. Any CatPlay-derived artifact remains prohibited from deployment,
distribution, publication, or linking into the C++ process until its license
terms are separately resolved. Its
bounded telemetry seam derives state from `ProdGadget` and the wireless start-up
state machine in
`Reference/CatPlaySource/catplay_carplay_rx_gadget/src/server_wireless.rs`; it does
not make CatPlay a workspace build input or claim protocol success.

Two catplay facts shaped the safety rules in this slice:

* `Reference/CatPlaySource/core/catplay_mfi/src/mfi_device_i2c.rs` opens
  `/dev/i2c-{bus}` with **no guard**, so any future integration must keep the
  bus allowlist implemented in `cp-harness/src/mfi.rs`.
* LIVI's default `LIVI_CP_MFI_I2C_BUS` is **2**, which is the on-board AC200 on
  this board. Our default is **1** and bus 2 is refused unconditionally.

## 3. `Reference/` is read-only

Nothing under `Reference/` was modified by this work. Cargo is never invoked with
`Reference/` as the workspace root, so no `target/` directory is created there.

## 4. Third-party crates actually compiled

From `Cargo.lock`, the complete set is 11 crates:

* linked into the binary (6): `serde`, `serde_core`, `serde_json`, `zmij`,
  `itoa`, `memchr` — all pure Rust, all MIT/Apache-2.0;
* build/derive time only (5): `serde_derive`, `syn`, `proc-macro2`, `quote`,
  `unicode-ident`.

No Electron, Node, JVM, Python, GStreamer, D-Bus, OpenSSL, `libc`-based binding
or any other C dependency is built or resident, which is what makes the static
`aarch64-unknown-linux-musl` build possible. Neither `Reference/CatPlaySource` nor
`Reference/LIVI` is a build input: the LIVI logic was re-expressed in
`cp-harness/src/ap.rs` under the attribution above.
