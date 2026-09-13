# 项目架构与实现状态

## 1. 目标

设备在香橙派 Zero2W 上运行轻量化 Linux，并向原车提供一个有线 CarPlay 外设。手机侧输入可以是无线 CarPlay、无线 CarLife+，以后通过统一输入接口扩展。Core 负责输入发现、会话调度、网页管理、诊断和无输入/多输入桌面。

## 2. 数据流与调度

```text
无线 CarPlay ──┐                 ┌─ 直通 Forward ─────┐
无线 CarLife+ ─┼─ InputAdapter ─ Core 调度             ├─ WiredCarPlay ─ 原车
未来输入 ──────┘                 ├─ Convert 转换 ─────┤
无/多输入 ───────────────────────└─ MainMenu 桌面 ────┘
```

调度原则：

1. **无输入**：输出 Core 桌面，用户仍可使用管理、状态和测试功能。
2. **单个无线 CarPlay 输入**：走 `Forward`，尽量转发原始控制、H.264 和音频数据，不做不必要的解码/重编码。
3. **单个 CarLife+ 输入**：走 `Convert`，把 CarLife+ 的会话、控制、视频和音频语义映射为输出侧 CarPlay 会话。
4. **多个输入**：输出 Core 桌面，用户手动选择；选择完成后按输入类型进入直通或转换路径。
5. **故障/断开**：清空旧输入的媒体状态并回到桌面或下一个可用输入，防止旧帧泄漏到新会话。

`Core/MainMenu` 中现有 `SessionCore` 已实现最多 8 个输入、自动/手动选择、活跃输入解析、有界视频/音频缓存和反向控制路由。当前优先级是 External > Synthetic > LocalDesktop。它还是调度骨架，并非完整桌面渲染器。

## 3. 模块状态

| 模块 | 当前状态 | 说明 |
|---|---|---|
| WirelessCarPlay C++ 接口 | 已有 | 输入适配器、媒体 IPC 客户端及测试 |
| WirelessCarPlay Engine | 已有实验实现 | 独立 Rust 进程，管理 AP/MFi/外部 CatPlay；通过 IPC 与 C++ 隔离 |
| WirelessCarLifePlus | 已接入 Core | 7 TCP 通道、蓝牙引导、会话协商、H.264/PCM、触控；CarLifeInputAdapter 接入 SessionCore / RealMediaStore，细项边界见 Input/API_AUDIT |
| MainMenu/调度 | 部分完成 | 有选择和控制路由；桌面 UI 目前主要在 Web 入口中 |
| Convert | 部分完成 | 有有界媒体协议/存储；CarLife+→CarPlay 语义转换尚未完成 |
| Forward | Core 链路已实现 | 无线 CPMF 原始媒体接入、ACK 驱动的流生命周期、选择/背压/断线隔离、双向控制与麦克风原生端口；实际输入返回 IPC / 有线协议后端仍待实现，详见 Core/Forward/README.md |
| Web | 已有 MVP | 状态、媒体、显示配置、触控与测试接口；仍需从单体 `main.cpp` 解耦 |
| WiredCarPlay 输出 | 接口层已实现 | 独立 C++20 库、状态机、有界双向媒体/控制、结构化元数据、13 命令/59 CSM 目录及测试；未接 Core/Convert，真实 USB/iAP2/RTSP/RTP 后端未实现，详见 Output/WiredCarPlay/API.md |

## 4. 关键边界

- 输入、Core、输出之间使用明确的会话/媒体/控制接口，禁止模块直接持有对方实现细节。
- 视频优先保留 H.264 Annex-B，直通路径时不解码；只有协议或分辨率不兼容时才转换。
- 所有队列和缓存必须有固定上限，避免 1 GB 内存设备上产生不可控增长。
- MFi I²C 总线必须先确认控制器身份；内部 AC200 与外部 MFi 可能同为 `0x10`，严禁仅凭地址访问。
- Reference 只用于互操作研究。许可证或来源不明的代码/固件不得进入产品构建和发布。

## 5. 下一阶段顺序

1. 定义稳定的 `InputAdapter`、`OutputAdapter`、媒体和控制事件 ABI。
2. 完善两个已接入 Input 的接口一致性、状态隔离及剩余预留接口；当前阶段不要求输出实车验收。
3. 实现 `Output/WiredCarPlay` 最小会话和 USB gadget/MFi 通路。
4. 实现 WirelessCarPlay→WiredCarPlay 的直通，并测量复制次数、CPU 和时延。
5. 实现 CarLife+→CarPlay 的控制、视频、音频和生命周期映射。
6. 抽离 MainMenu 桌面渲染与 Web 管理服务，补齐无输入/多输入实机流程。
7. 将 Core 和输出服务纳入 Linux 启动关键路径，持续用 UART/systemd 数据验证 10 秒目标。
