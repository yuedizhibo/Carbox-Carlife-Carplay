#include "carlife/transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>

namespace carlife {
namespace {

struct ChannelPort {
  int32_t channel;
  int mdPort;   // 无线：手机侧监听端口
  int huPort;   // 有线/转发：车机侧监听端口
  const char* name;
};

const ChannelPort kChannels[] = {
    {ch::CMD, port::MD_CMD, port::HU_CMD, "CMD"},
    {ch::VIDEO, port::MD_VIDEO, port::HU_VIDEO, "VIDEO"},
    {ch::AUDIO, port::MD_AUDIO, port::HU_AUDIO, "MEDIA"},
    {ch::TTS, port::MD_TTS, port::HU_TTS, "TTS"},
    {ch::VR, port::MD_VR, port::HU_VR, "VR"},
    {ch::TOUCH, port::MD_TOUCH, port::HU_TOUCH, "TOUCH"},
    {ch::UPDATE, port::MD_UPDATE, port::HU_UPDATE, "UPDATE"},
};

int tcpConnect(const std::string& host, int port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return -1;
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int tcpListen(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  if (::listen(fd, 4) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

Transport::Transport() = default;

Transport::~Transport() { close(); }

bool Transport::blockRead(int fd, uint8_t* dst, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, dst + got, n - got);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

bool Transport::attachFd(int32_t channel, int fd) {
  if (channel <= 0 || channel >= ch::COUNT) return false;
  auto& c = chans_[channel];
  if (c.alive.load()) {
    ::close(fd);
    return false;  // 该通道已建立
  }
  c.fd = fd;
  c.alive.store(true);
  c.th = std::thread([this, channel] { readerLoop(channel); });
  cv_.notify_all();
  return true;
}

bool Transport::connectPhone(const std::string& phoneIp, bool includeUpdateChannel,
                             std::string* err) {
  int ok = 0;
  for (const auto& cp : kChannels) {
    if (cp.channel == ch::UPDATE && !includeUpdateChannel) continue;
    int fd = tcpConnect(phoneIp, cp.mdPort);
    if (fd < 0) {
      if (err) *err = std::string("connect ") + phoneIp + ":" + std::to_string(cp.mdPort) +
                      " (" + cp.name + ") failed: " + strerror(errno);
      continue;
    }
    // attachFd 成功返回 true；失败（含“该通道已建立”）时它自己会 close(fd)。
    // 原写法是 if(!attachFd(...)) ++ok; else ++ok; —— 两个分支一样，
    // 于是失败也会被计成成功，ok 失去意义。
    if (attachFd(cp.channel, fd)) ++ok;
  }
  // CMD 通道是会话的生命线，必须有
  if (!channelOpen(ch::CMD)) {
    if (err) *err = "CMD channel (" + std::to_string(port::MD_CMD) + ") not established on " + phoneIp;
    close();
    return false;
  }
  return ok > 0;
}

bool Transport::serveHUPorts(bool includeUpdateChannel, std::string* err) {
  for (const auto& cp : kChannels) {
    if (cp.channel == ch::UPDATE && !includeUpdateChannel) continue;
    int fd = tcpListen(cp.huPort);
    if (fd < 0) {
      if (err) *err = "listen :" + std::to_string(cp.huPort) + " (" + cp.name + ") failed: " + strerror(errno);
      close();
      return false;
    }
    listeners_.push_back(fd);
  }
  for (size_t i = 0; i < listeners_.size(); ++i) {
    int lfd = listeners_[i];
    int32_t channel = i < 2 ? (i == 0 ? ch::CMD : ch::VIDEO)
                  : (i == 2 ? ch::AUDIO : (i == 3 ? ch::TTS : (i == 4 ? ch::VR : (i == 5 ? ch::TOUCH : ch::UPDATE))));
    acceptors_.emplace_back([this, lfd, channel] {
      int fd = ::accept(lfd, nullptr, nullptr);
      if (fd >= 0) attachFd(channel, fd);
    });
  }
  // 等 CMD 通道接进来（最多 60s）
  for (int i = 0; i < 600; ++i) {
    if (channelOpen(ch::CMD)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!channelOpen(ch::CMD)) {
    if (err) *err = "no inbound CMD connection on :" + std::to_string(port::HU_CMD);
    return false;
  }
  return true;
}

void Transport::readerLoop(int32_t channel) {
  auto& c = chans_[channel];
  const int hs = headerSize(channel);
  std::vector<uint8_t> hdr(static_cast<size_t>(hs));
  while (!closed_.load()) {
    HeaderInfo hi{};
    if (!blockRead(c.fd, hdr.data(), static_cast<size_t>(hs))) break;
    parseHeader(hdr.data(), channel, &hi);
    if (hi.payloadSize > 64u * 1024u * 1024u) break;  // 明显越界，断开
    Frame f;
    f.channel = channel;
    f.serviceType = hi.serviceType;
    f.timestamp = hi.timestamp;
    f.payload.resize(hi.payloadSize);
    if (hi.payloadSize > 0 && !blockRead(c.fd, f.payload.data(), hi.payloadSize)) break;
    {
      std::lock_guard<std::mutex> lk(mu_);
      queue_.push_back(std::move(f));
    }
    cv_.notify_one();
  }
  c.alive.store(false);
  if (c.fd >= 0) {
    ::close(c.fd);
    c.fd = -1;
  }
  bool anyAlive = false;
  for (auto& other : chans_) {
    if (other.alive.load()) anyAlive = true;
  }
  if (!anyAlive) {
    closed_.store(true);
    cv_.notify_all();
  }
}

bool Transport::channelOpen(int32_t channel) const {
  if (channel <= 0 || channel >= ch::COUNT) return false;
  return chans_[channel].alive.load();
}

int Transport::openChannels() const {
  int n = 0;
  for (auto& c : chans_) {
    if (c.alive.load()) ++n;
  }
  return n;
}

bool Transport::send(int32_t channel, const Frame& f) {
  if (channel <= 0 || channel >= ch::COUNT) return false;
  auto& c = chans_[channel];
  if (!c.alive.load()) return false;
  if (isShortHeaderChannel(channel) && f.payload.size() > 0xFFFF) return false;  // 长度域只有 16 位
  std::vector<uint8_t> bytes = f.encode();
  std::lock_guard<std::mutex> lk(c.writeMu);
  size_t off = 0;
  while (off < bytes.size()) {
    ssize_t w = ::write(c.fd, bytes.data() + off, bytes.size() - off);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

bool Transport::send(int32_t channel, uint32_t serviceType) {
  return send(channel, serviceType, {});
}

bool Transport::send(int32_t channel, uint32_t serviceType, std::vector<uint8_t> payload) {
  Frame f;
  f.channel = channel;
  f.serviceType = serviceType;
  f.payload = std::move(payload);
  if (channel != ch::CMD && channel != ch::TOUCH) {
    f.timestamp = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
  return send(channel, f);
}

bool Transport::recv(Frame* out, int timeoutMs) {
  std::unique_lock<std::mutex> lk(mu_);
  auto ready = [this] { return !queue_.empty() || closed_.load(); };
  if (timeoutMs < 0) {
    cv_.wait(lk, ready);
  } else {
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), ready)) return false;
  }
  if (queue_.empty()) return false;
  *out = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

void Transport::close() {
  // 【必须对并发调用安全，且最终必须把所有线程 join 完】
  // 谁会并发调进来：① 会话结束 Session::start() 里的 tx_.close()；
  //                   ② 读线程收到 EOF/错误时也会关。
  // 原实现丢了 compare_exchange 的结果 → 两边 join 同一个 std::thread →
  // 第二个抛 std::system_error(ESRCH)、无人捕获 → std::terminate。
  //   实测崩溃栈: carlife::Transport::close() ← Session::start() ← run()
  // 新结构：
  //   * 用 close_mu_ 串行化；
  //   * 第一次调用者关闭 fd 并 join（跳过调用者自身，自我 join 也会抛）；
  //   * 之后的调用者仍然再跑一遍 joinThreads() —— 因为若第一次的调用者
  //     正是某个读线程，它无法 join 自己，必须靠后面这次补上；
  //     否则 ~Transport 会在“线程仍 joinable”时销毁 std::thread → terminate。
  std::lock_guard<std::mutex> lk(close_mu_);
  if (!closed_) {
    closed_ = true;
    for (int fd : listeners_) {
      if (fd >= 0) ::close(fd);
    }
    listeners_.clear();
    for (auto& c : chans_) {
      if (c.fd >= 0) {
        ::shutdown(c.fd, SHUT_RDWR);
        ::close(c.fd);
        c.fd = -1;
      }
    }
  }
  joinThreads();
  cv_.notify_all();
}

void Transport::joinThreads() {
  const auto self = std::this_thread::get_id();
  for (auto& c : chans_) {
    if (!c.th.joinable()) continue;
    if (c.th.get_id() == self) continue;
    try {
      c.th.join();
    } catch (const std::exception&) {
      // 进来就绝不能再把异常带走：close() 抛异常会直接 terminate 整个进程。
    }
  }
  for (auto& t : acceptors_) {
    if (!t.joinable()) continue;
    if (t.get_id() == self) continue;
    try {
      t.join();
    } catch (const std::exception&) {
    }
  }
  acceptors_.clear();
}

// ------------------------------------------------------------------ 发现
bool Discovery::waitForPhone(int port, int timeoutMs, DiscoveryResult* out) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return false;
  }
  timeval tv{};
  if (timeoutMs >= 0) {
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  sockaddr_in from{};
  socklen_t fl = sizeof(from);
  uint8_t buf[2048];
  ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
  ::close(fd);
  if (n <= 0) return false;
  char ip[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
  out->phoneIp = ip;
  out->port = ntohs(from.sin_port);
  out->raw.assign(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
  return true;
}

void Discovery::sendBeacon(const std::string& phoneIp, int port, const std::string& text) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, phoneIp.c_str(), &to.sin_addr) == 1) {
    ::sendto(fd, text.data(), text.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
  }
  ::close(fd);
}

}  // namespace carlife