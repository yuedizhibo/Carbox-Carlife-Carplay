# Forward

无线 CarPlay 到有线 CarPlay 的低损耗直通层。

已实现 Core 直连协调器与现有无线输入的 CPMF 接入。代码可运行并有端到端本地测试，但**不等于已经具备真机完整直连**：有线 Output 仍无真实协议后端，现有无线 CPMF 引擎也缺少通用控制 ACK / 原车麦克风 IPC。没有通过静音、固定成功应答或改动 USB 来掩盖这些缺口。

## 已接通的 Core 路径

| 方向 / 功能 | 实现 |
|---|---|
| 无线媒体 → 有线 Output | 原始 H.264、协商兼容的 PCM/ALAC/AAC/Opus 字节转发，不解码、不重采样、不改声道 |
| 会话 / 流 | Core 当前源选择、输入 session/generation 与 Output epoch 绑定、主/副屏与多音频流、SETUP / RECORD / TEARDOWN 回执驱动 |
| 手机命令 → 车机 | 原生端口接受 SendControl / SendIap2 / SendService / AssertModes，转发真实 Output 执行结果，不把入队算成功 |
| 车机命令 → 手机 | HID report/UUID、Siri、UI、夜间/限制 UI、关键帧、changeModes、iAP 原样传递，并将手机后端应答关联回车机 |
| 麦克风 → 手机 | 原生端口按流 ID、代号和时间基转发原车麦克风；未声明真实能力时拒绝 |
| 车机信息 → 输入协商 | 输出成功启动后提供原车显示/尺寸/HID 描述符/原始 info/能力；输入尚未 begin 时也可读取；有线断开后撤销 |
| 元数据 / iAP2 | 原生载荷类型与原始参数保持不变；CPMF 扩展使用 `zero2w.cpmf.v1` schema 保留完整头和载荷 |
| 背压 | 默认输入媒体 64 包/8 MiB，麦克风 64 包/256 KiB，请求及完成结果各方向合计 32；Output 背压时最多额外暂存一包每方向 |
| 隔离 / 恢复 | 切源、断线和重配清除旧队列；输出流 ID 单调分配不复用；视频从关键帧恢复；挂起请求取消/超时保留错误 |

实际无线引擎当前产出的音频仍主要是 PCM。支持已协商压缩格式的转发代码及测试，不表示引擎已产出所有这些 codec。

## 组成

- `WirelessEndpoint`：线程安全、有界的输入端口。支持媒体/流声明、双向请求/应答、麦克风和车机信息交换。
- `CarPlayForwarder`：单线程执行的 Core 协调器。`tick()` 处理状态、建立/重配流、转发控制和媒体；`stop()` 后继续 tick 到 Inactive；`start()` 可重新启用。不负责启动 USB。
- `CatPlayForwardCapture`：接收现有 `CatPlayMediaClient` 的原始记录订阅，在网页预览存储裁剪之前接入。检查握手、会话、流、配置代号、序号、格式和关键帧；断开即失效。
- `CarPlayDirectConnection`：现有 Input + Core + Output 的装配类，安装订阅入口并持有上述对象。

## 现有输入接线

```cpp
#include "forward/direct_connection.hpp"

// core、client、output 必须活得比 connection 久。
// output 的真实后端另行驱动；这里不虚构 peer 或成功回执。
zero2w::forward::CarPlayDirectConnection connection(core, client, output, configured_fps);
client.start();
// Core 执行线程周期调用（例如每 2~5 ms）。不要从多个线程同时 tick。
connection.tick();

// 关闭时：先 stop，再继续 tick + 驱动 Output 后端，直到 Inactive。
connection.stop();
connection.tick();
```

构造必须在 `client.start()` 前；晚装观察者不会伪造或补放错过的 SessionBegin，需要等新的连接/快照。此装配类独占客户端的单一 observer 槽和 Output 的媒体/事件消费；不要同时装配其他控制协调器。没有改 Web 主程序自动创建无后端的假输出，也没有启用新的 systemd 服务。

`configured_fps` 必须与无线输入实际配置一致：CPMF 本身不携带协商帧率。显示 UUID 缺省时按主屏/副屏选用车机 `/info` 中的显示，尺寸/codec/帧率不兼容时明确失败，不偷偷缩放或转码。

## 完整原生输入后端接线

新的输入引擎适配器可不经过 CPMF Capture，直接使用 `WirelessEndpoint`：

1. 读取 `vehicle_info()`，将车机真实显示与 HID 描述符用于手机侧协商；不能仅复制报告却使用另一套 HID 描述符。
2. `begin(session, reverse_capabilities)` 声明后端真正支持的反向命令、iAP2、服务和麦克风；`set_stream` / `remove_stream` 发布协商后的流。
3. `push_media` 发布拥有字节的媒体包；手机命令用 `request_output`，手机后端消费 `pop_phone_completion` 并应答手机。
4. 消费 `take_reverse()`，实际执行后调用 `complete_reverse`；`take_microphone(generation)` 取原车音频。不要消费后立即伪造成功。
5. 输入断线调用 `end()`；后端必须停掉旧 generation 的已经领取任务。Core 的选择/清理在下次 tick 生效；已经交给外部后端的包也须由后端废弃。

上述原生端口的控制与麦克风往返已用显式后端测试夹具验证。旧 CPMF Capture 的 reverse_capabilities 是空集合，**不会声称旧引擎已经实现这条返回路径**。

## 时间基与“直通”的含义

保留应用媒体载荷，不是把两条链路的 USB/RTSP/RTP 加密包直接复制。两侧仍须各自进行握手、包化、加密和时间同步。

- 原生媒体的 `StreamClock` 时间戳保持其流时钟语义。
- 现有 CPMF 的 pts 是主机 `CLOCK_MONOTONIC` 微秒（音频由引擎在发布时生成，不能冒充原手机 RTP 样本计数）。Capture 标记 `MonotonicMicroseconds`，不把它误写成 NTP。
- Output 后端只有能够将该主机时间基映射到车机时钟，才可声明 `monotonic_media_timestamps=true`。否则返回 NotSupported。媒体中的数字和时间基原样保留，映射属于传输后端。
- 这是有界的少拷贝路径，不宣称零拷贝：Capture 将借用 span 拷入自有队列；协调器为可重试背压保留一包副本；没有解码/编码成本。

## 故障约定

- SETUP 或 RECORD 尚未 ACK 不送媒体，能力不符不降级伪装。
- 流配置改变：关闭旧输出流，再建立新流；同 ID 新配置的旧排队媒体被清除。
- 切源：取消双向挂起请求，清除输出/输入媒体及麦克风，异步关闭本协调器拥有的流，不停止其他输入。
- 关闭期间已领取的状态操作被取消时，Output 状态会进入 Failed，要求真实后端回收传输；不是断言远端操作从未发生。
- 销毁前应正常 drain。若调用者直接销毁仍有流的 Forwarder，会使 Output 失效，防止留下继续播放的孤立流，但物理 USB 关闭仍由后端负责。
- 无人应答的车机命令按 deadline 超时；当前没有被选中的无线输入时，不把车机命令发到后台手机。

## CPMF 兼容范围 / 未完成后端

现有接入可转发主/副屏 H.264、经过严格格式匹配的音频、AudioGain（线性幅度 ppm 换为协议 dB 的 duck/unduck）以及原始扩展消息。扩展 schema 必须由输出后端明确实现，不能直接把 CPMF 的本地消息号当 iAP2 消息号。声明为浮点、填充容器、不明字节序、超出 Output 支持范围的音频会拒绝，而不是当成 PCM16。

仍需协议后端工作的部分：

- 有线 Output 的 USB/iAP2/RTSP/RTP/MFi 真实执行引擎（上一阶段尚未实现）。
- 无线引擎的通用控制结果、原始 iAP2/HID 双向 IPC、原车麦克风替代测试静音，以及将 `vehicle_info` 用于实际手机协商。
- 原始 CPMF 扩展 schema 到车机线协议的编码。保留消息不等于所有元数据功能已可在车机显示。

本轮没有修改 Reference/外部 Rust 引擎、没有接 CarLife 转换、没有部署、没有切换 USB。

## 构建 / 测试

根 CMake 默认在启用 Output 时启用 `BUILD_CARPLAY_FORWARD`。关闭 Output 时也应关闭 Forward。

```sh
cmake -S . -B Temp/input-audit-build -DBUILD_WIRELESS_CARLIFE_PLUS=ON
cmake --build Temp/input-audit-build -j 4
ctest --test-dir Temp/input-audit-build --output-on-failure
```

验证记录见 `TEST_RESULTS.md`。测试成功只证明 Core/接口链路，不证明真实车机已经连接。
