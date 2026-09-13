# CatPlay C2A external patch layer

## Input API overlay (2026-09-13)

The build helper now applies three layers: cp-native, input-only, then
`catplay-c2a-input-api.patch`. The third layer connects bounded real iAP2
NowPlaying/artwork/call-state subscriptions to CPMF, adds HID descriptors and
maps session/sequence-bound extended controls to actual receiver commands.
It supports media/telephone/knob keys, two-contact multitouch, Home/Back/select/
directions/wheel, proximity, Siri actions, phone controls, night mode and limited
UI. This supersedes the older descriptions below saying controls are only touch
or that metadata/calls are absent. Lyrics, navigation, complete media-library/
contacts/OTA and the three accessibility/HID-mode setters remain unimplemented.
See `Input/API_AUDIT/INTERFACE_CONTRACT.md` for the complete per-type matrix.
The existing silent input-only recorder is still evaluation-only, not an
original-car microphone return path. No output/USB changes are part of this work.
The local-evaluation/license restrictions below remain unchanged.

`Reference/CatPlaySource` has no visible top-level license file. This directory does
not copy CatPlay into the `cp-native` binary and does not assert a license for
it. It records a minimal owner-authorized **local evaluation only** patch that is
applied only to a separate working checkout. CatPlay remains all-rights-reserved:
no resulting artifact may be deployed, distributed, published, or linked into
C++.

The patch adds `--cp-capabilities-json` and an engine-owned current-thread
Unix telemetry listener to the supplied `catplay_c2a` main binary.
`CP_TELEMETRY_SOCKET` selects its path (default
`/run/zero2w/catplay.sock`). It emits schema-version-1 line-delimited JSON with
`engine="catplay"`, lifecycle, and readiness fields derived only from the real
`ProdGadget`/receiver/transmitter reconciler states. In particular,
`bridge_active` requires transmitter `Transmitting` and receiver `Receiving`;
startup never claims an iPhone, vehicle, media, or second-screen session.

The listener accepts and immediately closes excess clients beyond four, retains
only each client's latest complete snapshot, bounds lines to 4096 bytes, and
evicts partial, broken, or `WouldBlock` writers after at most two retries (or a
two-second deadline). It creates mode `0600` and removes only its own socket inode. Its parent
must already be a non-symlink directory; stale paths are removed only when they
are sockets owned by the current uid. Unit tests cover mapping, JSON bounds,
socket safety/mode, client cap, and deterministic latest-only behavior.

The patch also adds the CPMF v1 producer at `CP_CATPLAY_MEDIA_SOCKET`
(default `/run/zero2w/catplay-media-v1.sock`). It exposes independently tagged
main/AltScreen H.264 Annex-B configuration/frames and phone-to-accessory
playback S16LE PCM. `VIDEO_START.p0` is the bounded screen role (`0=main`,
`1=alt`); the shared producer queue remains capped at two frames. The reverse
channel accepts strict, fixed-size keyframe and main-touch records. Touch input
is associated with one advertised single-contact HID digitizer and forwarded
as encrypted `hidSendReport`; stale sessions, Alt roles, malformed phases, and
out-of-range coordinates are rejected. It does not expose physical microphone,
calls, navigation, or lyrics.
The capability report still advertises *support*, not live readiness. It must
retain the `/dev/i2c-1` MFi allowlist in the external integration layer;
CatPlay's generic I2C implementation is not safe for this board unguarded.

Apply to a disposable CatPlay checkout only:

```sh
# The supplied checkout uses CRLF. Normalize the disposable copy's touched
# Rust sources and manifests before applying this LF patch.
CATPLAY_ROOT=/path/to/catplay
find "$CATPLAY_ROOT" -type f \( -name '*.rs' -o -name 'Cargo.toml' \) -exec sed -i 's/\r$//' {} +
patch -d "$CATPLAY_ROOT" -p1 < catplay-c2a-cp-native.patch
```

Then build externally with `CATPLAY_ROOT="$CATPLAY_ROOT" ../build/build-catplay-c2a.sh`; no CatPlay artifact
may be deployed or distributed until its upstream license is confirmed.

## Local-evaluation wireless input only

`catplay-c2a-input-only.patch` is a second, deterministic overlay applied **after**
`catplay-c2a-cp-native.patch`. It adds the narrow mode used for a board whose
existing `wlan0` AP and USB RNDIS management gadget must stay untouched:

```sh
# The helper refuses to patch Reference/CatPlaySource. CATPLAY_ROOT must be a
# disposable copy. It normalizes the listed source files, applies both layers
# once, and builds the external artifact.
CATPLAY_ROOT=/opt/zero2w-catplay-build \
  ../build/build-catplay-c2a.sh

# Capability proof only; this never opens I2C, gadget, network, or sockets.
./target/aarch64-unknown-linux-gnu/release/catplay_c2a --cp-capabilities-json
# includes: "input_only":true
```

For a local staging evaluation, copy
[`catplay-input-only.example.toml`](catplay-input-only.example.toml) as
`c2a.toml` beside the external binary, create its `./state` directory, set the
AP password out of band, and start **only**:

```sh
CP_TELEMETRY_SOCKET="$PWD/catplay.sock" \
CP_CATPLAY_MEDIA_SOCKET="$PWD/catplay-media-v1.sock" \
  ./catplay_c2a --cp-input-only
```

`--cp-input-only` and `--cp-bridge` are mutually exclusive. The input-only
branch validates configuration before every startup side effect and accepts
only `[mfi.i2c] bus_offset=1, dev_addr=16` (`/dev/i2c-1@0x10`). It rejects a
missing backend, remote/client backend, server stanza (even disabled), bus 2,
or any other address before `GadgetHelper`, MFi device lookup, I2C path
construction, configfs/UDC, output manager, TX gadget, or `TxAdapter` can run.
The same guard protects normal bridge mode. For this board's MFi transport, the
overlay binds the validated address with `I2C_SLAVE` and uses separate register
selector writes and data reads; the controller rejects CatPlay's former combined
`I2C_RDWR` repeated-start transaction. It also wakes Auth 3.0 and verifies device
version `0x07` before uncached certificate reads and each challenge. It polls
register `0x10` until the signing-complete state (with a bounded two-second
deadline and hardware error check) before reading response length/data from
`0x11`/`0x12`. Missing wake-up and a fixed 10 ms signing delay produced transient
zero-length responses and aborted the Bluetooth iAP2 session.

Input-only starts MFi (including the configured self-test), HomeKit state, the
bounded telemetry/CPMF servers, and a standalone `CarPlayWirelessGadget` on
the configured Bluetooth HCI and existing AP interface with invitations
unblocked. It always reports one primary display and can optionally report one
AltScreen display. The strict configuration file selected by
`CATPLAY_DISPLAY_CONFIG` (default `/tmp/zero2w-carplay-display.conf`) controls
only the next session's offered width, height, and fps; fixed UUIDs preserve
both display identities. An offer is never reported as negotiated or active
until matching RTSP/CPMF media exists. The receiver supports independent screen
stream types 110 and 111 and independent teardown. The main display advertises
one high-fidelity touchscreen HID device; AltScreen remains display-only. For
optional duplex speech-recognition setup it creates only a bounded 10 ms silent
evaluation recorder—not a physical microphone. It does **not** create a vehicle
path, lyrics, navigation, UDC/configfs, or wireless AP configuration. The
current-thread runtime services CPMF at a fixed cadence; its preview PCM worker
is a Tokio task, not an OS thread, and uses a fixed 960-sample maximum buffer
plus CPMF's bounded queues.

Input-only telemetry derives only from the wireless reconciler: listener-ready
is true only in `Inviting`, `Receiving`, or `Passive`; iPhone-connected is true
only in `Receiving`; `wired_endpoint_ready`, `vehicle_connected`, and
`bridge_active` are always false. Lifecycle names are input-only names and
never refer to UDC, gadget, or TX state.
