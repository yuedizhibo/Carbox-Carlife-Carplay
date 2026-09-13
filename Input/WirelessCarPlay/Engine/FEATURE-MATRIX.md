# Real-CarPlay feature matrix

Status labels are deliberately evidence-based:

- **implemented+tested**: host unit tests cover the implementation.
- **implemented-unverified-on-board**: built/planned but no Orange Pi evidence.
- **blocked-by-hardware**: requires an explicit physical connection or profile switch.
- **upstream-limitation**: the supplied upstream does not expose the required seam.
- **pending**: not implemented in this milestone.

| Area | Status | Evidence / boundary |
|---|---|---|
| MFi Auth 3.0 bus-1 policy and AC200 refusal | implemented+tested | `cp-harness::mfi`; only `/dev/i2c-1` is allowed before access. Hardware auth was verified before this milestone, but not rerun here. |
| RNDIS-preserving AP preflight | implemented+tested | `cp-harness::net`; development profile remains `usb0` / `192.168.77.2`. |
| AP config atomic secret handling, rollback plan, bounded restart policy | implemented+tested | `cp-native::ap`; board execution is unverified. |
| PHY channel/VHT validation | implemented+tested | Parses `iw phy` capability data; unavailable data requires conservative operator review. |
| AirPlay mDNS advertising | implemented-unverified-on-board | Suppressed unless a real engine handshake and local Avahi `--interface` support are proven. |
| CatPlay C2A dual-role artifact | pending | `build/build-catplay-c2a.sh` invokes external CatPlay with UI/jemalloc defaults disabled. The patched WSL-native host build compiles the dual-role graph through `catplay_c2a`, but link currently fails on unresolved TurboJPEG `tj3*` symbols. Cross build additionally awaits the Rust ARM64 target and a sysroot that includes every native CatPlay dependency (not only DBus/libusb/OpenSSL). |
| CatPlay lifecycle/capability handshake | implemented+tested | `cp-native::engine` validates a bounded handshake and caps restarts. `catplay-patch/` reports the existing `ProdGadget` wireless/wired support but keeps `listener_ready=false`; it remains rejected until live listener and telemetry-socket evidence exists. |
| Wireless iPhone CarPlay input | pending | Needs the above upstream patch, Bluetooth/iAP2/HAP/AirPlay board integration and iPhone evidence. |
| Wired vehicle CarPlay output | blocked-by-hardware | Vehicle USB profile owns the UDC and is mutually exclusive with RNDIS; requires alternate management/offline deployment acknowledgement and a vehicle head unit. |
| Main video/audio/mic/control forwarding | pending | No media pipeline is claimed until CatPlay exposes and the adapter taps it. |
| Second/instrument screen negotiation | pending | Telemetry schema reports `not-offered`/`not-negotiated`; no success is synthesized. CarPlay Ultra is **not** supported by the CatPlay README. |
| Now playing/artwork/lyrics | pending | Schema is bounded and ready; actual values wait for upstream events. CatPlay README calls now-playing incomplete. |
| GPS/navigation/phone/vehicle state | pending | Schema is bounded and ready; CatPlay README states GPS sync is incomplete. |
| Bounded preview keyframe resync semantics | implemented+tested | `cp-harness::telemetry::MediaFrameCache`; no board decode/re-encode path exists. |
| C++ web telemetry consumer | implemented+tested | `mvp_server` performs a bounded 200 ms Unix-socket query of `telemetry`, embeds only a bounded JSON-object reply, and exposes `realCarPlayTelemetryAvailable`; core tests cover bounded reply validation. It does not decode media or claim a session. |

## Required next board sequence

1. Keep USB RNDIS connected and verify `ssh root@192.168.77.2`.
2. Build/deploy only `cp-native`; run `preflight`, `mfi-check`, and `ap-plan` over USB. Do not start AP yet.
3. Confirm `iw phy` reports the chosen legal country/channel/width and confirm Avahi interface help support.
4. Implement and review the CatPlay upstream patch layer for the capability handshake, telemetry socket, and vehicle USB profile. Build it externally with `build-catplay-c2a.sh`.
5. In development profile, launch the patched engine, prove its listener handshake, then start AP and test iPhone discovery/pairing over USB logs.
6. Capture real iAP2/MFi/HAP/RTSP, main/secondary screen, media/audio/mic/HID telemetry before marking any feature active.
7. Only with alternate management confirmed, switch to vehicle USB profile, attach a real head unit, and test wired output/reconnects. Restore development profile before RNDIS-based debugging.
