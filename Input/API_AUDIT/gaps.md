# Unhandled / visible gaps

Declarations seen by the parser's broad probes but not captured by its extraction patterns,
plus parser-completeness counters. Nothing here is dropped silently.

## Completeness counters

- `apollo_sdk`: `{"excluded_path_segment": "/internal/", "extracted": 795, "files_scanned": 74, "non_public_members": 72, "visibility_note": "visibility is derived from the declaration line; non-public members are kept visible"}`
- `feature_config_java`: `{"extracted": 73, "static_final_string_total": 76}`
- `livi_channel_registrations`: `58`
- `livi_input_command`: `{"entries": 15}`
- `service_types_h`: `{"constexpr_complete": true, "constexpr_lines_total": 310, "constexpr_rows_extracted": 310}`
- `service_types_kt`: `{"const_val_complete": true, "const_val_lines_total": 309, "const_val_rows_extracted": 309}`
- `session_cpp`: `{"channel_cases": 24, "msg_cases": 60, "switch_statements": 5}`
- `vehicle_lib`: `{"declarations_extracted": 492, "files": ["Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CCarLifeLib.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CCarLifeLibWrapper.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CCarLifeLog.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CCmdChannelModule.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CConnectionSetupModule.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CConnectManager.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CCtrlChannelModule.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CMediaChannelModule.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CommonUtil.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CSocketConnector.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CTranRecvPackageProcess.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CTTSChannelModule.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CVideoChannelModule.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CVirtualShell.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/CVRChannelModule.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/ISocket.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/socket.h", "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include/socketv6.h"], "structs_extracted": 57, "unhandled_declaration_like": 0}`

## Gap entries

| source | line | name | reason |
| --- | --- | --- | --- |
| `Reference/LIVI/native/livi-helperd/crates/iap2-mfi/src/lib.rs` | 25 | `MfiError` | pub enum outside framing/control crates (not enumerated) |
| `Reference/LIVI/native/livi-helperd/crates/livi-aa/src/link.rs` | 11 | `Item` | pub enum outside framing/control crates (not enumerated) |
| `Reference/LIVI/native/livi-helperd/crates/livi-aa/src/tls.rs` | 32 | `Error` | pub enum outside framing/control crates (not enumerated) |
| `Reference/LIVI/src/preload/index.ts` | 13 | `usb-event` | ipcRenderer channel used outside a captured api property (module-scope listener) |
| `Reference/LIVI/src/preload/index.ts` | 34 | `projection-audio-chunk` | ipcRenderer channel used outside a captured api property (module-scope listener) |
| `Reference/LIVI/src/preload/index.ts` | 43 | `cluster-video-resolution` | ipcRenderer channel used outside a captured api property (module-scope listener) |
| `Reference/LIVI/src/preload/index.ts` | 48 | `telemetry:update` | ipcRenderer channel used outside a captured api property (module-scope listener) |
| `Reference/LIVI/src/preload/index.ts` | 52 | `projection-event` | ipcRenderer channel used outside a captured api property (module-scope listener) |
| `Reference/carlife-vehicle-lib/CarLife-Android-Vehicle/src/com/baidu/carlifevehicle/util/CarlifeConfUtil.java` | 38 | `private static final String TAG = "CarlifeConfUtil";` | not matched |
| `Reference/carlife-vehicle-lib/CarLife-Android-Vehicle/src/com/baidu/carlifevehicle/util/CarlifeConfUtil.java` | 39 | `private static final String CONF_FILE_DIR = "/data/local/tmp";` | not matched |
| `Reference/carlife-vehicle-lib/CarLife-Android-Vehicle/src/com/baidu/carlifevehicle/util/CarlifeConfUtil.java` | 40 | `private static final String CONF_FILE = "bdcf";` | not matched |

