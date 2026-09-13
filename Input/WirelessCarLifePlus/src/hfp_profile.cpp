#include "carlife/hfp_profile.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
#include <errno.h>
#include <fcntl.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#endif

namespace carlife {

#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
namespace {
// HFP Hands-Free unit（车机角色）。对照 SPP 的 1101，这里是 CarLife 认车机的关键 UUID。
constexpr const char* kHfpHsUuid = "0000111e-0000-1000-8000-00805f9b34fb";
constexpr const char* kProfilePath = "/zero2w/profile/hfp";
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

struct HfpProfileServer::Impl {
  sd_bus* bus = nullptr;
  sd_bus_slot* slot = nullptr;
  int channel = 7;          // BlueZ 对 HFP HS 的默认 RFCOMM 通道
  uint16_t features = 0x007f;
  uint16_t version = 0x0107;  // HFP 1.7
  std::string name;
  int connected_fd = -1;    // 已 dup，归我们所有
  std::string peer;
  bool registered = false;

  // 手机连入 HFP RFCOMM 时由 bluetoothd 回调。fd 由消息持有，必须 dup 才能留用。
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

const sd_bus_vtable HfpProfileServer::Impl::kVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("NewConnection", "oha{sv}", "", Impl::NewConnection, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestDisconnection", "o", "", Impl::RequestDisconnection,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release", "", "", Impl::Release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};
#endif

HfpProfileServer::HfpProfileServer() = default;
HfpProfileServer::~HfpProfileServer() { stop(); }

bool HfpProfileServer::start(int channel, const std::string& name, uint16_t features,
                             uint16_t version, std::string* err) {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  stop();
  impl_ = std::make_unique<Impl>();
  impl_->channel = channel > 0 ? channel : 7;
  impl_->name = name.empty() ? std::string("zero2w Handsfree") : name;
  impl_->features = features;
  impl_->version = version;

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
  sd_bus_error bus_error = SD_BUS_ERROR_NULL;
  // 带 Channel 注册：bluetoothd 自己监听该 RFCOMM 通道并对外发布 SDP 记录。
  // Version/Features 会写进 SDP 的 profile descriptor / SupportedFeatures 属性，
  // 使本机在手机看来是一台完整的免提车载套件（HFP HS）。
  r = sd_bus_call_method(impl_->bus, kBlueZService, kBlueZManagerPath, kProfileManagerIface,
                         "RegisterProfile", &bus_error, nullptr, "osa{sv}", kProfilePath, kHfpHsUuid,
                         5, "Name", "s", impl_->name.c_str(), "Role", "s", "server", "Channel", "q",
                         static_cast<uint16_t>(impl_->channel), "Version", "q", impl_->version,
                         "Features", "q", impl_->features);
  if (r < 0) {
    if (err) {
      *err = "RegisterProfile(HFP HS) failed: ";
      *err += bus_error.message && *bus_error.message ? bus_error.message : strerror(-r);
      if (bus_error.name && *bus_error.name) {
        *err += " (";
        *err += bus_error.name;
        *err += ")";
      }
    }
    sd_bus_error_free(&bus_error);
    stop();
    return false;
  }
  sd_bus_error_free(&bus_error);
  impl_->registered = true;
  return true;
#else
  (void)channel;
  (void)name;
  (void)features;
  (void)version;
  if (err) *err = "built without sd-bus support (CARLIFE_HAVE_SD_BUS)";
  return false;
#endif
}

int HfpProfileServer::waitForConnection(int timeoutMs, std::string* err) {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  if (!impl_ || !impl_->bus || !impl_->registered) {
    if (err) *err = "HFP profile is not registered";
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
        if (err) *err = "timeout waiting for an HFP connection";
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

void HfpProfileServer::stop() {
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

bool HfpProfileServer::active() const {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  return impl_ && impl_->registered;
#else
  return false;
#endif
}

std::string HfpProfileServer::peer() const {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  return impl_ ? impl_->peer : std::string();
#else
  return {};
#endif
}

}  // namespace carlife
