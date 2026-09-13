// 蓝牙配对 agent：对手机的配对请求一律「同意」。
//
// 为什么需要自己写（依据公开 issue，不是猜）：
//   * bluez/bluer #190：NoInputNoOutput 的 agent 会拒绝所有配对请求 ——
//     因为它的全部 handler 都是 None，BlueZ 在 just-works 流程里仍会调
//     RequestAuthorization / AuthorizeService，handler 为 None 时只能回错误。
//   * RPi-Distro/repo #291：bluez >= 5.55 起，用 NoInputNoOutput 能力注册 agent 失效
//     （同时影响 bluez-tools 的 bt-agent）。
//   * 板上实测 bt-agent -c NoInputNoOutput 在手机上表现为「PIN 不正确 / 拒绝配对」。
//
// 做法：用 sd-bus 自己实现 org.bluez.Agent1，并以 KeyboardDisplay 能力注册为默认 agent，
// 这样手机走 SSP 时会调 RequestConfirmation（数字比对）——我们回成功即「同意」；
// 走 legacy 配对时会调 RequestPinCode ——我们回一个固定 PIN。
//   * RequestConfirmation / RequestAuthorization / AuthorizeService → 直接同意
//   * RequestPinCode  → 返回固定 PIN（默认 0000）
//   * RequestPasskey  → 返回 0
// 这样无论手机走哪条配对路径，都不需要人参与，且不会出现「PIN 不正确」。
//
// 与 bt-agent 的关系：两者可共存，但**默认 agent** 才生效；
// 本类注册后会调用 RequestDefaultAgent 抢占默认位置，因此优先于 bt-agent。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace carlife {

class PairingAgent {
 public:
  PairingAgent();
  ~PairingAgent();
  PairingAgent(const PairingAgent&) = delete;
  PairingAgent& operator=(const PairingAgent&) = delete;

  // 注册为默认配对 agent。pin 用于 legacy 配对（默认 "0000"）。
  bool start(const std::string& pin, std::string* err);
  void stop();
  bool active() const;

  // 诊断计数：确认/授权的次数，便于在 /api/state 里看出「手机确实来配对过」。
  uint64_t confirmations() const;
  uint64_t pin_requests() const;
  std::string last_device() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace carlife
