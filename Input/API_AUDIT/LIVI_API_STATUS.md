# LIVI preload 的 70 个公开成员

来源：`Reference/LIVI/src/preload/index.ts`；逐个成员的行号、IPC channel、main-process 注册位置见 [inventory.md](inventory.md)。本表按功能合并行，但每个暴露成员都列名。

本项目没有提供 `window.projection` / `window.app` 的 Electron 兼容层。“有近似底层能力”不表示同名 API 已移植。dongle、Android Auto、桌面应用管理与原生无线 CarPlay 必须分开，不可静默排除后宣称 LIVI 全部覆盖。

## projection（46 个可调用成员）

| API（同组以逗号分隔） | 本项目对应状态 |
| --- | --- |
| `projection.quit` | 未移植 Electron 退出 API；本项目是服务进程 |
| `projection.onUSBResetStatus` | 未移植 USB reset 事件订阅 |
| `projection.usb.forceReset` | 未接 dongle reset |
| `projection.usb.detectDongle` | 未接 dongle 探测 |
| `projection.usb.getDeviceInfo` | 未接同等 dongle 信息查询 |
| `projection.usb.getLastEvent` | 未接同等 USB 事件快照 |
| `projection.usb.getSysdefaultPrettyName` | 未接系统麦克风友好名称查询 |
| `projection.usb.uploadIcons` | 未接 dongle 图标上传 |
| `projection.usb.uploadLiviScripts` | 未接 dongle 脚本上传 |
| `projection.usb.listenForEvents` | 未接 USB 事件队列/订阅 |
| `projection.settings.get`, `projection.settings.save`, `projection.settings.onUpdate` | 有项目自身配置/状态机制；未提供 LIVI Config 及订阅兼容语义 |
| `projection.audio.listSinks`, `projection.audio.listSources` | 未移植音频设备枚举 API；麦克风源注入不是设备枚举 |
| `projection.ipc.start`, `projection.ipc.stop`, `projection.ipc.restart` | Input/Engine 有生命周期控制；无同名 Electron API |
| `projection.ipc.setVisible` | 未贯通等价可见性/投屏抑制动作 |
| `projection.ipc.sendFrame` | 有关键帧请求路径；不是该 API 已兼容的证明 |
| `projection.ipc.setBluetoothPairedList` | 未移植 paired-list 设置协议 |
| `projection.ipc.connectBluetoothPairedDevice` | 有蓝牙连接相关底层代码，未移植该 API / 完整语义 |
| `projection.ipc.forgetBluetoothPairedDevice` | 未移植忘记配对设备事务 |
| `projection.ipc.dongleFirmware` | 未移植 dongle 固件 check/download/upload/status |
| `projection.ipc.switchTransport` | Core 选择 Input 不等于 LIVI dongle/AA/CP transport 切换 |
| `projection.ipc.getTransportState` | 有 Core/Engine 状态，非 LIVI TransportSnapshot 兼容实现 |
| `projection.ipc.getDevices` | 有输入源列表，不等于已配对手机设备库 |
| `projection.ipc.selectDevice` | 源选择不等于手机设备切换，缺等价事务 |
| `projection.ipc.cycleSession` | 未完成 LIVI 等价手机会话轮转 |
| `projection.ipc.forgetDevice` | 未完成 LIVI 等价忘记设备事务 |
| `projection.ipc.sendTouch` | 基础 CPMF Touch 已有 C++/input-only 引擎路径；仍需手机互操作验收 |
| `projection.ipc.sendMultiTouch` | C++ 扩展与真实引擎双点HID已对接；不是10点端到端能力 |
| `projection.ipc.sendCommand` | 已映射媒体/电话/HID、方向/旋钮、Home/Back/选择/滚轮、Siri及昼夜/受限UI；具体子命令与拒绝边界见 INTERFACE_CONTRACT.md |
| `projection.ipc.onEvent` | 有本项目状态查询；未实现 LIVI 事件订阅 API |
| `projection.ipc.readMedia` | C++ 扩展媒体状态可读；已新增真实iAP2 NowPlaying/封面生产者，非完整媒体库API |
| `projection.ipc.readNavigation` | C++ 扩展导航状态可读；真实引擎生产者缺失 |
| `projection.ipc.onAudioChunk`, `projection.ipc.offAudioChunk` | 有媒体缓冲/HTTP 流，不是 Electron 音频块订阅 API |
| `projection.ipc.setVolume` | Core 音频状态不等于四路音量在真实音频后端已生效 |
| `projection.ipc.setVisualizerEnabled` | 未移植可视化开关 |
| `projection.ipc.requestCluster` | 有双屏/流角色相关代码；未完成等价 API 与手机协商验收 |
| `projection.ipc.clusterRepaintNudge` | macOS 窗口 repaint workaround，非 Linux Input 功能；未移植 |
| `projection.ipc.onClusterResolution` | 有视频流描述，不是该事件重放/订阅 API |
| `projection.ipc.onTelemetry`, `projection.ipc.offTelemetry`, `projection.ipc.getTelemetrySnapshot` | 有本项目遥测/状态读取；未提供 LIVI telemetry API 兼容层 |

## app（22 个可调用成员 + 2 个属性）

以下是完整应用宿主 API，而非手机协议 API。当前没有移植 LIVI Electron 宿主层；未移植不意味着应当把桌面 API 塞进 Input 协议实现。

| API | 状态 / 职责 |
| --- | --- |
| `app.platform`, `app.compositor` | 两个环境属性；未提供同名全局对象 |
| `app.getVersion` | 应用版本查询，未移植同名 API |
| `app.listDisplayModes` | 宿主显示模式枚举，未移植同名 API |
| `app.listWifiChannels` | 宿主 Wi-Fi 信道枚举，未移植同名 API |
| `app.listWifiCountryCodes` | 宿主地区码枚举，未移植同名 API |
| `app.listWifiInterfaces` | 宿主 Wi-Fi 接口枚举，未移植同名 API |
| `app.listBtAdapters` | 宿主蓝牙适配器枚举，未移植同名 API |
| `app.getLatestRelease` | 应用更新查询，未移植 |
| `app.performUpdate` | 应用镜像更新，未移植；协议 OTA 状态不是此实现 |
| `app.onUpdateEvent`, `app.onUpdateProgress` | 更新事件/进度订阅，未移植 |
| `app.resetDongleIcons` | dongle 图标配置复位，未移植 |
| `app.beginInstall`, `app.abortUpdate` | 应用安装/中止更新，未移植 |
| `app.customPageUrl`, `app.customIconUrl` | 自定义页面/图标地址查询，未移植 |
| `app.quitApp`, `app.restartApp` | 桌面应用退出/重启，未移植同名 API |
| `app.openExternal` | 宿主打开外部 URL，未移植 |
| `app.notifyUserActivity` | 用户活动事件，未移植 |
| `app.reportPath` | UI 路由报告，未移植 |
| `app.broadcastMediaKey`, `app.onMediaKey` | 应用内媒体键广播/订阅，未移植；不等于手机媒体键已通 |

## 原生 CP 控制再细分

LIVI `CpSession` / `CpStack` 还包含不直接暴露于 preload 的公开方法，完整候选清单单列在索引中，不用上面的 70 个成员代替它。

- 触控：单点、多点、显示参数和流选择。
- 媒体：play / pause / playPause / next / prev。
- 旋钮/方向：旋转、上下左右、选择按下/松开、back / home / wheel。
- 电话：接听/拒接、数字 0–9、星号/井号、hook。
- 语音：Siri；参考中的 release 分支本身也可能无动作。
- 显示：昼夜、主屏/cluster 关键帧、显示配置、active stream。

上述类别在本项目并非全部完成真实引擎映射，详见 [主报告](README.md)。`InputCommand` 有 15 个枚举项，但参考 `handleInput` 并未映射其中全部项；stop、快进/快退、音量增减、静音不能单凭枚举声明判为原生 CP 已支持。
