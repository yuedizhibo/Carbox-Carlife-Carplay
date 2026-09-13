# Zero2W 车载互联桥接固件

> [!IMPORTANT]
> **本项目处于初期阶段，尚不具备可运行能力。** 接口层与协议链路已经实现并有本地测试覆盖，但真实车机后端与手机端到端通路尚未接通，请不要用于实车、量产或生产环境。
>
> **This project is in an early stage and is NOT runnable yet.** Interface layers and protocol paths exist and are covered by local tests, but the real head-unit backend and end-to-end phone connectivity are not in place. Do not use it on a real vehicle, in production, or in any safety-relevant setup.

**中文** | [English](#english)

## 简介

香橙派 Zero2W（Allwinner H618）上的车载互联桥接软件：接收手机侧的无线 CarPlay、无线 CarLife+ 输入，经 Core 统一调度后直通或转换为原车可识别的有线 CarPlay 输出。

## 功能

### 无线 CarPlay 输入
- CPMF（CarPlay Media Protocol）媒体与控制 IPC：主/副屏 H.264、PCM 音频、流生命周期与 9 类车机控制事件。
- 扩展消息接口：媒体元数据、封面、歌词、导航、车辆、电话、通讯录、通话记录、Ducking、SafeArea、日夜模式、帧率、文件传输、激活、内容加密、多会话、辅助触控、VoiceOver、HID 模式、接近传感器与能力声明。
- MFi Auth 3.0 原生工具：证书长度、随机挑战与响应读取；访问前按总线身份校验，拒绝对内部以太网 PHY 所在的 I²C 总线做任何操作。
- Rust sidecar：CarPlay 接入点配置与监管、MFi 安全策略、USB 调试链路保护、preflight 诊断与有界 JSON 控制套接字。

### 无线 CarLife+ 车机端
- 蓝牙发现与引导：HFP 认车机、SPP 上的四步无线握手与 IP 交换。
- 7 条 TCP 通道：会话协商、鉴权四步、19 项能力协商、视频/音频/触控/硬键通道与心跳超时。
- 媒体：H.264 Annex-B 解码显示（默认 1920x1080@30）、PCM 播放与帧快照。
- 控制：单点触控与硬键回传，按车机能力选择多点格式。
- 拓扑：车机侧热点（hostapd + dnsmasq）、UDP 7999 发现、adb 端口转发与本机监听模式。

### 调度（Core）
- 无输入时输出管理桌面，保留状态、测试与配置功能。
- 单个无线 CarPlay 走直通路径，单个 CarLife+ 走转换路径。
- 多输入时输出桌面由用户选择，最多注册 8 个输入。
- 输入断开或故障时清空旧媒体状态，防止上一会话的帧泄漏到新会话。
- 所有队列与缓存固定上限，适合 1 GB 内存设备。

### 输出
- 直通（Forward）：原始 H.264 与 PCM 字节转发，不解码、不重采样、不改声道；会话与流由 SETUP/RECORD/TEARDOWN 回执驱动；手机命令与车机控制双向转发并回传真实执行结果；保留原车麦克风与车机显示/HID 信息用于输入侧协商。
- 有线 CarPlay 输出接口（WiredCarPlay）：C++20 接口层，包含会话状态机、异步请求与执行回执、能力检查、有界双向媒体与麦克风队列、结构化媒体元数据、13 个控制命令与 59 个 iAP2 CSM 消息目录。

### 网页界面
状态与遥测、媒体预览、显示配置、触控与测试接口，9 类控制命令严格表单解析与非法输入拒绝。

### 固件
定制 Linux 内核与设备树、无 initrd 启动、按板载设备裁剪的驱动集、USB 串口/网络调试链路、Wi-Fi 与蓝牙配置。

## 模块

| 目录 | 功能 |
| --- | --- |
| `Input/WirelessCarPlay` | 无线 CarPlay 输入、MFi Auth 3.0 工具、Rust sidecar |
| `Input/WirelessCarLifePlus` | 无线 CarLife+ 车机端 |
| `Core/MainMenu` | 输入注册、自动/手动选择与桌面调度 |
| `Core/Forward` | 无线 CarPlay 到有线输出的直通 |
| `Core/Convert` | 有界媒体记录与协议转换数据面 |
| `Core/Web` | 网页管理、桌面、测试与媒体接口 |
| `Output/WiredCarPlay` | 有线 CarPlay 输出接口 |
| `Linux` | 轻量化 Linux 固件、内核配置与构建脚本 |

数据流、调度规则与模块完成度见 [PROJECT_ARCHITECTURE.md](PROJECT_ARCHITECTURE.md)，接口清单见各模块目录内的 README / API 文档。

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

CarLife+ 输入依赖 SDL2/FFmpeg，默认不参与构建：

```bash
cmake -S . -B build -DBUILD_WIRELESS_CARLIFE_PLUS=ON
```

---

## English

### Overview

In-vehicle connectivity bridge for the Orange Pi Zero 2W (Allwinner H618). It accepts wireless CarPlay and wireless CarLife+ from the phone, schedules both through a common Core, and then forwards or converts them into wired CarPlay that a head unit can recognise.

### Features

#### Wireless CarPlay input
- CPMF (CarPlay Media Protocol) media and control IPC: main/auxiliary screen H.264, PCM audio, stream lifecycle and 9 classes of head-unit control events.
- Extended message interface: media metadata, artwork, lyrics, navigation, vehicle data, telephony, contacts, call log, ducking, safe area, day/night, frame rate, file transfer, activation, content encryption, multi-session, assistive touch, VoiceOver, HID mode, proximity and capability reporting.
- Native MFi Auth 3.0 tool: certificate length, random challenge and response retrieval; the bus identity is verified before any access and the I²C bus carrying the on-board Ethernet PHY is refused unconditionally.
- Rust sidecar: CarPlay access-point configuration and supervision, MFi safety policy, USB debug-link protection, preflight diagnostics and a bounded JSON control socket.

#### Wireless CarLife+ head unit
- Bluetooth discovery and bootstrap: HFP vehicle identification, four-step handshake over SPP and IP exchange.
- Seven TCP channels: session negotiation, four-step authentication, 19 capability items, video/audio/touch/hard-key channels and heartbeat timeouts.
- Media: H.264 Annex-B decode and display (1920x1080@30 by default), PCM playback and frame snapshots.
- Control: single-point touch and hard keys sent back to the phone, multi-touch format chosen from the negotiated capabilities.
- Topologies: head-unit hotspot (hostapd + dnsmasq), UDP 7999 discovery, adb port forwarding and local listening mode.

#### Scheduling (Core)
- With no input, an administration desktop is shown and status, test and configuration functions remain available.
- A single wireless CarPlay source takes the forwarding path; a single CarLife+ source takes the conversion path.
- With several inputs, the desktop lets the user choose; up to 8 inputs can be registered.
- When an input disconnects or fails, stale media state is cleared so frames from the previous session cannot leak into the next one.
- Every queue and buffer is bounded, which suits a 1 GB device.

#### Output
- Forwarding: raw H.264 and PCM byte forwarding without decode, resample or channel change; sessions and streams are driven by SETUP/RECORD/TEARDOWN receipts; phone commands and head-unit controls are forwarded in both directions with real execution results; vehicle microphone and head-unit display/HID information are kept for input-side negotiation.
- Wired CarPlay output interface (WiredCarPlay): a C++20 interface layer with a session state machine, asynchronous requests and execution receipts, capability checks, bounded bidirectional media and microphone queues, structured media metadata, 13 control commands and 59 iAP2 CSM message IDs.

#### Web interface
Status and telemetry, media preview, display configuration, touch and test endpoints, with strict form parsing and rejection of invalid input for 9 classes of control commands.

#### Firmware
Customised Linux kernel and device tree, boot without initrd, a driver set trimmed to the on-board devices, USB serial/network debug links, and Wi-Fi/Bluetooth configuration.

### Modules

| Directory | Function |
| --- | --- |
| `Input/WirelessCarPlay` | Wireless CarPlay input, MFi Auth 3.0 tool, Rust sidecar |
| `Input/WirelessCarLifePlus` | Wireless CarLife+ head unit |
| `Core/MainMenu` | Input registration, automatic/manual selection, desktop scheduling |
| `Core/Forward` | Forwarding from wireless CarPlay to the wired output |
| `Core/Convert` | Bounded media recording and protocol conversion data path |
| `Core/Web` | Web administration, desktop, test and media endpoints |
| `Output/WiredCarPlay` | Wired CarPlay output interface |
| `Linux` | Lightweight Linux firmware, kernel configuration and build scripts |

Data flow, scheduling rules and per-module completeness are described in [PROJECT_ARCHITECTURE.md](PROJECT_ARCHITECTURE.md); interface listings live in the README/API documents inside each module.

### Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

The CarLife+ input needs SDL2/FFmpeg and is not part of the default build:

```bash
cmake -S . -B build -DBUILD_WIRELESS_CARLIFE_PLUS=ON
```
