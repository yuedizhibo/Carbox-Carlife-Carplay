#include "carlife/spp_profile.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
#include <fcntl.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#endif

namespace carlife {

#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
namespace {
constexpr const char* kSppUuid = "00001101-0000-1000-8000-00805f9b34fb";
constexpr const char* kProfilePath = "/zero2w/profile/spp";
constexpr const char* kProfileIface = "org.bluez.Profile1";
constexpr const char* kBlueZService = "org.bluez";
constexpr const char* kBlueZManagerPath = "/org/bluez";
constexpr const char* kProfileManagerIface = "org.bluez.ProfileManager1";

uint64_t nowUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

void setError(std::string* err, const char* what, int r) {
  if (!err) return;
  *err = std::string(what) + ": " + strerror(-r);
}
}  // namespace

struct SppProfileServer::Impl {
  sd_bus* bus = nullptr;
  sd_bus_slot* slot = nullptr;
  int channel = 1;
  std::string name;
  int connected_fd = -1;   // 已 dup，归我们所有
  std::string peer;
  bool registered = false;

  // BlueZ 有手机连入 RFCOMM 时回调这里。fd 由消息持有，必须 dup 才能留用。
  static int NewConnection(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<Impl*>(userdata);
    const char* device = nullptr;
    int fd = -1;
    int r = sd_bus_message_read(m, "o", &device);
    if (r < 0) return r;
    r = sd_bus_message_read(m, "h", &fd);
    if (r < 0) return r;
    const int owned = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (owned < 0) return -errno;
    if (self->connected_fd >= 0) ::close(self->connected_fd);
    self->connected_fd = owned;
    self->peer = device ? device : "";
    return sd_bus_reply_method_return(m, nullptr);
  }

  static int RequestDisconnection(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<Impl*>(userdata);
    if (self->connected_fd >= 0) {
      ::close(self->connected_fd);
      self->connected_fd = -1;
    }
    return sd_bus_reply_method_return(m, nullptr);
  }

  static int Release(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, nullptr);
  }

  static const sd_bus_vtable kVtable[];
};

const sd_bus_vtable SppProfileServer::Impl::kVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("NewConnection", "oha{sv}", "", Impl::NewConnection, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestDisconnection", "o", "", Impl::RequestDisconnection,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release", "", "", Impl::Release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};
#endif

SppProfileServer::SppProfileServer() = default;
SppProfileServer::~SppProfileServer() { stop(); }

bool SppProfileServer::start(int channel, const std::string& name, std::string* err) {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  stop();
  impl_ = std::make_unique<Impl>();
  impl_->channel = channel > 0 ? channel : 1;
  impl_->name = name.empty() ? std::string("zero2w Serial Port") : name;

  int r = sd_bus_open_system(&impl_->bus);
  if (r < 0) {
    setError(err, "sd_bus_open_system", r);
    impl_.reset();
    return false;
  }
  r = sd_bus_add_object_vtable(impl_->bus, &impl_->slot, kProfilePath, kProfileIface,
                               Impl::kVtable, impl_.get());
  if (r < 0) {
    setError(err, "sd_bus_add_object_vtable", r);
    stop();
    return false;
  }
  // 带 Channel 注册：bluetoothd 自己监听该 RFCOMM 通道并对外发布 SDP 记录。
  //
  // 【带退避重试】BlueZ 对“同一 UUID 已注册”的判定包含竞态：上一个进程被部署脚本
  // SIGKILL 后，其 profile 注册是【异步】清理的；紧接着注册同一 UUID 会拿到
  // “UUID already registered”（bluetoothd 日志：tried to register … which is already
  // registered）。实测已发生。处理：首次失败时先 UnregisterProfile 清一下同名路径
  //（若本就不是我们注册的，也是无害的），再退避重试。
  r = -1;   // 复用函数开头已声明的 r（重复声明会编译失败）
  std::string last_msg;
  for (int attempt = 0; attempt < 6; ++attempt) {
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(impl_->bus, kBlueZService, kBlueZManagerPath, kProfileManagerIface,
                           "RegisterProfile", &bus_error, nullptr, "osa{sv}", kProfilePath, kSppUuid,
                           3, "Name", "s", impl_->name.c_str(), "Role", "s", "server", "Channel",
                           "q", static_cast<uint16_t>(impl_->channel));
    last_msg = bus_error.message && *bus_error.message ? bus_error.message : strerror(-r);
    if (bus_error.name && *bus_error.name) {
      last_msg += " (";
      last_msg += bus_error.name;
      last_msg += ")";
    }
    sd_bus_error_free(&bus_error);
    if (r >= 0) break;
    if (attempt == 0) {
      sd_bus_error purge = SD_BUS_ERROR_NULL;
      (void)sd_bus_call_method(impl_->bus, kBlueZService, kBlueZManagerPath, kProfileManagerIface,
                               "UnregisterProfile", &purge, nullptr, "o", kProfilePath);
      sd_bus_error_free(&purge);
    }
    usleep(500 * 1000);
  }
  if (r < 0) {
    if (err) *err = "RegisterProfile failed: " + last_msg;
    stop();
    return false;
  }
  impl_->registered = true;
  return true;
#else
  (void)channel;
  (void)name;
  if (err) *err = "built without sd-bus support (CARLIFE_HAVE_SD_BUS)";
  return false;
#endif
}

int SppProfileServer::waitForConnection(int timeoutMs, std::string* err) {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  if (!impl_ || !impl_->bus || !impl_->registered) {
    if (err) *err = "SPP profile is not registered";
    return -1;
  }
  const uint64_t deadline = timeoutMs > 0 ? nowUs() + uint64_t(timeoutMs) * 1000 : 0;
  for (;;) {
    if (impl_->connected_fd >= 0) {
      const int fd = impl_->connected_fd;
      impl_->connected_fd = -1;
      return fd;
    }
    int r = sd_bus_process(impl_->bus, nullptr);
    if (r < 0) {
      setError(err, "sd_bus_process", r);
      return -1;
    }
    if (r > 0) continue;
    uint64_t wait_us = 200000;
    if (deadline) {
      const uint64_t now = nowUs();
      if (now >= deadline) {
        if (err) *err = "timeout waiting for an SPP connection";
        return -1;
      }
      wait_us = std::min<uint64_t>(wait_us, deadline - now);
    }
    r = sd_bus_wait(impl_->bus, wait_us);
    if (r < 0) {
      setError(err, "sd_bus_wait", r);
      return -1;
    }
  }
#else
  (void)timeoutMs;
  if (err) *err = "built without sd-bus support (CARLIFE_HAVE_SD_BUS)";
  return -1;
#endif
}

void SppProfileServer::stop() {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  if (!impl_) return;
  if (impl_->bus && impl_->registered) {
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;
    (void)sd_bus_call_method(impl_->bus, kBlueZService, kBlueZManagerPath, kProfileManagerIface,
                             "UnregisterProfile", &bus_error, nullptr, "o", kProfilePath);
    sd_bus_error_free(&bus_error);
  }
  impl_->slot = sd_bus_slot_unref(impl_->slot);
  if (impl_->bus) impl_->bus = sd_bus_flush_close_unref(impl_->bus);
  if (impl_->connected_fd >= 0) {
    ::close(impl_->connected_fd);
    impl_->connected_fd = -1;
  }
  impl_.reset();
#else
  impl_.reset();
#endif
}

bool SppProfileServer::active() const {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  return impl_ && impl_->registered;
#else
  return false;
#endif
}

}  // namespace carlife
