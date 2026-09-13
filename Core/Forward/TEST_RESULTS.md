# Core CarPlay 直连验证记录

日期：2026-09-13。环境：Windows + WSL Debian / GCC 14.2 / CMake 3.31.6。

## 结果

- `carplay_forward_tests`：225 条显式检查通过，Release 不会因 NDEBUG 关闭检查。
- 根项目 CTest（CarLife、MFi、Output、Forward 启用）：17/17 通过。
- Release：Forward / Output / catalog 3/3 通过。
- ASan + UBSan：Forward / Output / catalog 3/3 通过，无 sanitizer 报告。编译 `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie`，链接 `-no-pie`。

## 端到端与边界覆盖

1. 真实 Unix socketpair 分片写入 CPMF，经 CatPlayMediaClient 解析 → 原始观察者 → WirelessEndpoint → Core 选择 → Output 流和媒体队列。
2. H.264 字节、配置、PTS 保留，AAC opaque 包不再被提前强制按 PCM16 拒绝；时间基明确标记 MonotonicMicroseconds。
3. Output 不支持单调时钟映射时拒绝该时间基；不把它冒充 NTP 或音频样本计数。
4. 主屏/音频 SETUP 完成后再 RECORD，失败回执不产生假活跃流；配置变化关闭旧流并从关键帧恢复。
5. Core 切源、手动选择、stop/start、输入断开、有线输出重启与旧 epoch/generation 隔离。
6. 有界背压保持帧序，暂停取输入并重试暂存包；切源清理输出/输入旧媒体和麦克风。
7. 手机命令经 Output 后端明确应答后才返回结果；错误码和响应字节保留。启动者回执不被 Forward 抢走。
8. 车机 HID UUID/report/时间戳、Siri/UI/关键帧/changeModes/夜间/限制 UI/iAP 命令与原始参数，经输入后端执行回执关联回车机。
9. 原生麦克风流 ID 双向映射、字节和时间戳保留；现有 CPMF 引擎不具备该能力时不声明。
10. 车机 PeerInfo 传到输入协商端，包含显示信息；有线断开后失效。
11. 结构化 NowPlaying 和原始 iAP2 参数、反向请求超时、非法端口操作与容量上限。
12. `CarPlayDirectConnection` 装配类接入、等待真实 Output 就绪、流启动、媒体转发和正常 drain。

## 实现来源与未验证项

PiAgent（DeepSeek Flash）按限定合同仅增加了 Input 原始记录观察者（新头文件、客户端声明和通知点）。指定了工作目录、独立 session、五分钟外部超时。所有 Core 转发设计/代码、Output 接缝、解析入口修复和测试由 Codex 实施并独立验证。测试过程中出现过一次在链接尚未结束时启动 CTest 的 `text file is busy`；等待构建结束后重跑通过，这不是运行中的产品错误。

现有编译器会报告共享代码已有的 `decode_audio_format` 未使用参数，以及根项目其他已有 warning；没有把这些无关代码改动混入此次任务。

没有真机 CarPlay 输出验收，没有启用 USB gadget/角色切换，没有更改 Zero2W SSH/服务/MFi，没有部署 Rust 外部引擎。

Output 协议引擎尚未实现；无线旧 CPMF 后端仍没有通用控制 ACK、原车麦克风返回和实际车机 PeerInfo 协商入口。双向控制/麦克风的 Core 行为由显式测试后端验证，不能据此宣称这些后端缺口已经补齐。完整接线边界见 README。
