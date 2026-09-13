# Wireless CarPlay 输入

- `include/`、`src/`：C++ 输入适配器与媒体 IPC 客户端。
- `MFI/`：MFi Auth 3.0 原生工具及总线安全检查。
- `Engine/`：独立 Rust 侧车进程、CatPlay 补丁边界和部署文件。

Engine 与 C++ Core 通过有界 IPC 隔离；参考 CatPlay 源不直接链接到 C++ 产品进程。

---

## CarPlay 本地扩展接口（CPMF）协议表

注意：下表描述本项目的 IPC 定义，不是“全部 CarPlay 功能已接入”的证明。
2026-09-13 核查发现真实 CatPlay 引擎补丁没有对应 25 类扩展下行生产者和 11 类扩展上行处理器。
C++ 解码/状态写入与模拟 IPC 测试不能替代手机端端到端验证；具体状态见 [核查报告](../API_AUDIT/README.md)。

**位置**：`include/wirelesscarplay/catplay_media_ext.hpp`（Input 侧拥有）+ `src/catplay_media_client.cpp`（编解码与分发）。
**为什么不改基础协议头**：CPMF 的基础定义在 `Core/Convert/include/wirelesscarplay/catplay_media_protocol.hpp`，归 Core 所有，
本任务的写入范围只有 `Input/WirelessCarPlay/**`，所以扩展做成**独立头文件**：复用 Core 的帧格式常量、字节序助手与
`valid_declared()/valid_static()` 校验，自己实现"新类型也认"的解码与解析器。旧类型（0x0001–0x0023、0x8001–0x8002）
的编号与语义**一字未改**。

**关键行为**：类型不认识时**按 `payload_bytes` 整条跳过并计数**（`record_skipped()`），绝不因此撕掉会话 ——
与 CarLife 侧"不理解的记录只丢不拆会话"一致。只有分帧错误才断链（`fail(true)`）。

### 帧格式（与基础 CPMF 完全一致）

```
0..3  'CPMF' | 4 ver=1 | 5 =0 | 6..7 hdrlen=64 (BE) | 8..9 type (BE) | 10..11 flags (BE)
12..15 stream_id | 16..23 session_id | 24..31 sequence | 32..39 pts | 40..43 payload_bytes
44..47 p0 | 48..51 p1 | 52..55 p2 | 56..59 p3 | 60..63 保留=0        其后紧跟 payload
```

### 下行（引擎/手机 → 车机）

| 号 | 类型 | 字段 | 来源（iAP2/CarPlay）|
|---|---|---|---|
| 0x0030 | Metadata | payload=`title\0artist\0album\0album_artist\0app\0genre\0`；p0=flags(bit0 valid,bit1 playing)，p1=duration_ms，p2=position_ms，p3=track_no<<16\|track_count | iAP2 **Now Playing `0x5000–0x5003`** `NowPlayingUpdate`（`Temp/audit/00-iap2-authoritative-inventory.md`）|
| 0x0031 | ArtworkChunk | payload=封面字节；p0=chunk_index，p1=chunk_count，p2=total_bytes，p3=revision | 同上 + iAP2 File Transfer 会话 |
| 0x0032 | LyricsChunk | payload=LRC；p0/p1/p2/p3 同 Artwork | 非 iAP2 标准接口（App 侧提供），见审计 6.4 |
| 0x0033 | MediaLibrary | p0=op(0 resume,1 pause,2 seek,3 next,4 prev,5 revision)，p1=position_ms，p2=revision | iAP2 **媒体库 `0x4C00–0x4C09`** |
| 0x0034 | Navigation | payload=**固定 464 字节**（`road_name[192]`+`next_road_name[192]`+`icon[32]`+`destination[32]`，均为定长 NUL 填充；其后 BE：`distance_remaining_m` u32、`eta_epoch_s` u64、`lane_bitmap` u16、`destination_reached` u8）；p0=**maneuver_code 原值**，p1=distance_to_maneuver_m，p2=time_remaining_s，p3=bit0 active | iAP2 **Route Guidance `0x5200–0x5203`** `RouteGuidanceUpdate`/`ManeuverUpdate` |
| 0x0035 | Vehicle | payload=80B BE 固定结构（speed_kph、rpm、fuel_pct、range_km、outside_temp_c、odometer_km、double lat、double lon、float heading、doors、gear、night_mode、lights、parking_brake、flags、vin[24]）| iAP2 **车辆状态 `0xA100–0xA102`** + **定位 `0xFFFA–0xFFFC`** |
| 0x0036 | Telephony | payload=`caller\0number\0`；p0=call_state，p1=duration_s，p2=signal<<8\|battery，p3=bit0 dtmf_supported | iAP2 **电话 `0x4154–0x4161`** |
| 0x0037/0x0038 | ContactsChunk / CallLogChunk | payload=`name\0number\0…`；p0=chunk_index，p1=chunk_count，p2=total_count，p3=revision | 同上 |
| 0x0039 | Ducking | p0=nav_active，p1=media_ppm，p2=nav_ppm，p3=transition_ms | 语义化音频仲裁（对照 CarLife `AudioTrackManagerDualNormal.java:376-395` 的按比例压低）|
| 0x003A | SafeArea | p0=top<<16\|bottom，p1=left<<16\|right，p2=width<<16\|height | CarPlay `projectionSafeArea*`（审计 8.1）|
| 0x003B | AuxPlane | p0=enabled，p1=x<<16\|y，p2=w<<16\|h，p3=primary_plane | 多屏内容注入 dash/aux（审计 8.3，`aa-proxy-rs/src/config.rs:612-621`）|
| 0x003C | Calibration | p0=gamma<<16\|contrast，p1=saturation | 显示校准（审计 8.2，`livi-compositor/ctrl.rs:273,279`）|
| 0x003D | DayNight | p0=day_night | `CommandSetNightMode` |
| 0x003E | FrameRate | p0=target_fps，p1=actual_fps | 视频帧率协商 |
| 0x003F | FileTransfer | payload=`file\0`；p0=active，p1=bytes_done，p2=bytes_total | iAP2 File Transfer 会话 |
| 0x0040 | Ota | payload=`version\0`；p0=state，p1=progress_pct | — |
| 0x0041 | Activation | p0=state(0 未知 1 未激活 2 激活中 3 已激活) | iAP2 **鉴权 `0xAA00–0xAA15`** |
| 0x0042 | ContentEncryption | p0=mode(0 关 1 要求 2 已启用) | 同上（审计 5.4）|
| 0x0043 | MultiSession | payload=`id\0id\0…`；p0=count，p1=active_index | LIVI 多会话 / `AirPlaySessionGuard` 互斥 |
| 0x0044 | AssistiveTouch | p0=enabled | iAP2 **辅助触控 `0x5400–0x5404`** |
| 0x0045 | VoiceOverState | p0=enabled | iAP2 **VoiceOver `0x5601–0x5613`** |
| 0x0046 | HidModeState | p0=mode | iAP2 **HID `0x6800–0x6806`** |
| 0x0047 | ProximityState | p0=state(0 远 1 近) | 接近传感器 |
| 0x0048 | Capability | p0=max_multi_touch，p1=touchpad<<8\|knob，p2=flags(bit0 voice,bit1 hid,bit2 voiceover) | 能力声明 |

### 上行（车机 → 引擎/手机）—— 修掉"非 Touch 直接丢弃"

`queue_control()` 原先 `if(e.type!=Touch) return;`，**CarPlay 侧硬键被显式丢弃**（审计 4 表 L400）。
现在 9 种控制类型全部下发；只有 Touch 的高频 Move 做合并，队列满时丢最旧的也保证按键/电话/车控发出去。

| 号 | 类型 | 字段 | 对应 ControlEvent |
|---|---|---|---|
| 0x8010 | Key | payload=键名；p0=**(kind<<16)\|id**（kind 0=MediaButton、1=TelephonyButton，见下节；**不是 CarLife 的 `KEYCODE_*`，也不是 Android keycode**），p1=action | `Type::Key` |
| 0x8011 | MultiTouch | payload=count×8B（int16 x,int16 y,uint8 id,uint8 phase,uint16 保留）；p0=count | `Type::MultiTouch` |
| 0x8012 | Knob | p0=dir(0 左 1 右 2 上 3 下 4 按下)，p1=steps(有符号) | `Type::Knob` |
| 0x8013 | Gesture | payload=手势名 | `Type::Gesture` |
| 0x8014 | ProximityEvent | p0=state | `Type::Proximity`（取 `x`）|
| 0x8015 | Voice | p0=action(0 按下 1 松开 2 长按) | `Type::Voice`（取 `x`）|
| 0x8016 | TelephonyCtrl | payload=DTMF 串；p0=op(0 接听 1 挂断 2 拨号 3 DTMF 4 静音 5 保持) | `Type::Telephony`（`x`=op，`dtmf`=串）|
| 0x8017 | VehicleCtrl | payload=车控名；p0=value | `Type::VehicleCtrl` |
| 0x8018/0x8019/0x801A | HidModeSet / VoiceOverSet / AssistiveTouchSet | p0=mode / enabled | 预留（`x`=值）|

**非指针类事件的参数约定**：`Proximity`/`Voice`/`Telephony`/`VehicleCtrl` 用 `ControlEvent::x` 承载标量参数
（它们是"带一个数值的按钮"，没有坐标语义），在协议里就是 p0。这样不用给冻结的 `ControlEvent` 加字段。

### 硬键标识：**两个生态两套码表，且都不是 Android keycode**

CarPlay 与 CarLife 的硬键码表**互不相干**，数字相同也不代表语义相同。三个表都核对过出处：

| 码表 | 出处 | 样例（**注意同数不同义**）|
|---|---|---|
| **CarPlay / 我们的 `Key` 消息** | `Reference/CatPlaySource/carplay/catplay_hid/src/media_buttons.rs`（HID Consumer 用法 0xB0/0xB1/0xCD/0xB5/0xB6/0x29E）、`.../catplay_hid/src/telephony.rs` | `MediaButton`: None/Play/Pause/PlayPause/NextTrack/PrevTrack/ACNavGuidance = **0..6**；`TelephonyButton`: Up/HookSwitch/Flash/Drop/Mute/PhoneKey0-9/Star/Pound/Delete = **0..17** |
| **CarLife** | `Reference/apollo-DuerOS/.../carlife-sdk/src/main/java/com/baidu/carlife/sdk/internal/protocol/ServiceTypes.kt:356-408`（消息 `MSG_TOUCH_CAR_HARD_KEY_CODE = 0x00068008`） | `KEYCODE_MUTE`=**0x13(19)**、`KEYCODE_MOVE_UP`=0x17(23)、`KEYCODE_BACK`=0x0E(14) |
| *（对照组，不用）* Android ADB | 仅作对照 | `KEYCODE_DPAD_UP`=19、`KEYCODE_DPAD_DOWN`=20、`KEYCODE_BACK`=4 |

> **这就是为什么数字本身没有意义**：`19` 在 Android 里是 DPAD_UP，在 CarLife 里是 **MUTE**。
> CarLife 车道曾用 Android ADB 码表发 `up`（19/20/4），因此那一轮 A4 的“验证通过”**不成立**——
> 码值确实发出去了，但 CarLife 侧收到的是 MUTE 等含义，不是“上/下/返回”。

**我们这边（CarPlay/CPMF）**：`hard_key_code()` 把名字解析成 CarPlay 标识，高位类别 `0=MediaButton`、`1=TelephonyButton`，
低位是标识号，所以 `p0 = (kind<<16) | id`；与 CarLife 的 `KEYCODE_*`、与 Android 均无关系。
可用名：`media.play|pause|play_pause|next|prev|ac_nav`、`tel.up|hook|flash|drop|mute|0..9|star|pound|delete`，
也可直接给十进制号（按 Media 类别）。

### 音频角色映射

CPMF `AudioStart`/`AudioChunk` 的 `p0` 就是 CarPlay `AudioType`，序号出处
`Reference/CatPlaySource/carplay/catplay_carplay/src/msg/streams.rs` 的
`enum AudioType { Default, Alert, Media, Telephony, SpeechRecognition, Compatibility }`（0..5，与 Core 校验的 `p0<=5` 一致）。
`role_from_audio_type()` 映射到 `mvp::AudioRole`：Default/Media/Compatibility→Media、Alert→Alert、Telephony→Telephony、
SpeechRecognition→Voice。`AudioGain` 是绝对增益，作用在当前活跃角色上；语义化闪避见 0x0039。

### 集成步骤（root CMakeLists 归集成方）

本任务的写入范围只有 `Input/WirelessCarPlay/**`，所以测试自带独立编译路径 `tests/run_ext_tests.sh`。
接入 CMake 只需三行：

```cmake
add_executable(media_ext_tests Input/WirelessCarPlay/tests/media_ext_tests.cpp)
target_link_libraries(media_ext_tests PRIVATE synthetic_wireless Threads::Threads)
add_test(NAME media_ext_tests COMMAND media_ext_tests)
```

### 本轮**没有**做成的项（如实列出，不是占位）

1. **多声道/杜比透传（>2 声道）**：CPMF 的 `AudioStart`/`AudioChunk` 是 Core 校验的（`p2` 只允许 1/2 声道、
   采样率在固定白名单内），`RealMediaStore::start_audio()` 也**硬性** `channels!=1&&channels!=2 → return false`，
   并有 `valid_rate()` 限制。要真透传 ALAC/AAC-LD/Opus 或多声道，必须**放宽 Core 的这两处校验**（Core 所有，本任务禁改）。
   ≤2 声道的角色/编码信息本轮已经落到 `AudioState`（codec/channels/sample_rate/role）✓。
2. ~~**导航逐向的落库**~~ —— **本轮已完成**：`Navigation` 解码后直接 `core_.set_navigation_state()`（`Core` 已补 `NavigationState`）。
   载荷改成固定 464 字节布局（四种文本定长 NUL 填充 + BE 二进制字段），因为文本后面还要跟二进制字段，
   用 `'\0'` 连接会让接收侧分不清“最后一个字符串”和“二进制段开始”。
   **`maneuver_code` 原值透传**：`maneuver` 只在 p0 落在本 CPMF 消息自身枚举序号范围内时顺序透传，越界留 `Maneuver::None` ——
   **不编 CarLife / Android Auto 的映射表**（参考树里也没有可引用的表）。
3. **引擎侧待接**：以上新消息需要 CPMF 生产者（`catplay_c2a`/`cp-native`）真的发出来。我们这侧的解码/编码/分发已完整
   并由端到端测试覆盖（生产者由测试扮演），但**引擎里产生这些数据的源头**（iAP2 `NowPlayingUpdate`、
   `RouteGuidanceUpdate`、`LocationInformation`、Telephony、HID 等）在参考树 `Reference/CatPlaySource` 内，
   按任务约束不得改动参考树，因此这些属于"引擎侧待接"，不是本侧未实现。
4. **音频"按 role 分流到浏览器"**：`SessionCore` 边界已经通了，但浏览器实际取音频的路径是
   `RealMediaStore::audio_packet()` → `mvp_server` 的 `/media/...`，那两处归 Core/Web。多声道要走浏览器还需要第 1 条先解禁。

### 复现

```bash
wsl.exe -d Debian -- bash /mnt/d/littlethings/CarPlay/zero2w/Input/WirelessCarPlay/tests/run_ext_tests.sh
```
