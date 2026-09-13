# CarPlay Main/Alt Screen Design and Implementation Notes

This document records the protocol, implementation, validation, and debugging requirements for the CarPlay main display and the real AltScreen/instrument-cluster display.

## 1. What AltScreen is

AltScreen is a separate CarPlay video stream, normally used for instrument-cluster/navigation content. It is **not** a clone of the main display and is not a generic extended desktop.

Consequences:

- Main and AltScreen require separate RTSP `SETUP`s and separate media sockets.
- AltScreen does not automatically render the CarPlay home screen.
- iPhone may keep AltScreen blank until the accessory explicitly requests supported cluster UI, normally the Maps instrument-cluster URL.
- Main-screen frames must never be copied into AltScreen and reported as a real second CarPlay stream.

## 2. Truth model

Display configuration is only an accessory offer. It is not proof that iPhone accepted or started the stream.

The UI and API must distinguish:

- `configured`: parameters persisted for the next `/info` exchange.
- `offered`: the display was included in the current AirPlay `/info` response.
- `not-offered`: no AltScreen display was published in that session.
- `not-negotiated`: published, but no matching RTSP `SETUP` was received.
- `waiting-config`: a matching RTSP/CPMF stream exists but no valid SPS/PPS is available.
- `waiting-keyframe`: valid SPS/PPS exists, but no IDR has arrived.
- `active`: the matching stream has valid SPS/PPS and an IDR/frame flow.
- `inactive`: a previously known stream is not currently running.
- `unsupported`: the phone, receiver, decoder, or browser rejected the stream.

Never report AltScreen as active merely because `alt_enabled=1`, because a second UUID exists, or because the phone advertises the `altScreen` feature.

## 3. Configuration contract

A bounded text file is shared between the C++ web process and CatPlay. C++ writes it atomically. CatPlay loads it before constructing the session display offer. Existing sessions are not mutated in place.

```text
version=1
main_width=1920
main_height=1080
main_fps=60
alt_enabled=0
alt_width=1280
alt_height=720
alt_fps=30
```

Constraints:

- dimensions must be even;
- width: `320..2560`;
- height: `240..1440`;
- frame rate: `1..60`;
- total configured pixel rate must not exceed `250,000,000 pixels/s`;
- main UUID is fixed: `5E2D6D6C-8E32-4E2A-9A74-000000001080`;
- AltScreen UUID is fixed: `5E2D6D6C-8E32-4E2A-9A74-000000001081`.

Fixed UUIDs preserve iPhone display identity while resolution and frame-rate offers change.

A successful web configuration update returns `applies="next-session"`. Reconnect CarPlay before expecting the new offer.

## 4. `/info` display description

Publish the main display first and optional AltScreen second.

Required role fields:

- main display: `type=110` (`StreamType::Screen`);
- AltScreen: `type=111` (`StreamType::AltScreen`);
- each display has its own fixed UUID;
- each display has independent width, height, physical dimensions, and maximum FPS.

Publishing only a second UUID and dimensions is insufficient. The explicit `type=111` is required for iPhone to identify the alternate display and issue the matching second screen `SETUP`.

### View areas

If the accessory enables the `viewAreas` session feature, every offered display must include matching geometry:

- non-empty `viewAreas`;
- at least one area covering the advertised display;
- `initialViewArea`, currently index `0`;
- a valid safe area inside the display bounds;
- origins and dimensions that do not overflow or exceed the display.

For the current full-screen implementation:

```text
viewAreas[0].originX = 0
viewAreas[0].originY = 0
viewAreas[0].width  = display width
viewAreas[0].height = display height
safeArea            = the same full rectangle
initialViewArea      = 0
```

Do not advertise `viewAreas` while omitting these fields. A real iPhone was observed accepting `RECORD` and then repeatedly tearing the session down when the feature and display geometry were inconsistent.

## 5. Session feature negotiation

The receiver must publish only capabilities it can actually process. The session response uses the intersection between iPhone features and accessory features.

- Publish `viewAreas` only when complete view-area geometry is present.
- Publish `altScreen` only when AltScreen is enabled and the receiver has an independent type-111 receive path.
- Do not infer successful negotiation from configuration alone.
- Log the actual session `enabledFeatures` response.

A phone advertising `altScreen` proves phone capability only. A real second stream is established only after a matching AltScreen RTSP `SETUP`.

## 6. RTSP lifecycle

The receiver must support two independent screen slots:

- main: type 110 and/or main UUID;
- AltScreen: type 111 and/or Alt UUID.

Requirements:

1. Keep independent `TcpHelper<ScreenReceiverSession>` instances.
2. Pass both `(stream_type, display_uuid)` into the sink.
3. Map the stream to an explicit `VideoRole::Main` or `VideoRole::Alt`.
4. Reject mismatched pairs such as `type=110` with the Alt UUID.
5. Reject AltScreen setup when AltScreen was not enabled for that session.
6. One stream's EOF or teardown must not clear the other stream.
7. Session-level teardown must end both streams only when the owner session epoch matches.
8. Never classify roles by arrival order; iPhone may set up AltScreen before main.

Observed valid negotiation contains log entries similar to:

```text
Opening AltScreen stream ... UUID ...1081
Opening Screen stream ... UUID ...1080
```

Repeated `SETUP → RECORD → TEARDOWN` means the phone is rejecting session state; it is not a browser canvas problem.

## 7. UI activation commands

A negotiated AltScreen socket does not guarantee that iPhone will render content into it.

### Main display

After the event channel and session are ready, request main UI explicitly. The original input-only implementation needed this to move the main stream from an RTSP session to actual video production.

### AltScreen

Request the instrument-cluster UI explicitly for the Alt UUID. The current reference URL is:

```text
maps:/car/instrumentcluster/map
```

The event command must identify the destination display. Equivalent command intent is:

```text
showUI/requestUI:
  uuid = 5E2D6D6C-8E32-4E2A-9A74-000000001081
  url  = maps:/car/instrumentcluster/map
```

Stopping Alt UI must also target the Alt UUID. Do not send a global/empty `stopUI` in a dual-display session.

Timing matters:

- `RECORD` can occur before both individual screen `SETUP`s.
- Do not permanently lose activation by sending it too early.
- Retain desired main/Alt UI state and issue/retry the command once the event channel and matching display stream are ready.
- Commands are best-effort and their response status must be logged.

The exact command spelling and plist schema must be verified against the receiver's command serializer and live iPhone response; do not assume that a reference implementation's raw `showUI` name is interchangeable with CatPlay's existing `requestUI` enum without testing.

## 8. Keyframe requests

Dual-display keyframe requests must be display-specific.

Required plist intent:

```text
forceKeyFrame.params.uuid = target display UUID
```

An empty/global `forceKeyFrame` may work in a single-display session but is ambiguous once type 110 and type 111 are both active. The observed failure mode was:

- both RTSP streams opened;
- both streams delivered SPS/PPS;
- main and Alt remained `waiting-keyframe`;
- no IDR arrived because the command did not identify the target display.

Implementation requirements:

- route each CPMF `REQUEST_KEYFRAME(session, stream)` to the role associated with that exact stream;
- map main role to UUID `...1080` and Alt role to UUID `...1081`;
- maintain independent keyframe throttles per role;
- do not let a recent main request suppress an Alt request through one shared timestamp;
- request an initial keyframe after each display is activated/configured;
- request another keyframe after loss, decode reset, queue overflow, or browser startup miss;
- validate session and stream IDs before forwarding a reverse request.

## 9. CPMF mapping and memory bounds

CPMF v1 uses `VIDEO_START.p0` as the explicit screen role:

- `0 = main`;
- `1 = alt`.

Each role owns:

- one configuration buffer, capped at 64 KiB;
- four encoded frame slots, capped at 1 MiB each;
- independent stream ID;
- independent configuration generation;
- independent sequence state;
- independent keyframe request state.

Two screens add at most about 8.2 MiB of C++ frame/config storage, plus the existing bounded parser buffer. Large slots are allocated lazily on the heap; placing them on the worker thread stack caused an approximately 8 MiB stack overflow/segmentation fault on the 1 GB Zero 2W.

Rust uses a shared bounded producer queue rather than an unbounded queue per stream. It holds at most eight encoded frames (8 MiB worst case across both screens) and drains at most four per service iteration. Queue overflow must drop predictably and request a keyframe for the affected role. The earlier two-frame/one-drain setting covered only about 22 ms at the combined offered rate of 90 fps and could drop frames whenever the single-thread RTSP reconciler briefly ran longer.

Endpoints:

- `/media/video/main`;
- `/media/video/alt`;
- `/media/video` remains a compatibility alias for main.

`/media/video/*` uses the `X-Media-After` request header. The query-string form `?after=...&timeout=...` is invalid for this API.

## 10. Browser behavior

The browser owns one `VideoDecoder` and one canvas per role. Decode queue depth is capped at two per decoder.

Important rules:

- configure `VideoDecoder` synchronously as soon as SPS/PPS is available;
- poll a stream in `waiting-keyframe`, not only in `active`;
- on HTTP 204 while waiting for the first frame, call `/api/media/keyframe` for that role and retry quickly;
- do not wait for a slow global state-poll interval before fetching frames;
- one HTTP response may contain up to two consecutive video-frame records; decode all of them in sequence;
- when `X-Media-After` already names the newest cached frame, return HTTP 204 rather than rewinding the client to an old IDR;
- expose measured browser decode FPS separately for main and Alt;
- use one persistent response-body stream per screen: even HTTP keep-alive request/response polling remains vulnerable to SSH-forward round-trip jitter;
- keep that response open while a screen temporarily waits for a recovery keyframe; closing and reopening it would itself request more keyframes and create a reconnect storm;
- detect browser abort/refresh with bounded nonblocking `poll` plus `recv(MSG_PEEK)`, so an idle persistent stream cannot leave a worker stuck in `CLOSE_WAIT`;
- service HTTP through a fixed four-worker pool with a bounded eight-socket admission queue, so the two persistent video streams, state, and touch have bounded independent execution slots;
- parse CPMF incrementally in the browser because one network chunk may split a record or contain several records;
- stream PCM continuously with a per-audio-stream sequence cursor. Polling only during the 500 ms status refresh while retaining ten 10 ms chunks yields roughly 100 ms of sound followed by 400 ms of silence;
- consume each decoded output stream on one 128 KiB small-stack native PCM clock, started only after CatPlay's RTP latency gate and advanced with absolute monotonic 10 ms deadlines. A Tokio interval with `MissedTickBehavior::Skip` on the busy current-thread executor permanently deletes delayed audio ticks; live testing measured 12.426 s queued into a 2.972 s decoded ring, causing 9.454 s overflow and silence before CPMF. The evaluation-only silence microphone uses the same absolute-deadline clock so speech-recognition timing cannot drift for the same reason;
- preserve separate logical audio streams and forward CarPlay `duckAudio` / `unduckAudio` commands over CPMF. The browser applies the iPhone-provided dB target and ramp duration only to the media GainNode while leaving navigation/prompt audio at its own gain. CatPlay parses and dispatches the command, but the accessory sink remains responsible for applying it and mixing the streams;
- bound the shared dual-stream PCM ingress at 32 chunks (about 120 KiB worst case) and drain at most eight per service iteration. Retain at most 16 chunks per stream in C++ (about 120 KiB total worst case), start browser playback with a 120 ms cushion, and cap scheduled nodes at 64 to absorb bounded SSH/browser jitter without unbounded latency.
- treat byte-identical SPS/PPS with unchanged dimensions as the same decoder configuration even if the producer assigns a new config ID around an IDR; otherwise each requested keyframe creates a decoder-reset/keyframe feedback loop;
- reset only the decoder whose actual stream/configuration changed;
- never clear the other canvas because one role ended;
- use `http://localhost` through the SSH tunnel for WebCodecs secure-context behavior.

A browser black canvas does not prove the CarPlay stream is absent. Check RTSP setup, CPMF config bytes, queued frames, and keyframe state separately.

## 11. Telemetry requirements

For each screen expose at least:

- role;
- selected flag;
- negotiated stream ID;
- state;
- configuration generation;
- SPS/PPS byte count;
- queued frame count;
- dropped frame count when available.

Protocol telemetry and media telemetry must agree but are not interchangeable. If RTSP logs and CPMF show a real Alt stream while high-level `secondScreen` still says `not-offered`, treat that as a telemetry mapping defect; do not discard the stronger per-stream evidence, and do not falsely call it active until an IDR/frame is received.

## 12. Web API

- `GET /api/display-config`;
- `POST /api/display-config` with all seven configuration fields;
- `POST /api/media/keyframe` with an exact current session/stream target;
- `POST /api/control` with bounded main-display touch `phase=down|move|up`, `x`, and `y` fields.

Main-display browser pointer events are scaled to the coded canvas, paced to animation frames, serialized, and forwarded through a bounded reverse CPMF queue. CatPlay validates the current session, main-display role, dimensions, and coordinates before issuing an encrypted `hidSendReport`. AltScreen remains display-only: no touch capability is claimed for type 111.

Configuration remains separate from negotiated state. The webpage must label changes as applying to the next session.

## 13. Current live findings

The latest tested dual-screen build has demonstrated:

- iPhone advertises `altScreen`;
- session negotiation includes `viewAreas` and `altScreen`;
- `/info` publishes main `type=110` and Alt `type=111` with fixed UUIDs;
- iPhone sends an independent AltScreen `SETUP type=111`;
- CatPlay opens separate main and Alt RTSP screen receivers;
- CPMF assigns independent stream IDs;
- both streams can reach `waiting-keyframe` with valid config.

A later live session validated both roles as CPMF `active`: main stream ID 2 and Alt stream ID 1 each had valid SPS/PPS, queued IDR frames, and `/media/video/alt` returned HTTP 200. An exported Alt packet decoded successfully as H.264 `1280x720`, but the decoded image was completely black. This proves the RTSP, CPMF, HTTP, and decoder paths were operating and that the black image originated in the phone's Alt encoder output.

That session used CatPlay's `requestUI` command with the Alt UUID. The live black frame, together with the reference implementation behavior, requires a distinct `showUI` event for cluster activation:

```text
type = showUI
params.uuid = 5E2D6D6C-8E32-4E2A-9A74-000000001081
params.url = maps:/car/instrumentcluster/map
```

Until a non-black frame is observed after this command, the protocol transport may be reported `active`, but user-visible Alt content must remain reported as unvalidated/black rather than claimed working.

A subsequent `showUI` live test produced visible Apple Maps briefly and exposed another independent session requirement. When Maps/Siri entered speech recognition, iPhone requested duplex MainAudio:

```text
type=100
input=true
audioType=speechRecognition
audioFormat=536870912 (0x20000000, Opus 24 kHz mono)
audioLatencyMs=80
```

The old input-only sink rejected duplex audio, returned RTSP 500, and iPhone then closed both screen sockets and sent session `TEARDOWN`. Therefore optional audio or microphone setup must never be allowed to tear down otherwise healthy displays. The evaluation implementation accepts the decoded downlink and supplies a bounded 10 ms silent microphone source; it must remain labelled evaluation/silence and not be reported as a real physical microphone. A later stability run observed two such speech-recognition microphone setups with zero audio-open failures, RTSP 500 responses, session teardowns, or screen EOFs while both displays remained active.

## 14. Debugging sequence

When AltScreen has no picture, check in this order:

1. `alt_enabled=true` was loaded before the current session.
2. `/info` contains the Alt UUID and `type=111`.
3. `viewAreas`, safe area, and `initialViewArea` are internally valid.
4. Session `enabledFeatures` includes both `viewAreas` and `altScreen`.
5. iPhone sends a second screen `SETUP` for type 111/Alt UUID.
6. Receiver opens the Alt screen socket without clearing main.
7. Alt CPMF `VIDEO_START` has role `1` and a unique stream ID.
8. Alt SPS/PPS arrives and `configBytes > 0`.
9. Alt-targeted `showUI/requestUI` is accepted.
10. Alt-targeted `forceKeyFrame` includes UUID `...1081`.
11. An IDR arrives and state changes from `waiting-keyframe` to `active`.
12. `/media/video/alt` returns HTTP 200 with advancing sequence.
13. Alt `VideoDecoder` is configured and its canvas is updated.

This sequence prevents misdiagnosing a protocol, media, keyframe, HTTP, WebCodecs, or UI-activation failure as the same generic “black screen” problem.

## 15. Rollout gates

1. Unit-test strict configuration parsing and atomic persistence.
2. Unit-test display plist serialization for type, UUID, view areas, and safe area.
3. Unit-test feature intersection.
4. Unit-test two simultaneous RTSP screen slots and independent teardown.
5. Unit-test role assignment when Alt arrives before main.
6. Unit-test interleaved CPMF main/Alt records.
7. Unit-test exact session/stream keyframe requests and per-role throttling.
8. Build/test x86_64 locally.
9. Cross-build ARM64 and verify AArch64, PIE, interpreter, GLIBC/GLIBCXX, and SHA256.
10. Deploy only to a new timestamp-isolated stage.
11. Preserve RNDIS/AP/network state and existing system services.
12. Verify main remains active with Alt disabled.
13. Enable Alt and report only states observed from the real iPhone.
14. Confirm `/media/video/main` and `/media/video/alt` independently return advancing frames.
15. Validate both canvases through the localhost SSH tunnel.
