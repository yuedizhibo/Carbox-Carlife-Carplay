// Core 自己的管理桌面。
//
// 定位：它不是「又一个输入」，而是 Core 在**无输入**与**多输入未选择**时自己产出的显示面。
// 内容全部来自 SessionCore::snapshot()，不自行维护并行状态；产出一帧 SVG
// （复用既有 VideoFrame{encoding=Svg} 缝，与 LocalDesktopBackend 的兜底帧同一条路）。
//
// 依据 Core/Convert/MAPPING.md §6。
//
// 渲染的叠加信息（全部取自 snapshot()，不自行维护并行状态）：
//   · safe area 内缩     —— DisplayConfig.safe_*（LIVI projectionSafeArea*，AUDIT 8.2）
//   · 显示校准           —— DisplayConfig.*_pct（100=不改），用 SVG filter 作用于整帧（AUDIT 8.3）
//   · 昼夜配色           —— DisplayConfig.day_night（0=沿用既有深色，1=进一步压暗）
//   · 副屏平面           —— DisplayConfig.aux_*（LIVI cluster {main,dash,aux}，AUDIT 8.4）
//   · 元数据/封面位/进度 —— MediaInfo（封面只留位置与 revision，贴图由 Web 层做，AUDIT 6.5）
//   · 车况/电话/链路/音频 —— VehicleState / TelephonyState / LinkState / AudioState
//   · 逐向导航条         —— NavigationState（active==0 时整块不画；destination_reached 画"已到达"；
//                          图标只由我们自己的 Maneuver 枚举选，**不给 maneuver_code 编映射表**）
// 无输入、多输入未选择、手动选择提示、FNV-1a 指纹去重、仅活跃面发布等既有行为不变。
//
// 线程模型：1 Hz tick 线程与**外部线程**（切源后的 HTTP worker）可同时要求出帧，
// 由内部 state_mutex_ 串行化；publish_now() 的完整契约见其声明。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/session_core.hpp"

namespace mvp {

class DesktopRenderer {
 public:
  explicit DesktopRenderer(SessionCore& core, std::string source_id = "core-desktop");
  ~DesktopRenderer();
  DesktopRenderer(const DesktopRenderer&) = delete;
  DesktopRenderer& operator=(const DesktopRenderer&) = delete;

  // 注册为 LocalDesktop 源并启动周期性刷新（1 Hz）。
  void start();
  void stop();
  // 1 Hz 检查，但只有内容变化（或刚成为活跃面）时才真正提交一帧。
  // 静态画面不需要重发，这避免了每秒 256 KiB 的帧体复制。
  // 只有桌面是活跃面、且内容相比上次提交有变化时才真正提交；不再是活跃面时清除标记，
  // 下次成为活跃面必定重发一次。
  // 行为与 publish_now() **完全一致**（就是它的等价别名），保留是因为启动与 tick 都在用。
  void refresh();
  // ── 立刻出一帧（外部线程入口）──────────────────────────────────────
  // 背景：本类自己在成为活跃面时**不会**被通知，只能等下一个 1 Hz tick，
  // 于是"切换源 → 有画面"之间会有最长约一个 tick（实测达 3 秒）的空帧。
  // 切换源的一方（Web 层）在改完选择后立刻调这个入口，即可把空白压到接近 0。
  //
  // 【契约】
  //   · 线程：**可从任意线程调用**（HTTP worker / 1 Hz tick / 主线程均可）。
  //     内部用 state_mutex_ 串行化，不会与 tick 并发写 published_in_epoch_ / last_signature_。
  //   · 阻塞：**不等待 tick**，不 sleep / 不 join / 不做 I/O；只在需要时同步渲染并提交一帧。
  //     持锁时间有界（一次渲染 + 一次 256 KiB 帧体拷贝，μs~ms 量级）；
  //     若与 tick 撞上，只会等对方把这一次渲染做完，不会长时间阻塞。
  //   · 返回 true ：本次**真的提交了一帧**（桌面是活跃面，且 SessionCore 接受了它）。
  //     返回 false：没提交。原因可能是——① 桌面不是活跃面（含未 start、已切换给别人）；
  //     ② 提交被 SessionCore 拒（活跃源不是它）；③ 内容与上次相同且本轮已提交过（指纹去重）。
  //     本接口**不抛出异常**。
  //   · 不启动/不停止任何线程，不改变源注册状态；可在 start() 之前安全调用（返回 false）。
  //   · 调用方必须保证 DesktopRenderer 与它引用的 SessionCore 的生命周期长于调用；
  //     且不要与 stop() 并发（stop() 只负责收 tick 线程，不负责同步外部调用者）。
  bool publish_now();
  // 桌面当前是否就是活跃显示面。
  bool active() const;
  uint64_t frames_published() const { return frames_published_; }
  std::string_view id() const { return id_; }

 private:
  void tick();
  std::string render(const SessionSnapshot& snapshot) const;

  SessionCore& core_;
  std::string id_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<uint64_t> frames_published_{0};
  std::atomic<uint64_t> controls_received_{0};
  // 只在内容变化时提交：桌面是静态画面，1 Hz 重发等于每秒白白复制 256 KiB 的帧体。
  // 每次「重新成为活跃面」后必须无条件提交一次，否则活跃态切换后画面上是空的。
  bool published_in_epoch_{false};
  uint64_t last_signature_{};
  // 守护上面两个非原子字段 + 整段"取快照→渲染→提交"。
  // 锁序固定为 state_mutex_ → SessionCore::mutex_（snapshot/submit_video），反向不存在 ⇒ 无死锁。
  // 不用 recursive/共享锁：临界区里不做任何会回调本对象的事。
  std::mutex state_mutex_;
  // 【绝不要把这个 256 KiB 放栈上】VideoFrame 是 262184 字节（payload 占 256 KiB）。
  // publish_now() 会被 HTTP worker 线程调用，而线程栈余量不可假设：
  // 本项目刚因"大对象放栈上"导致 main() 击穿 8 MiB 栈、服务启动即 SIGSEGV（/health=000）。
  // 所以帧体只申请一次、之后复用（顺带省掉每次发布的 256 KiB 零初始化）；
  // 用 unique_ptr 是为了让 sizeof(DesktopRenderer) 保持很小 ——
  // 这样即便以后有人把它当栈对象（现在 main.cpp 用的是 make_unique，已经是堆）也不会踩坑。
  std::unique_ptr<VideoFrame> frame_;
};

}  // namespace mvp
