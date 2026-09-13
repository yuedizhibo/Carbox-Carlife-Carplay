#include "carlife/hfp_slc.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <poll.h>
#include <unistd.h>
#endif

namespace carlife {

#if defined(__linux__)
namespace {

constexpr std::size_t kLogLimit = 4096;

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 有界追加，避免日志无限膨胀（只保留尾部）。
void appendLog(std::string* log, const char* what, const std::string& text) {
  if (!log) return;
  *log += what;
  *log += text;
  if (log->size() > kLogLimit) log->erase(0, log->size() - kLogLimit);
}

bool writeAll(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
    if (n > 0) {
      sent += std::size_t(n);
      continue;
    }
    if (n < 0 && (errno == EINTR)) continue;
    return false;
  }
  return true;
}

// 读一行（以 \r\n 或 \n 结束），带超时。返回 false 表示超时/出错。
// 行内容写入 out（不含结尾 CR/LF）。
bool readLine(int fd, int timeout_ms, std::string* out, bool* peer_closed) {
  *peer_closed = false;
  out->clear();
  for (;;) {
    char ch = 0;
    const ssize_t n = ::read(fd, &ch, 1);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, timeout_ms);
        if (r > 0) continue;
        return false;   // 超时
      }
      return false;
    }
    if (n == 0) {
      *peer_closed = true;
      return false;   // 对端关闭
    }
    if (ch == '\n') {
      if (!out->empty() && out->back() == '\r') out->pop_back();
      return true;
    }
    out->push_back(ch);
    if (out->size() > 512) return false;   // 异常长行，防跑飞
  }
}

// 发送一条 AT 指令（自动补 \r），并把后续所有行读到 OK/ERROR。
// 中间收到的 +XXX: 应答行全部记录进 log。
bool command(int fd, const std::string& at, int timeout_ms, std::string* err, std::string* log) {
  const std::string line = at + "\r";
  if (!writeAll(fd, line)) {
    if (err) *err = "写失败: " + at;
    return false;
  }
  appendLog(log, "  >> ", at);
  for (;;) {
    std::string response;
    bool closed = false;
    if (!readLine(fd, timeout_ms, &response, &closed)) {
      if (err) {
        *err = closed ? ("对端在应答 " + at + " 前关闭了连接")
                      : ("等待 " + at + " 的应答超时");
      }
      return false;
    }
    if (response.empty()) continue;
    appendLog(log, "\n  << ", response);
    // 规范里 OK / ERROR 都是大写，但手机实现大小写不一，统一转大写比较。
    std::string upper;
    upper.reserve(response.size());
    for (char c : response) upper.push_back(char(std::toupper(static_cast<unsigned char>(c))));
    if (upper.rfind("OK", 0) == 0) return true;
    if (upper.find("ERROR") != std::string::npos) {
      if (err) *err = "对 " + at + " 应答 ERROR";
      return false;
    }
    // +XXX: 中间应答，继续读
  }
}

}  // namespace

bool runHfServiceLevelConnection(int fd, uint16_t hf_features, int timeout_ms, std::string* err,
                                 std::string* log) {
  if (fd < 0) {
    if (err) *err = "无效的 fd";
    return false;
  }
  if (log) *log += "[HFP SLC] 开始\n";

  struct Step {
    const char* name;
    std::string at;
    bool required;
  };
  const std::string brsf = "AT+BRSF=" + std::to_string(static_cast<unsigned>(hf_features));
  const Step steps[] = {
      {"BRSF 能力交换", brsf, true},
      {"CIND 指示符查询", "AT+CIND=?", true},
      {"CIND 当前值", "AT+CIND?", true},
      {"CMER 启用指示", "AT+CMER=3,0,0,1", true},
      {"CHLD 呼叫保持能力", "AT+CHLD=?", true},
      // 编解码协商（HFP 1.6+）：1=CVSD, 2=mSBC。失败不致命（对端可能不支持）。
      {"BAC 编解码", "AT+BAC=1,2", false},
  };

  for (const auto& step : steps) {
    std::string step_err;
    const bool ok = command(fd, step.at, timeout_ms, &step_err, log);
    if (!ok) {
      if (step.required) {
        if (err) *err = std::string("SLC 卡在「") + step.name + "」: " + step_err;
        if (log) *log += std::string("\n[HFP SLC] 失败于 ") + step.name + "\n";
        return false;
      }
      // 可选步骤失败：记录后继续。
      appendLog(log, "\n  (可选步骤跳过) ", step_err);
    }
  }
  if (log) *log += "\n[HFP SLC] 建立完成\n";
  return true;
}

void sniffLink(int fd, int duration_ms, std::string* log) {
  if (fd < 0) return;
  const int64_t deadline = nowMs() + static_cast<int64_t>(duration_ms);
  std::string hex;
  std::string txt;
  std::size_t total = 0;
  auto flush = [&]() {
    if (hex.empty()) return;
    appendLog(log, "\n  << [", hex + "]  ascii=\"" + txt + "\"");
    hex.clear();
    txt.clear();
  };
  while (nowMs() < deadline) {
    pollfd p{fd, POLLIN, 0};
    const int r = ::poll(&p, 1, 200);
    if (r < 0) break;
    if (r == 0) continue;
    char buf[512];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break;
    }
    if (n == 0) {
      flush();
      appendLog(log, "\n  (对端关闭了链路)", "");
      return;
    }
    for (ssize_t i = 0; i < n; ++i) {
      const unsigned char c = static_cast<unsigned char>(buf[i]);
      char h[4];
      std::snprintf(h, sizeof(h), "%02x ", c);
      hex += h;
      txt += (c >= 0x20 && c < 0x7f) ? char(c) : '.';
    }
    total += std::size_t(n);
    if (hex.size() >= 512) flush();   // 分段刷新，避免长时间无输出
  }
  flush();
  char summary[96];
  std::snprintf(summary, sizeof(summary), "\n  (观测结束，共收到 %zu 字节)", total);
  appendLog(log, summary, "");
}

#else

bool runHfServiceLevelConnection(int fd, uint16_t hf_features, int timeout_ms, std::string* err,
                                 std::string* log) {
  (void)fd;
  (void)hf_features;
  (void)timeout_ms;
  (void)log;
  if (err) *err = "HFP SLC 仅在 Linux 上实现";
  return false;
}

void sniffLink(int fd, int duration_ms, std::string* log) {
  (void)fd;
  (void)duration_ms;
  (void)log;
}

#endif

}  // namespace carlife
