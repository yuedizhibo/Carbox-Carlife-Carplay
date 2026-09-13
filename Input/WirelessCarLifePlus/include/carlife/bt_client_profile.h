// 用蓝牙协议栈自己完成「按 UUID 连接远端服务」。
//
// 依据参考实现（carlife-sdk/.../receiver/transport/instant/BluetoothDeviceDiscover.kt）：
//     device.createRfcommSocketToServiceRecord(UUID).connect()
// 语义是：**给定服务 UUID，由蓝牙协议栈去查对方 SDP 并建立连接** —— 调用方不需要
// 知道 RFCOMM 通道号。
//
// Linux/BlueZ 的等价机制是两步：
//   1) org.bluez.ProfileManager1.RegisterProfile(path, uuid, {Role:"client"})
//      —— 注册一个客户端角色 profile，BlueZ 连上后会通过 Profile1.NewConnection
//         把已连接的 fd 交给我们；
//   2) org.bluez.Device1.ConnectProfile(uuid)
//      —— BlueZ 自己去做 SDP 解析、自己选通道、自己连。
//
// 为什么改成这样：此前我们手工 `sdptool browse` 解析文本再自己猜通道号，
// 既脆弱（实测同一命令在 shell 里 5194 字节、在应用里却解析出 0 条），
// 又只能覆盖标准 SPP(0x1101) —— 而实测手机（小米）用的是厂商自定义 UUID。
// 交给协议栈后，候选 UUID 可以逐个试（BlueZ 会各自解析通道），
// 不再需要任何文本解析。
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace carlife {

class BtClientProfile {
 public:
  BtClientProfile();
  ~BtClientProfile();
  BtClientProfile(const BtClientProfile&) = delete;
  BtClientProfile& operator=(const BtClientProfile&) = delete;

  // 注册客户端角色 profile（同一时刻只保留一个 uuid 的注册）。
  // 重复调用会先注销上一个。
  bool registerClient(const std::string& uuid, std::string* err);

  // 请求 BlueZ 连接 devicePath（形如 /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF）
  // 上 uuid 对应的服务。成功时返回已连接的 fd（调用方负责关闭），失败返回 -1。
  // BlueZ 若找不到该 UUID 的服务会回 org.bluez.Error.NotSupported 之类，属正常候选失败。
  int connectProfile(const std::string& devicePath, const std::string& uuid, int timeoutMs,
                     std::string* err);

  // 取远端设备已发布的服务 UUID 列表（Device1.UUIDs）—— 这就是我们的候选来源，
  // 不需要任何 SDP 文本解析。
  static bool deviceUuids(const std::string& devicePath, std::vector<std::string>* out,
                          std::string* err);

  // 由蓝牙地址构造 BlueZ 的设备对象路径。
  static std::string devicePathForMac(const std::string& mac);

  void stop();
  bool active() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace carlife
