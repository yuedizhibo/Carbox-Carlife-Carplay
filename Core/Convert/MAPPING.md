# CarLife ⇄ CarPlay 功能对接规范

本文是 `Core/Convert`（语义转换）与 `Core/Forward`（同协议直通）的实现依据。
每一条都必须能回答：**输入侧是什么消息 → Core 内部什么结构 → 输出侧什么消息 → 是否需要转换**。

## 0. 三条铁律

1. **视频不做无谓解码/重编码。** 输入侧已是 H.264 Annex-B，输出侧也要 H.264，那就只做「参数集 + 访问单元」的重新封装与时间戳重排（直通）。只有下列情况才转码：编码不同（如 H.265）、分辨率/帧率超出输出侧协商能力、或像素格式/码率必须重压。
2. **音频按用途分流，不混一路。** 娱乐音与导航音在 CarLife 侧本来就是两条通道（`MEDIA_*` / `NAV_TTS_*`），CarPlay 侧也是不同的音频流（media / alert）。分流必须保持，才能在输出侧做正确的 ducking 与混音策略。
3. **一切缓存有界。** 每个输入一份有界媒体面（`RealMediaStore`：视频 **48** 帧 ring、音频 **128** 块 ring/每流、1 MiB 单帧上限；音频最多 **6** 条独立流，足以覆盖 Media / Navigation / Telephony / Alert / Voice 五个 role 各占一条 ring），输入断开必须清空，禁止旧帧/旧封面泄漏到新会话。

   > 修订记录：本行原写“视频 4 帧、音频 16 块”，与代码早已不符（`kVideoFrames=48`、`kAudioChunks=128`、`kAudioStreams=6`）。
   > 这处陈旧描述直接导致 `Core/Convert/tests/media_tests.cpp` 里“20 条撞 16 槽 → 发 16 条 / 丢 4 条”的断言变成假失败。

## 1. 通道与语义对照

| 功能 | CarLife（HU 侧，本仓已实现） | Core 内部结构 | CarPlay 输出侧 | 转换类型 |
|---|---|---|---|---|
| 视频 | `ch::VIDEO`；`VIDEO_DATA` 0x00020001（H.264 Annex-B）、`VIDEO_HEARTBEAT` 0x00020002 | `RealMediaStore` 视频面：`VideoConfig`(SPS/PPS) + `VideoFrame`(IDR 对齐) | CarPlay screen stream（H.264） | **直通** |
| 娱乐音频 | `ch::AUDIO`；`MEDIA_INIT` 0x00030001、`MEDIA_DATA` 0x00030006 / `MEDIA_DATA_ENCODER` 0x00030007、`MEDIA_STOP/PAUSE/RESUME_PLAY/SEEK_TO` | `AudioChunk`，`audio_type = 2 (media)` | CarPlay media audio | PCM16 直通；AAC 需重封装 |
| 导航音频 | `ch::TTS`；`NAV_TTS_INIT` 0x00040001、`NAV_TTS_DATA` 0x00040003 / `NAV_TTS_DATA_ENCODE` 0x00040004、`NAV_TTS_END` 0x00040002 | `AudioChunk`，`audio_type = 1 (alert)` | CarPlay alert/nav audio | PCM16 直通 + ducking |
| 麦克风（上行） | `ch::VR`；`VR_AUDIO_INIT` 0x00050002、`VR_AUDIO_DATA` 0x00050003 / `VR_DATA` 0x00058001、`VR_AUDIO_STOP` 0x00050004、`VR_AUDIO_INTERRUPT` 0x00050006 | `HostSink::takeMicrophone()` 缝 → `ControlEvent` 之外的第二条上行通道 | CarPlay mic uplink（iAP2/RTSP） | 采样率/声道转换 |
| 触控 | `ch::TOUCH`；`TOUCH_ACTION_DOWN/UP/MOVE` 0x00068002/3/4、`TOUCH_ACTION_POINTER_DOWN/UP` 0x0006800B/0C（多点） | `ControlEvent{Type::Touch, Down/Move/Up}` | CarPlay HID 触摸 | 坐标换算 + 能力门控 |
| 硬键 | `TOUCH_CAR_HARD_KEY_CODE` 0x00068008（Android keycode） | `ControlEvent{Type::Key}` | CarPlay HID 按键 | keycode 映射表 |
| 生命周期 | `ch::CMD` 握手：`HU_PROTOCOL_VERSION`→`MD_PROTOCOL_VERSION_MATCH_STATUS`→`MD_INFO`→鉴权四步→能力协商 19 项→`VIDEO_ENCODER_INIT`/`INIT_DONE`/`START` | `SessionCore`：`upsert_source(id, External, connected)` + 活跃态选择 | CarPlay 会话生命周期 | **1:1 绑定**，断开即清媒体面 |
| 关键帧请求 | 输出侧要 I 帧时：CarLife 侧通过 `VIDEO_ENCODER_RESET`/帧率控制路径（本仓用 `request_keyframe()` → 媒体面标记） | `RealMediaStore::request_keyframe()` / `take_keyframe_request()` | CarPlay 侧 keyframe request | 透传 |

## 2. 视频：直通的条件与实现

**直通路径（默认）**

```
手机 ──H.264 Annex-B──► CarLife VIDEO ──► CoreHostSink::onVideoAnnexB()
     ──► 提取 SPS(7)/PPS(8) 作为 VideoConfig；IDR(5) 判定关键帧
     ──► RealMediaStore::push_video_frame()  （有界 ring，IDR 对齐）
     ──► CarPlay screen stream（重新封装为输出侧记录，字节不动）
```

关键点（本仓已实现的部分）：

- 参数集提取与 IDR 判定：`Input/WirelessCarLifePlus/src/carlife_input.cpp` 的 `forward_video_annexb()`（遍历 Annex-B NAL，收集 SPS/PPS，变更时重建流）
- 有界入队与关键帧对齐：`Core/Convert/src/real_media_store.cpp` 的 `push_video_frame_locked()`（序列连续性检测、缺口即回到等关键帧、ring 满丢旧帧并计数）
- 直通的形状与 CarPlay 一致：视频配置 + 访问单元，无像素级操作

**必须转码的判定表**

| 条件 | 判定依据 | 处理 |
|---|---|---|
| 输入编码不是 H.264 | CarLife 侧固定 H.264（`VIDEO_ENCODER_INIT` 协商即 H.264），故暂不会触发 | — |
| 分辨率/帧率超出输出侧协商能力 | 输出侧会话协商出的 `main_screen` 能力 | 转码（缩放+重编）或拒绝并记录 |
| 输入码率超过输出侧链路预算 | 输出侧 USB/WiFi 带宽与服务端配置 | 转码降码率 |
| 需要叠加 Core 桌面（多输入/断线提示） | 见 §6 桌面合成 | 此时才解码/合成/重编 |

> 桌面叠加是唯一常态化的转码场景，且只在「输出 Core 桌面」时发生；正常直通不叠加。

## 3. 音频：三条流的用途与 ducking

| Core `audio_type` | 来源 | 输出侧用途 | 关键行为 |
|---|---|---|---|
| 2 `media` | CarLife `MEDIA_*` | CarPlay 娱乐音频 | 独立流；导航音出现时按 `AudioGain` 压低 |
| 1 `alert` | CarLife `NAV_TTS_*` | CarPlay 导航/提示音 | **不参与循环播放**，播完即回；触发 ducking |
| 4 `speech-recognition` | CarLife `VR_*`（上行） | CarPlay 语音识别上行 | 与麦克风采集绑定，见 §4 |

- CarLife 的 `AudioInit{sampleRate, channelConfig, sampleFormat}`：`sampleFormat==2` 为 PCM16。非 PCM（AAC，受 `AAC_SUPPORT` 能力位控制）当前本仓只计数，接入时需解码或按 CarPlay 侧可接受编码重封装。
- `channelConfig`：12→双声道，4→单声道（本仓已按此处理）。
- ducking 由输出侧下发（CPMF `AudioGain`：`p0=持续毫秒`、`p1=增益 ppm`），Core 负责把它翻译成 CarPlay 侧的对应控制。

## 3.1 多声道 / 多格式 / 压缩载荷（如实转发，不做任何变换）

**根因**：用户硬要求“**不要自己解码串改音频，尽量用原生方式，支持多声道/杜比**”。所以多声道与杜比只能靠**原样透传**支持，不能被我方校验挡在门外。

| 维度 | 改前 | 改后 | 依据 |
|---|---|---|---|
| CPMF `AudioStart` / `AudioChunk` 声道 | 硬拒 `!=1 && !=2` | `1..kMaxAudioChannels`（=8：5.1=6、7.1=8） | `kMaxAudioChannels`（`catplay_media_protocol.hpp`） |
| 采样率 | 白名单 8000/16000/24000/32000/44100/48000 | 范围 `8000..192000` | `kMinAudioRate`/`kMaxAudioRate` |
| 位深 | 隐含 16 | 8/16/24/32（容器 1/2/3/4 字节） | `valid_audio_format()`，照抄 `asbd.rs:76-90` 的 `fill_pcm(rate, valid_bits, total_bits, channels, float)` 语义 |
| 每包帧数 | 只允许 `p3 <= p1/50`（≈20ms） | PCM 仍用 `p1/50`；**压缩载荷**用 `kMaxOpaqueFramesPerPacket`（8192） | AAC 一包典型 1024 样本，48k 下 21.3ms > 20ms，拿 PCM 的约束会误杀 AAC-ELD |
| 压缩载荷 | 无 | `kFlagOpaquePayload`：不做 PCM 帧长等式，只限上界，**字节原样** | 本层不解码任何非 PCM 载荷 |

**格式描述符（落在既有 16 字节里，不新增消息类型、不动消息号）**

| 字节 | 含义 |
|---|---|
| `[0..3]` | 既有约定（96 / 1 / ≤1 / 0）——**原校验一字未改** |
| `[4]` | codec：0=未声明 1=Pcm 2=Pcm16 3=Pcm24 4=Alac 5=AacLc 6=AacEld 7=Opus |
| `[5]` / `[6]` | valid_bits / total_bits（8/16/24/32） |
| `[7]` | role（AudioRole 0..5） |
| `[8..11]` | format_flags（be32，照抄 `AudioFormatFlags` 低 5 位：1=float 2=signed 4=packed 8=aligned_high 16=native_endian） |
| `[12..15]` | bytes_per_frame（be32） |

**向后兼容**：`[4..15]` 全零 = 旧生产者（只写前 4 字节）→ 语义为“未声明”→ 按 PCM16 处理，**既有调用点（`carlife_input.cpp`、`catplay_media_client.cpp`、旧测试）行为不变**。

**role → 流**：每流一条独立 ring；`audio_type` 推导 role（0 media / 1 **navigation** / 2 media / 3 telephony / 4 voice / 5 alert，与 `Core/Web` 的 `media_audio_type` 名字表一一对应），也可用 `set_audio_role()` 显式覆盖。上限 `kAudioStreams=6`，五个 role 可同时在线。

## 3.2 导航压媒体（ducking）

语义**照抄参考实现**（`AudioTrackManagerDualNormal.java:376-395`：`setVolume(maxVolume / mMusicAudioTrackVolumReduceRatio)`，`mMusicAudioTrackVolumReduceRatio = 3` 见同文件 `:68`）：

- **只压低媒体轨**，导航轨保持 `1000000ppm`；
- 默认 `duck_ratio_ppm=333333`（=1/3）、`duck_transition_ms=300`，可用 `set_duck_config()` 改；
- `set_ducking(true/false)` 幂等（重复调用不产生新的渐变命令）；
- 与 CarLife A1 共用**唯一执行点** `apply_gain_locked()`，因此客户端只有一套渐变逻辑（`carlife_input.cpp:470/524` 直接调 `set_audio_gain()`）；
- 线上就是 CPMF `AudioGain`（`p0`=渐变毫秒、`p1`=增益 ppm），且只在**代次变化**时发一次；
- 会话边界**保留配置**（ratio/transition），只复位当前增益。

实测对应：车机侧日志 `[A1] TTS 开始 -> 压低媒体音 333333ppm (1/3)，渐变 300ms`。

## 3.3 元数据 / 专辑封面 / 歌词（MediaRelay）

`Core/Convert/include/wirelesscarplay/media_relay.hpp`（header-only：`CMakeLists.txt` 不在本模块职责区，故不新增 .cpp）。

- **可注册回调 + 定长缓冲**：`MediaRelaySink{context, on_media_info, on_artwork, on_lyrics}`；正文存 `kMaxArtwork`(128 KiB) / `kMaxLyrics`(4 KiB) 两块定长缓冲。
- **本层不自行发明协议号**：CPMF 消息号由输出侧定义（`Input/WirelessCarPlay/include/wirelesscarplay/catplay_media_ext.hpp`，对应 iAP2 `NowPlayingUpdate 0x5000–0x5003` / `MediaLibraryUpdate 0x4C00–0x4C09`）；本层只决定“何时该发、发什么字节”。
- **去重**：`MediaInfo` 逐字段比较（不用 `memcmp` 整块 —— 有填充字节会假阴性）；封面/歌词按 `(revision, 长度, 内容)` 去重。
- **超限拒绝而不是截断**：歌词截断会显示成一堆残缺的 LRC 时间戳。
- **迟到订阅者**：`artwork()/lyrics()/media_info()` + `revision`，新会话/新页面按 revision 补齐。
- **会话边界**：`clear_media_locked()` 调 `relay_.reset()`，杜绝上一会话的封面泄漏（与铁律 3 同源）。
- 大正文**不进快照**：`RealMediaSnapshot::Media` 只放 `info_generation` / `artwork_revision` / `artwork_bytes` / 同歌词字段，页面按 revision 变化再取，避免每帧拷 128 KiB。

## 3.4 帧率：如实上报 + 请求（**不是**降载手段）

- `set_target_fps(role, fps)`：写入 `DisplayConfig.target_fps` 同源的目标值，并挂一个**待下发请求**（`1..240`，否则拒绝）；
- `take_rate_request(role, fps)`：适配器取走请求，翻译成**输入侧原生消息**（CarLife：`MSG_CMD_VIDEO_ENCODER_FRAME_RATE_CHANGE = 0x0001800C`，`ServiceTypes.kt:60`），≥`kRateRequestIntervalMs`(500ms) 节流；
- `actual_fps`：1 秒滑动窗统计**手机实际推来的帧**（不是入 ring 的帧）—— 这样 `actual_fps` 才能与 `target_fps` 对上，也能分辨“手机真的只给 30”与“我们丢了帧”；
- **不改变任何帧的取舍**：不改分辨率、不改码率、不丢帧（见 `l2-scratch` 第 4 组：设 60fps 后第一个关键帧仍照常入 ring 并 `Active`）。

## 4. 麦克风（上行）

转接盒的麦克风源是**原车有线 CarPlay 会话回传的录音数据**，不是 Zero2W 自带麦克风。
CarLife 的 `MIC_RECORD_WAKEUP_START` / `MIC_RECORD_RECOG_START` 命令启动上行录音，
`MIC_RECORD_END` 停止；`VR_AUDIO_DATA` 是手机发来的下行语音音频，不能作为上行录音请求。

```
原车麦克风 ──► WiredCarPlay 接收 ──► Core 会话路由/必要格式转换 ──► Input 上行 → 手机
```

本仓已有的缝（`HostSink::takeMicrophone(dst, capacity_frames, frames_out)`）：

- 无采集源时返回 false，会话侧只累加 `mic_requests`/`mic_frames` 计数并在页面上展示（当前板上就是这个状态）
- 接入原车回传流时：按输入侧录音格式要求做必要转换后回填；不能从下行 `VR_AUDIO_INIT` 推定上行录音格式
- 反向：CarPlay 语音识别需要上行时，同样走这条缝，只是目的地换成 CarPlay 侧流

## 5. 控制：坐标、能力门控与多点

- **坐标**：页面/CarPlay 侧给的是像素坐标，必须按**输入侧协商分辨率**换算。本仓已在 `CarLifeInputAdapter::on_control()` 用 `HostControl{x,y}` 传入，`Session` 侧按 `negotiated_` 组装 `TouchSinglePoint`。
- **三阶段**：`down` / `move` / `up` 必须分别下发（本仓已把旧的「固定 down+up 点击」改为真实三阶段）。`move` 做合并（同一队列里只保留最新一条），避免有界队列被拖动刷掉。
- **能力门控**：CarLife 的 `MULTI_TOUCH` 能力位决定是否可用多点。未协商成功时只发单点，余下 `POINTER_*` 消息不能发。
- **硬键**：Android keycode ↔ CarPlay HID。本仓 `keycode_for()` 已覆盖 up/down/left/right/enter/back/home/menu/volume_up/volume_down，数字形式也接受。
- **`INPUT_DISABLE`** 能力位为真时，输入侧会拒绝触摸；Core 必须据此**屏蔽**转发并给页面反馈，不能静默丢弃。

## 6. Core 管理桌面（无输入 / 多输入）

`SessionCore` 已经确定了调度语义（`PROJECT_ARCHITECTURE.md` §2）：无输入、或多输入且未手动选择时，活跃源应是桌面。桌面不是「又一个输入」，而是 **Core 自己产出的一个 SVG 帧**，并注册为 `LocalDesktop` 类型的源。

- 产出方式：复用既有 `VideoFrame{encoding = Svg}` 缝（`LocalDesktopBackend::refresh()` 已证明这条路可行：一条 SVG 即一帧）
- 内容必须来自 `SessionCore::snapshot()`，不能各自维护状态：
  - 每个输入的 id / 类型 / 连接状态 / 是否活跃
  - 有输入但未选择时的提示（「检测到 N 个输入，请选择」）
  - 当前选择模式的提示（自动 / 手动）
  - 丢帧与控制丢包计数（诊断用）
- 刷新时机：源集合或活跃态变化时立即刷新；否则低频心跳刷新（如 1 Hz），避免空转占 CPU
- 输出侧策略：桌面帧只有两种编码选择——SVG（本机/页面直出）或转码后的 H.264（走 CarPlay 输出）。**不允许**把 SVG 当作 CarPlay 画面直接塞进 H.264 通路。

## 7. 生命周期绑定（最容易出错的地方）

| 事件 | 必须做的事 |
|---|---|
| 输入会话建立（`Established`） | `upsert_source(id, External, true)`；建立该输入的有界媒体面 |
| 输入会话结束 | `upsert_source(..., false)`；`end_session()` 清媒体面；**丢弃该输入未发送的帧** |
| 活跃输入切换 | 旧输入媒体面置非选中（停止供给），新输入置选中并**重新等关键帧**（不能沿用旧 GOP） |
| 输出侧断开 | 不回退输入会话，但停止向输出推进；恢复时从关键帧重启 |
| 输出侧要求关键帧 | 透传到当前活跃输入的媒体面（CarLife → 手机；CarPlay → 引擎） |

「切换活跃输入必须重新等关键帧」是本设计里最关键的一条：H.264 的 P 帧依赖前序帧，跨会话复用会直接花屏。

## 8. 待实现清单（按依赖顺序）

1. `Core/Convert`：本文件的音频三流拆分的**输出侧**封装（把媒体面记录翻译成 CarPlay 侧音频流）—— 多声道/多格式/压缩载荷的**承载与转发已完成**（§3.1），待输出侧按 `opaque` 与 `codec` 选择对应编码
2. `Core/Forward`：CarPlay→CarPlay 直通通道（零拷贝/少拷贝，测量复制次数与时延）
3. `Core/Convert`：CarLife→CarPlay 的控制映射（HID 层）与能力门控
4. `Core/MainMenu`：§6 的桌面渲染器（当前只有 `LocalDesktopBackend` 的静态兜底 SVG）
5. 输出侧 `Output/WiredCarPlay`：USB gadget + iAP2 + MFi + transmitter 会话（当前仅骨架）
6. 麦克风上行闭环（§4）
7. 转码支路（§2 判定表里的三档），仅在直通不可行时启用
8. **浏览器侧解码非 PCM**：`opaque` 流的载荷已是 AAC-ELD/ALAC/OPUS 原字节，当前 `Core/Web/assets/app.js` 只按 PCM16 (`getInt16`) 读，需按 `codec` 分派（本层不代劳解码）
9. **多声道播放**：`kMaxAudioChannels=8` 已可透传，但浏览器侧播放器与 AudioWorklet 是否按 >2 声道建，需与 `Core/Web` 对齐（本层不重采样、不下混）
