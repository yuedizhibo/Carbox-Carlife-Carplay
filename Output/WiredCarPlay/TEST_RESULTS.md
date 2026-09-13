# Output 接口验证记录

日期：2026-09-13。环境：Windows PowerShell + WSL Debian，GCC 14.2.0，CMake 3.31.6。

## 已运行且通过

| 检查 | 实测结果 |
|---|---|
| PowerShell 逐项参考对照 | 59/59 CSM 名称、ID、家族；13/13 命令名称、方向；11/11 注释扩展 |
| 独立 Debug 构建 / CTest | 2/2；Output 1167 条检查，catalog 722 条检查 |
| 独立 Release 构建 / CTest | 2/2；测试使用显式检查，不受 NDEBUG 影响 |
| AddressSanitizer + UndefinedBehaviorSanitizer | 2/2，无报告；`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie`，链接 `-no-pie` |
| 根项目回归（启用 CarLife 与 Output） | 16/16，通过；原 14 项加 Output 2 项，13.22 秒 |
| 独立构建依赖检查 | 仅标准 C++20 / Threads；没有链接 Core、Convert、Input、Rust 或 Reference |

测试覆盖：启动受理与真实回执的分离、启动鉴权错误、进度上报、重复/旧 epoch 回执、流建立失败不生效、SETUP/RECORD/TEARDOWN 生命周期、模式请求、全 13 个控制命令双向规则、全 59 个 CSM 原始参数往返、全部 31 个服务通道、20 组结构化服务示例、类型不匹配和非法值、31 个音频格式位往返、车机麦克风、媒体/事件/请求背压、完成结果容量限制、预留 shutdown 槽、应答超时、取消/断开清理、失效能力拒绝和双线程媒体收发。

PiAgent（DeepSeek Flash）在明确工作目录和独立 session 下完成了 catalog 头文件、catalog C++ 测试、PowerShell 参考核对脚本；已由 Codex 独立审查、编译和执行。本轮没有使用 worker 报告代替验证结果。

## 未执行 / 未实现

- 没有连接车机、切换 USB、修改 Zero2W 服务、MFi I²C 或现有 USB 网络。
- 没有验证真实 USB 枚举/角色切换、鉴权、iAP2 会话、RTSP/RTP、音视频在车机播放、车机麦克风或反控。
- 没有把 Output 接到 Core/Convert/Input；没有新增音视频编码器或跨协议语义转换。
- 测试夹具直接调用后端端口产生 PeerInfo/回执，只证明接口契约与状态机，不能证明 CarPlay 协议可用或授权/认证通过。

复现命令见 README；真实后端责任和当前边界见 API.md。
