// 7 条 TCP 通道的传输层。每通道一个阻塞读线程，把 Frame 投进统一队列。
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "carlife/service_types.h"
#include "carlife/wire.h"

namespace carlife {

// 无线发现：手机接入车机 WiFi 后向 UDP 7999 发发现报文，车机用报文源地址作为手机 IP
// （与本项目 Android 侧 R100 实现一致）。真实环境还需要 BLE / 经典蓝牙 SPP
// (UUID 00001101-0000-1000-8000-00805F9B34FB) 做配对引导，见 discovery.cpp。
struct DiscoveryResult {
  std::string phoneIp;
  std::string raw;
  uint16_t port = 0;
};

class Discovery {
 public:
  static bool waitForPhone(int port, int timeoutMs, DiscoveryResult* out);
  static void sendBeacon(const std::string& phoneIp, int port, const std::string& text);
};

class Transport {
 public:
  Transport();
  ~Transport();
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  // 无线：车机主动连手机（WirlessConnector.kt:20-26）
  bool connectPhone(const std::string& phoneIp, bool includeUpdateChannel, std::string* err);
  // 有线/adb 转发：车机在 7200/8200/... 监听，等待手机连入
  bool serveHUPorts(bool includeUpdateChannel, std::string* err);

  bool send(int32_t channel, const Frame& f);
  bool send(int32_t channel, uint32_t serviceType);
  bool send(int32_t channel, uint32_t serviceType, std::vector<uint8_t> payload);

  // 取一帧；timeoutMs<0 无限等。返回 false = 超时或全部通道关闭
  bool recv(Frame* out, int timeoutMs);
  bool allClosed() const { return closed_.load(); }
  bool channelOpen(int32_t channel) const;
  int openChannels() const;
  void close();

 private:
  struct Chan {
    int fd = -1;
    std::thread th;
    std::atomic<bool> alive{false};
    std::mutex writeMu;
  };
  void readerLoop(int32_t channel);
  bool attachFd(int32_t channel, int fd);
  bool blockRead(int fd, uint8_t* dst, size_t n);

  std::array<Chan, ch::COUNT> chans_{};
  std::vector<int> listeners_{};
  std::mutex mu_;
    // 串行化 close()：会话结束路径与“读线程 EOF”路径会并发调用它。
    std::mutex close_mu_;
    // 把线程 join 完（跳过调用者自身）。close() 内部会调；单独抽出是因为：
    // 若首个调用者本身就是某个读线程，它无法 join 自己，必须由后续调用者补上，
    // 否则 ~Transport 会在“仍 joinable”时销毁 std::thread → terminate。
    void joinThreads();
  std::condition_variable cv_;
  std::deque<Frame> queue_;
  std::atomic<bool> closed_{false};
  std::vector<std::thread> acceptors_{};
};

}  // namespace carlife
