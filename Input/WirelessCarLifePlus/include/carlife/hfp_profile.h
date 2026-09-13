// 通过 BlueZ 5 的 org.bluez.ProfileManager1 注册 HFP HS（Hands-Free unit, UUID 111E）。
//
// 为什么需要这个（有源码依据，不是猜）：
//   百度官方车机端（Reference/carlife-vehicle-lib/CarLife-Android-Vehicle）的蓝牙模块整体
//   围绕 HFP 实现，而不是 SPP：
//     * BtPairStateMachine.PairEnableState.enter() -> BtHfpProtocolHelper.btOOBInfo(...)
//     * 该状态机用 getHfpConnectionState()（BluetoothProfile 的 HFP 状态）判连接
//     * 配套 CarlifeBTHfpConnectionProto / CarlifeBTHfpIndicationProto /
//       CarlifeBTIdentifyResultIndProto
//   CarLife 用 "这是不是一台免提车载套件(HFP HS)" 来识别车机。我们的板子此前只发布
//   SPP(1101) 与 AVRCP(110C/110E)，没有 111E —— 所以手机端 CarLife 的无线列表里
//   根本看不到本机，也就无法把它当成可连接的 CarLife 对象。
//
// 为什么用 111E 而不是 111F：
//   111F = HFP AG（Audio Gateway，手机角色）；111E = HFP HS（Hands-Free，车机角色）。
//   我们要扮演的是车机，故选 111E。
//
// 为什么自己注册而不是装 PulseAudio/oFono（板上实测两者都不存在）：
//   BlueZ 的 audio 插件只在存在免提后端时才注册 HFP；没有后端就只剩 AVRCP
//   （板子上 AVRCP 有、A2DP/HFP 都没有，正是这个原因）。
//   自己用 ProfileManager1 注册可完全掌控 SDP 记录、RFCOMM 通道与后续 AT 报文处理。
//
// 依据 BlueZ doc/org.bluez.ProfileManager.rst：
//   RegisterProfile(path, uuid, {Name, Role, Channel} | {…, Version, Features})
//   官方预定义值：HFP HS 默认 Version 1.7、Features 0b000000、RFCOMM 通道 7。
//   选项名与类型（Channel/Version/Features 均为 uint16 → D-Bus "q"）由该文档确认。
#pragma once

#include <memory>
#include <string>

namespace carlife {

class HfpProfileServer {
 public:
  HfpProfileServer();
  ~HfpProfileServer();
  HfpProfileServer(const HfpProfileServer&) = delete;
  HfpProfileServer& operator=(const HfpProfileServer&) = delete;

  // 注册 HFP HS 服务。
  //   channel  : RFCOMM 通道号，BlueZ 对 HFP HS 的默认值是 7。
  //   features : HF 特性位掩码（HFP 1.7 规范）。车机常用：EC/NR、三方通话、
  //              来电显示、语音识别、远程音量、增强通话状态/控制 → 低 7 位 = 0x007F。
  //   version  : HFP 版本，0x0107 = 1.7。
  // 失败返回 false 并给出原因。
  bool start(int channel, const std::string& name, uint16_t features, uint16_t version,
             std::string* err);

  // 等待手机连入 HFP；成功返回已连接的 fd（调用方负责关闭），失败返回 -1。
  // timeoutMs<=0 表示一直等（内部分段处理 D-Bus 消息）。
  int waitForConnection(int timeoutMs, std::string* err);

  // 注销 profile 并释放 D-Bus 连接。
  void stop();
  bool active() const;

  // 最近一次连入的设备对象路径（诊断用）。
  std::string peer() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace carlife
