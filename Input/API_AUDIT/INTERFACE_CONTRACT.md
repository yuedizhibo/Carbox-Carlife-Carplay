# 两个 Input 的对接契约

范围依据用户最新确认：只核对 Input 接口，不做 WiredCarPlay 输出、USB 角色切换或真车验收。`true`/HTTP `accepted` 表示通过当前层的解析或排队，不表示手机/车辆已执行。

## 公共边界

| 边界 | 接口/数据 | 约束 |
| --- | --- | --- |
| Core → Input | `ControlEvent`、`register_control_sink`、`route_control` | 9 类事件；源选择后路由；回调不等于设备执行确认 |
| Input → Core | 媒体/音频/显示/交互/导航/车辆/电话/连接状态 setter | 状态面与媒体字节面分离 |
| Input → 媒体面 | `RealMediaStore`、会话/视频流/音频流生命周期 | 有界存储，旧会话拒绝；音频格式取描述符，不固定假报 PCM |
| HTTP → Core | `POST /api/control` | form ≤4096 B、≤16 字段；重复/未知字段、NUL、溢出、非法转义拒绝 |
| CarLife 宿主接口 | `HostSink`、`HostControl`、`VehicleReport`、`MicrophoneSource` | 回调同步、有界；提供方从车机/后续转换层取得数据，不要求本地麦克风/CAN |
| CarPlay 引擎接口 | CPMF v1，64 B 大端头 | 扩展控制带当前非零 session、单调 sequence；分包/粘包按声明长度读取 |

## Core 控制的映射

| ControlEvent | HTTP type | CarLife | CarPlay 引擎 |
| --- | --- | --- | --- |
| Touch | touch | TOUCH_ACTION_* / ACTION_3 | 0x8002 → 主屏 HID |
| Key | key | CarLife 专用键码 | 0x8010 → Media/Telephony/Knob HID，非 Android 键码 |
| MultiTouch | multitouch | 最多10点；按能力选择线格式 | 0x8011 → 2个 HID 触点槽，id 0/1；不代表10点端到端支持 |
| Knob | knob | 方向转重复键，最多32次，整批排队；Press一次 | 0x8012 → 4 B KnobReport，绝对步数≤127，Press一次 |
| Gesture | gesture | 支持的触摸板语义；非任意手势解释器 | 0x8013 → select/down/up、home/back、方向、wheel_left/right；其他拒绝 |
| Proximity | proximity | 协议无对应上行，拒绝 | 0x8014 → Proximity HID |
| Voice | voice | VR_START / VR_STOP | 0x8015 → Siri ButtonDown / ButtonUp / Prewarm |
| Telephony | telephony | 接听/挂断及逐位数字、*、#；拨号末尾追加PHONE_CALL且整批入队；未映射操作不声称执行 | 0x8016 → 接听/挂断/拨号/DTMF/静音；hold未映射 |
| VehicleCtrl | vehicle | 车控消息为手机→车机，不能反向编造 | 0x8017 → night_mode / limited_ui；其他车控拒绝 |

CarPlay 键名：play、pause、play_pause/playpause、next、prev、ac_nav 及 media.* 别名；tel.up/hook/flash/drop/mute、tel.0..9/star/pound/delete；select/enter/home/back/left/right/up/down/wheel_left/wheel_right。字符串与数字键码的边界测试独立于手机行为。

CarLife 专用 API：`requestForeground()` → CMD `0x18025` 空载荷；`requestModuleControl(module,status)` → CMD `0x18028` singular protobuf（module1/status2 为 `08 01 10 02`，不是列表）；`Session::requestFrameRate`；`setVerifySeam`；`setMicrophoneSource`；`setMicPrepareCallback`；`setVehicleReportSource`；`setMultiTouch`；`setAppEventHandler` / `onAppEvent`（前后台、屏幕开关、桌面、用户在场和返回前台请求/应答8类通知）。配置型回调在 start 前安装。

## CarPlay 全部本地扩展下行（25类）

以下编号是本项目 CPMF 扩展，不能把每个编号当成 LIVI 已提供的独立功能。

| 类型 | 接口落点 | 引擎生产者 |
| --- | --- | --- |
| 0x30 Metadata | 媒体信息/进度/状态 | 新增真实 iAP2 NowPlaying 订阅 |
| 0x31 ArtworkChunk | 有界严格组包 | 新增匹配 artwork transfer id 的完成文件，≤128 KiB |
| 0x32 LyricsChunk | 有界严格组包 | 预留，未接真实歌词来源 |
| 0x33 MediaLibrary | 库状态 | 预留，不是完整浏览事务 |
| 0x34 Navigation | 导航结构 | 预留，未接真实导航来源 |
| 0x35 Vehicle | 车辆结构 | 预留，车辆来源属于后续回传层 |
| 0x36 Telephony | 来电/通话状态 | 新增真实 iAP2 CallState 订阅；单条状态投影，不是完整多通电话模型 |
| 0x37 ContactsChunk | 联系人数量 | 预留，未保留全部联系人数据 |
| 0x38 CallLogChunk | 通话记录数量 | 预留，未保留全部记录数据 |
| 0x39 Ducking | 闪避状态 | 音频增益已有基础0x23路径；此语义扩展预留 |
| 0x3A SafeArea | 屏幕边界 | 来自主屏配置 offer，不是已协商确认 |
| 0x3B AuxPlane | 副平面状态 | 预留；基础主/副屏媒体流独立存在 |
| 0x3C Calibration | 校准状态 | 预留 |
| 0x3D DayNight | 昼夜状态 | 下行预留；night_mode 上行命令已映射 |
| 0x3E FrameRate | 帧率状态 | 扩展预留；基础视频配置携带帧率 |
| 0x3F FileTransfer | 文件进度 | 状态预留，不是文件交付 |
| 0x40 Ota | OTA 状态 | 宿主职责，未接安装事务 |
| 0x41 Activation | 激活状态 | 预留，不生成 token |
| 0x42 ContentEncryption | 加密状态 | 状态预留；不代替实际 CarPlay 加密会话 |
| 0x43 MultiSession | 会话列表 | 预留，未接手机设备轮转 |
| 0x44 AssistiveTouch | 辅助触控状态 | 预留 |
| 0x45 VoiceOverState | 读屏状态 | 预留 |
| 0x46 HidModeState | HID 模式 | 预留 |
| 0x47 ProximityState | 接近状态 | 下行预留；上行 HID 已映射 |
| 0x48 Capability | 能力状态 | 真实引擎 offer：2点、旋钮、Siri 控制；不是麦克风就绪 |

上行全部11类：0x8010–0x8017 见控制映射表；0x8018 HidModeSet、0x8019 VoiceOverSet、0x801A AssistiveTouchSet 仍为本地协议预留，未接引擎动作。不将其广告为已支持。

## 仍不能标成“全部接入”的接口差异

- CarPlay 预留扩展与真实生产者的差异如上；电话增量/多通模型、完整通讯录/媒体库/歌词/导航接口仍未贯通。
- CarLife 文件/OTA/激活、部分HFP与车控接口目前有解析或回调，不是完整执行事务。Android Activity、USB/AOA 和 Electron/dongle APIs 不属于这两个无线 Input 的等价宿主接口。
- C++ 的 Core 路由接受和引擎执行结果尚无逐请求 ACK；不能把 HTTP 200 当执行确认。
- CarPlay 当前仅在状态发布时检查活跃源，切源检查与 setter 不是原子事务；完整多源状态重放/隔离仍需统一 Core 状态设计。

完整来源名称、文件、行号及数值见 inventory.md/json；LIVI 所有70个 preload 成员见 LIVI_API_STATUS.md。上述差异显式保留，不以补常量或模拟包掩盖。
