// 通过 BlueZ 5 的 org.bluez.ProfileManager1 注册 SPP（Serial Port, UUID 1101）。
//
// 为什么需要这个：BlueZ 5 里 SDP server 归 bluetoothd 所有，老的 `sdptool add SP`
// 路径已不可用（实测 `Failed to connect to SDP server on FF:FF:FF:00:00:00`，
// 适配器 UUID 列表里也始终没有 1101）。手机侧 CarLife 应用扫不到 SPP 通道就连不上，
// 所以必须走 BlueZ 的正式扩展点：D-Bus ProfileManager1。
//
// 做法（依据 BlueZ doc/profile-api.txt）：
//   RegisterProfile(path, "00001101-0000-1000-8000-00805f9b34fb",
//                   {Name, Role="server", Channel=<n>, AutoConnect})
// 带 Channel 时由 bluetoothd 自己监听该 RFCOMM 通道并对外发布 SDP 记录，
// 有手机连入时回调我们导出的 org.bluez.Profile1.NewConnection(device, fd, props)。
//
// 用 sd-bus（systemd 自带）而不是 libdbus：同样的功能代码量约为 1/4。
#pragma once

#include <memory>
#include <string>

namespace carlife {

class SppProfileServer {
 public:
  SppProfileServer();
  ~SppProfileServer();
  SppProfileServer(const SppProfileServer&) = delete;
  SppProfileServer& operator=(const SppProfileServer&) = delete;

  // 注册 SPP 服务。channel 是 RFCOMM 通道号（通常 1）。失败必须返回 false 并给出原因。
  bool start(int channel, const std::string& name, std::string* err);
  // 等待手机连入；成功返回已连接的 fd（调用方负责关闭），失败返回 -1。
  // timeoutMs<=0 表示一直等（内部会分段处理 D-Bus 消息）。
  int waitForConnection(int timeoutMs, std::string* err);
  // 注销 profile 并释放 D-Bus 连接。
  void stop();
  bool active() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace carlife
