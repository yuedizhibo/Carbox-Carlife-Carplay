# Zero2W 车载互联桥接固件

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
