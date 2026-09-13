#include "carlife/bt_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include "carlife/service_types.h"
#include "carlife/wire.h"

// BlueZ 的 RFCOMM 不需要额外开发包：地址族/协议号是稳定 ABI，这里自己声明，
// 这样在没有 libbluetooth-dev 的机器（比如当前 WSL）上照样能编译。
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef BTPROTO_RFCOMM
#define BTPROTO_RFCOMM 15
#endif

namespace carlife {
namespace {

struct SockaddrRc {
  sa_family_t rc_family = static_cast<sa_family_t>(AF_BLUETOOTH);
  uint32_t rc_port = 0;  // htobdaddr + channel
  uint8_t rc_bdaddr[6] = {0};
};

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

bool runQuiet(const std::string& cmd, std::string* out) {
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return false;
  char buf[512];
  std::string got;
  while (fgets(buf, sizeof(buf), p)) got += buf;
  int rc = pclose(p);
  if (out) *out = trim(got);
  return rc == 0;
}

}  // namespace

BtLink::BtLink() = default;

BtLink::~BtLink() { close(); }

void BtLink::close() {
  if (linkFd_ >= 0) {
    ::close(linkFd_);
    linkFd_ = -1;
  }
  if (listenFd_ >= 0) {
    ::close(listenFd_);
    listenFd_ = -1;
  }
  rxbuf_.clear();
}

bool BtLink::bindListen(int domain, const sockaddr* addr, socklen_t len, const std::string& what,
                        std::string* err) {
  int fd = ::socket(domain, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = "socket(" + what + "): " + strerror(errno);
    return false;
  }
  if (domain != AF_BLUETOOTH) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  }
  if (::bind(fd, addr, len) != 0) {
    std::string hint;
    if (errno == EOPNOTSUPP && domain == AF_UNIX) {
      hint = "（AF_UNIX 不能建在 Windows 挂载盘/DrvFs 上，请把桥接路径放到 /tmp）";
    }
    if (errno == EAFNOSUPPORT && domain == AF_BLUETOOTH) {
      hint = "（本机内核没有蓝牙协议栈，可改用 --bt-local 桥接做对拍）";
    }
    if (err) *err = "bind(" + what + "): " + strerror(errno) + hint;
    ::close(fd);
    return false;
  }
  if (::listen(fd, 2) != 0) {
    if (err) *err = "listen(" + what + "): " + strerror(errno);
    ::close(fd);
    return false;
  }
  listenFd_ = fd;
  return true;
}

bool BtLink::listenRfcomm(int channel, std::string* err) {
  SockaddrRc a{};
  a.rc_port = static_cast<uint32_t>(channel);  // htobs: 小端
  memset(a.rc_bdaddr, 0, sizeof(a.rc_bdaddr));  // ANY
  if (!bindListen(AF_BLUETOOTH, reinterpret_cast<sockaddr*>(&a), sizeof(a),
                  "rfcomm channel " + std::to_string(channel), err)) {
    return false;
  }
  return true;
}

bool BtLink::listenLocal(const std::string& path, std::string* err) {
  ::unlink(path.c_str());
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
  if (!bindListen(AF_UNIX, reinterpret_cast<sockaddr*>(&a), sizeof(a), "unix " + path, err)) {
    return false;
  }
  path_ = path;
  return true;
}

bool BtLink::acceptLink(int timeoutMs, std::string* err) {
  if (listenFd_ < 0) {
    if (err) *err = "没有监听中的蓝牙链路";
    return false;
  }
  pollfd pfd{listenFd_, POLLIN, 0};
  int rc = ::poll(&pfd, 1, timeoutMs);
  if (rc == 0) {
    if (err) *err = "等待手机通过蓝牙连入超时";
    return false;
  }
  if (rc < 0) {
    if (err) *err = std::string("poll: ") + strerror(errno);
    return false;
  }
  int fd = ::accept(listenFd_, nullptr, nullptr);
  if (fd < 0) {
    if (err) *err = std::string("accept: ") + strerror(errno);
    return false;
  }
  linkFd_ = fd;
  rxbuf_.clear();
  return true;
}

bool BtLink::adoptFd(int fd, std::string* err) {
  if (fd < 0) {
    if (err) *err = "adoptFd: invalid fd";
    return false;
  }
  close();
  linkFd_ = fd;
  rxbuf_.clear();
  return true;
}

bool BtLink::connectLocal(const std::string& path, int timeoutMs, std::string* err) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = strerror(errno);
    return false;
  }
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (true) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
      linkFd_ = fd;
      return true;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      if (err) *err = std::string("connect unix ") + path + ": " + strerror(errno);
      ::close(fd);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

bool BtLink::connectRfcomm(const std::string& mac, int channel, int timeoutMs, std::string* err) {
  SockaddrRc a{};
  a.rc_port = static_cast<uint32_t>(channel);
  // 解析 "AA:BB:CC:DD:EE:FF"（BlueZ 的 bdaddr 是小端 6 字节）
  unsigned b[6] = {0};
  if (std::sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
    if (err) *err = "蓝牙地址格式不对: " + mac;
    return false;
  }
  for (int i = 0; i < 6; ++i) a.rc_bdaddr[i] = static_cast<uint8_t>(b[5 - i]);
  int fd = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
  if (fd < 0) {
    if (err) *err = std::string("socket(AF_BLUETOOTH): ") + strerror(errno) + " (本机没有蓝牙协议栈?)";
    return false;
  }
  (void)timeoutMs;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    if (err) *err = std::string("connect rfcomm ") + mac + ": " + strerror(errno);
    ::close(fd);
    return false;
  }
  linkFd_ = fd;
  return true;
}

bool BtLink::findPairedPhone(const std::string& wantName, std::string* mac, std::string* name) {
  std::string out;
  // bluetoothctl 支持按状态过滤；拿不到就退回全部已知设备。
  if (!runQuiet("bluetoothctl devices Paired 2>/dev/null", &out) || out.empty()) {
    (void)runQuiet("bluetoothctl devices 2>/dev/null", &out);
  }
  if (out.empty()) return false;
  // 逐行解析："Device AA:BB:CC:DD:EE:FF 设备名"
  std::string firstMac, firstName;
  std::size_t lineStart = 0;
  while (lineStart < out.size()) {
    std::size_t lineEnd = out.find('\n', lineStart);
    const std::string line = out.substr(lineStart, lineEnd == std::string::npos
                                                       ? std::string::npos
                                                       : lineEnd - lineStart);
    lineStart = (lineEnd == std::string::npos) ? out.size() : lineEnd + 1;
    const std::size_t p = line.find("Device ");
    if (p == std::string::npos) continue;
    const std::string rest = line.substr(p + 7);
    const std::size_t sp = rest.find(' ');
    const std::string m = (sp == std::string::npos) ? rest : rest.substr(0, sp);
    const std::string n = (sp == std::string::npos) ? std::string() : trim(rest.substr(sp + 1));
    if (firstMac.empty()) {
      firstMac = m;
      firstName = n;
    }
    if (!wantName.empty() && n == wantName) {
      if (mac) *mac = m;
      if (name) *name = n;
      return true;
    }
  }
  if (wantName.empty() && !firstMac.empty()) {
    if (mac) *mac = firstMac;
    if (name) *name = firstName;
    return true;
  }
  return false;
}

// 解析 sdptool browse 的输出：每条服务记录的 {名称, 服务类 UUID, RFCOMM 通道}。
namespace {
struct SdpRecord {
  std::string name;
  std::string uuid;
  int channel = 0;
};

std::vector<SdpRecord> parseSdp(const std::string& out) {
  std::vector<SdpRecord> recs;
  SdpRecord cur;
  bool in_record = false;
  std::size_t pos = 0;
  while (pos <= out.size()) {
    const std::size_t nl = out.find('\n', pos);
    const std::string raw =
        out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? out.size() + 1 : nl + 1;
    const std::string line = trim(raw);
    if (line.rfind("Service RecHandle:", 0) == 0) {
      if (in_record) recs.push_back(cur);
      cur = SdpRecord{};
      in_record = true;
      continue;
    }
    if (!in_record) continue;
    if (line.rfind("Service Name:", 0) == 0) {
      cur.name = trim(line.substr(std::strlen("Service Name:")));
      continue;
    }
    if (line.rfind("UUID 128:", 0) == 0) {
      cur.uuid = trim(line.substr(std::strlen("UUID 128:")));
      continue;
    }
    // Service Class ID List 里的条目形如:  "Serial Port" (0x1101)
    if (line.size() > 4 && line[0] == '"') {
      const std::size_t lp = line.find("(0x");
      const std::size_t rp = line.find(')', lp);
      if (lp != std::string::npos && rp != std::string::npos && rp > lp + 3) {
        const std::string hex = line.substr(lp + 3, rp - lp - 3);
        if (hex.size() == 4) cur.uuid += (cur.uuid.empty() ? "0x" : ",0x") + hex;
      }
      continue;
    }
    if (line.rfind("Channel:", 0) == 0 && cur.channel == 0) {
      cur.channel = std::atoi(trim(line.substr(std::strlen("Channel:"))).c_str());
      continue;
    }
  }
  if (in_record) recs.push_back(cur);
  return recs;
}
}  // namespace

int BtLink::querySppChannel(const std::string& mac, std::string* note) {
  std::string out;
  (void)runQuiet("sdptool browse " + mac + " 2>&1", &out);
  const auto recs = parseSdp(out);
  // 【诊断输出位置】数字进 note（JSON 字段），原文进 stdout（mvp.log）。
  // 教训：曾把 out.substr(0,160) 直接放进 note —— 按字节截断把某个 UTF-8
  // 中文字符劈成半个，导致 /api/state 整个 JSON 非法 UTF-8、前端解析失败。
  // 对外部数据做字节截断必须考虑编码边界，宁可只报数字。
  {
    std::string head;
    for (unsigned char c : out.substr(0, 200)) {
      head += (c >= 0x20 && c < 0x7f) ? char(c) : '.';
    }
    std::cout << "[bt] SDP 原始输出 " << out.size() << " 字节, 解析出 " << recs.size()
              << " 条记录; 开头(ASCII): " << head << std::endl;
  }
  // 【严格匹配】只认服务类里带 Serial Port(0x1101) 的记录。
  // 实测教训：旧版“取第一个 Channel:”会抓到 Headset Gateway(1112) 的 channel 2，
  // 拿它去连 SPP 必然失败 —— 宁可不连，也不猜。
  for (const auto& r : recs) {
    if (r.channel > 0 && r.uuid.find("0x1101") != std::string::npos) {
      if (note) *note = "SDP: SPP(0x1101) channel=" + std::to_string(r.channel);
      return r.channel;
    }
  }
  // 未找到：把手机实际提供的 RFCOMM 服务完整列出，便于判断它用的是哪个厂商 UUID。
  std::string list;
  for (const auto& r : recs) {
    if (r.channel <= 0) continue;
    if (!list.empty()) list += "; ";
    list += (r.name.empty() ? std::string("(无名)") : r.name) + "[ch" + std::to_string(r.channel);
    if (!r.uuid.empty()) list += " " + r.uuid;
    list += "]";
  }
  if (note) {
    *note = "手机未发布 SPP(0x1101)；RFCOMM服务: " + (list.empty() ? std::string("(无)") : list) +
            " [原始输出 " + std::to_string(out.size()) + " 字节, 记录 " +
            std::to_string(recs.size()) + " 条]";
  }
  return -1;
}

std::vector<std::pair<std::string, int>> BtLink::listRfcommChannels(const std::string& mac) {
  std::string out;
  (void)runQuiet("sdptool browse " + mac + " 2>&1", &out);
  std::vector<std::pair<std::string, int>> result;
  for (const auto& r : parseSdp(out)) {
    if (r.channel > 0) {
      result.emplace_back(r.name.empty() ? std::string("(无名)") : r.name, r.channel);
    }
  }
  return result;
}

bool BtLink::sendRaw(uint32_t serviceType, const std::vector<uint8_t>& payload) {
  if (linkFd_ < 0) return false;
  Frame f;
  f.channel = ch::CMD;  // 蓝牙链路只跑 CMD 通道短头
  f.serviceType = serviceType;
  f.payload = payload;
  auto bytes = f.encode();
  size_t off = 0;
  while (off < bytes.size()) {
    ssize_t w = ::write(linkFd_, bytes.data() + off, bytes.size() - off);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

bool BtLink::send(uint32_t serviceType, const std::vector<uint8_t>& payload) {
  return sendRaw(serviceType, payload);
}

bool BtLink::recv(uint32_t* serviceType, std::vector<uint8_t>* payload, int timeoutMs) {
  if (linkFd_ < 0) return false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  auto need = [](const std::vector<uint8_t>& b) { return b.size() < 8 ? 8 - b.size() : static_cast<size_t>(be16_get(b.data())); };
  for (;;) {
    size_t want = need(rxbuf_);
    if (want == 0) {  // 头里长度是 0 => 整帧只有 8 字节头
      *serviceType = be32_get(rxbuf_.data() + 4);
      payload->assign(rxbuf_.begin() + 8, rxbuf_.end());
      rxbuf_.clear();
      return true;
    }
    if (rxbuf_.size() >= 8 && rxbuf_.size() >= want + 8) {
      *serviceType = be32_get(rxbuf_.data() + 4);
      payload->assign(rxbuf_.begin() + 8, rxbuf_.begin() + 8 + want);
      rxbuf_.erase(rxbuf_.begin(), rxbuf_.begin() + 8 + static_cast<long>(want));
      return true;
    }
    int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count());
    if (ms <= 0) return false;
    pollfd pfd{linkFd_, POLLIN, 0};
    int rc = ::poll(&pfd, 1, ms);
    if (rc <= 0) return false;
    uint8_t buf[4096];
    ssize_t n = ::read(linkFd_, buf, sizeof(buf));
    if (n <= 0) return false;
    rxbuf_.insert(rxbuf_.end(), buf, buf + n);
  }
}

bool BtLink::bringup(const BringupConfig& cfg, BringupResult* out, std::string* err) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.timeoutMs);
  bool sentIpRequest = false;
  bool gotIp = false;
  while (std::chrono::steady_clock::now() < deadline) {
    int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - std::chrono::steady_clock::now())
                                    .count());
    if (left <= 0) break;
    uint32_t stype = 0;
    std::vector<uint8_t> payload;
    if (!recv(&stype, &payload, left)) {
      if (!gotIp) {
        *err = "蓝牙引导阶段没有拿到手机 IP";
        return false;
      }
      break;
    }
    switch (stype) {
      case msg::WIRELESS_INFO_REQUEST: {
        out->peerType = 0;
        out->peerFrequency = 0;
        PbReader r(payload.data(), payload.size());
        uint32_t f = 0, w = 0;
        while (r.next(&f, &w)) {
          if (f == 1 && w == 0) r.readInt32(&out->peerType);
          else if (f == 2 && w == 0) r.readInt32(&out->peerFrequency);
          else if (!r.skip(w)) break;
        }
        PbWriter pw;
        pw.fieldInt32(1, cfg.wirelessType);
        pw.fieldInt32(2, cfg.wifiFrequency);
        sendRaw(msg::WIRELESS_INFO_RESPONSE, pw.data());
        std::cout << "[bt] 应答 WIRELESS_INFO_RESPONSE type=" << cfg.wirelessType
                  << " freq=" << cfg.wifiFrequency << " (手机侧请求 type=" << out->peerType << ")" << std::endl;
        break;
      }
      case msg::WIRELESS_TARGET_INFO_REQUEST: {
        PbWriter pw;
        pw.fieldString(1, cfg.wifiDeviceName);
        pw.fieldString(2, cfg.targetInfo);
        sendRaw(msg::WIRELESS_TARGET_INFO_RESPONSE, pw.data());
        std::cout << "[bt] 应答 WIRELESS_TARGET_INFO_RESPONSE wifiDeviceName=" << cfg.wifiDeviceName
                  << std::endl;
        if (!sentIpRequest) {
          sentIpRequest = true;
          sendRaw(msg::WIRELESS_REQUEST_IP, {});
          std::cout << "[bt] 已发 WIRELESS_REQUEST_IP，等手机给出它的地址" << std::endl;
        }
        break;
      }
      case msg::WIRELESS_RESPONSE_IP: {
        PbReader r(payload.data(), payload.size());
        uint32_t f = 0, w = 0;
        while (r.next(&f, &w)) {
          if (f == 1 && w == 2) {
            r.readString(out ? &out->phoneIp : nullptr);
          } else if (!r.skip(w)) {
            break;
          }
        }
        if (!out->phoneIp.empty()) {
          gotIp = true;
          std::cout << "[bt] 手机 IP = " << out->phoneIp << std::endl;
          // 手机常常紧接着发 MD_STATUS，再收 500ms 把引导阶段尾巴处理干净
          for (int tail = 0; tail < 5; ++tail) {
            uint32_t extraType = 0;
            std::vector<uint8_t> extra;
            if (!recv(&extraType, &extra, 100)) break;
            if (extraType == msg::WIRELESS_MD_STATUS) {
              int32_t status = 0;
              PbReader r(extra.data(), extra.size());
              uint32_t f2 = 0, w2 = 0;
              while (r.next(&f2, &w2)) {
                if (f2 == 1 && w2 == 0) r.readInt32(&status);
                else if (!r.skip(w2)) break;
              }
              std::cout << "[bt] 手机 MD_STATUS = " << status << std::endl;
              PbWriter pw2;
              pw2.fieldInt32(1, 1);
              sendRaw(msg::WIRELESS_HU_STATUS, pw2.data());
            }
          }
          return true;
        }
        *err = "WIRELESS_RESPONSE_IP 载荷为空";
        return false;
      }
      case msg::WIRELESS_MD_STATUS: {
        int32_t status = 0;
        PbReader r(payload.data(), payload.size());
        uint32_t f = 0, w = 0;
        while (r.next(&f, &w)) {
          if (f == 1 && w == 0) r.readInt32(&status);
          else if (!r.skip(w)) break;
        }
        std::cout << "[bt] 手机 MD_STATUS = " << status << std::endl;
        PbWriter pw;
        pw.fieldInt32(1, 1);
        sendRaw(msg::WIRELESS_HU_STATUS, pw.data());
        break;
      }
      default:
        if (cfg.verbose) std::cout << "[bt] 忽略引导消息 0x" << std::hex << stype << std::dec << std::endl;
        break;
    }
  }
  if (gotIp) return true;
  *err = "蓝牙引导未完成";
  return false;
}

bool BtLink::advertiseBlueZ(const std::string& name, bool verbose, std::string* note) {
  std::string out;
  bool any = false;
  if (runQuiet("command -v hciconfig >/dev/null 2>&1 && hciconfig -a 2>/dev/null | head -20", &out)) {
    if (out.find("usb") == std::string::npos && out.empty()) {
      if (note) *note = "hciconfig 没看到任何蓝牙适配器";
      return false;
    }
    any = true;
    std::string q;
    runQuiet("hciconfig hci0 up 2>&1 | tail -1", &q);
    if (verbose) std::cout << "[bt] hciconfig hci0 up: " << trim(q) << std::endl;
    runQuiet("hciconfig hci0 name \"" + name + "\" 2>&1 | tail -1", &q);
    runQuiet("hciconfig hci0 piscan 2>&1 | tail -1", &q);  // 可发现 + 可连接
    if (verbose) std::cout << "[bt] hciconfig hci0 piscan: " << trim(q) << std::endl;
  }
  // 【切勿调用 sdptool add SP】
  // BlueZ 5 里 SDP server 归 bluetoothd 所有，这条老命令要么无效、要么成功写入
  // 一条我们无法管理的 SPP 记录 —— 而后者会占住 UUID 00001101，使下一步
  // ProfileManager1.RegisterProfile 被拒：
  //   "RegisterProfile failed: UUID already registered (org.bluez.Error...)"
  // 实测已发生：CarLife 引导刚起步就死在这里（phase 停在 advertising、running=false）。
  // SPP 的正确发布方式是 ProfileManager1（见 carlife_input.cpp 里的 SppProfileServer）。
  // 保留 sdptool 的存在性探测仅用于日志，不再执行 add。
  if (runQuiet("command -v sdptool >/dev/null 2>&1 && echo present", &out)) {
    if (verbose) std::cout << "[bt] sdptool 存在，但不再使用它发布 SPP（改用 ProfileManager1）" << std::endl;
  }
  if (runQuiet("command -v btmgmt >/dev/null 2>&1 && btmgmt name \"" + name + "\" 2>&1 | tail -1", &out)) {
    any = true;
  }
  if (!any) {
    if (note) *note = "本机没有可用的蓝牙栈（BlueZ 工具不存在或无适配器）";
    return false;
  }
  if (note) *note = "已尽力设置可被发现 + 注册 SPP 服务";
  return true;
}

}  // namespace carlife
