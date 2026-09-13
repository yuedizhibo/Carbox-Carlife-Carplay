#include "carlife/bt_client_profile.h"

#include <chrono>
#include <cstdint>
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
constexpr const char* kProfilePath = "/zero2w/profile/btclient";
constexpr const char* kProfileIface = "org.bluez.Profile1";
constexpr const char* kBlueZService = "org.bluez";
constexpr const char* kManagerPath = "/org/bluez";
constexpr const char* kManagerIface = "org.bluez.ProfileManager1";
constexpr const char* kDeviceIface = "org.bluez.Device1";

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string upper(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;
}
}  // namespace

struct BtClientProfile::Impl {
  sd_bus* bus = nullptr;
  sd_bus_slot* slot = nullptr;
  std::string uuid;
  bool registered = false;
  int connected_fd = -1;   // 已 dup，归我们所有

  // BlueZ 连上远端服务后回调这里，把 fd 递过来（与 SPP 服务端同一形态）。
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
    return sd_bus_reply_method_return(m, nullptr);
  }

  static int RequestDisconnection(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, nullptr);
  }
  static int Release(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, nullptr);
  }

  static const sd_bus_vtable kVtable[];
};

const sd_bus_vtable BtClientProfile::Impl::kVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("NewConnection", "oha{sv}", "", Impl::NewConnection, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestDisconnection", "o", "", Impl::RequestDisconnection,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release", "", "", Impl::Release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};
#endif

BtClientProfile::BtClientProfile() = default;
BtClientProfile::~BtClientProfile() { stop(); }

bool BtClientProfile::registerClient(const std::string& uuid, std::string* err) {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  if (impl_ && impl_->registered && impl_->uuid == uuid) return true;
  stop();
  impl_ = std::make_unique<Impl>();
  impl_->uuid = uuid;

  int r = sd_bus_open_system(&impl_->bus);
  if (r < 0) {
    if (err) *err = std::string("sd_bus_open_system: ") + strerror(-r);
    impl_.reset();
    return false;
  }
  r = sd_bus_add_object_vtable(impl_->bus, &impl_->slot, kProfilePath, kProfileIface,
                               Impl::kVtable, impl_.get());
  if (r < 0) {
    if (err) *err = std::string("sd_bus_add_object_vtable: ") + strerror(-r);
    stop();
    return false;
  }
  // 关键：Role=client（不传 Channel）。BlueZ 会自己去做 SDP 解析、自己挑通道。
  sd_bus_error bus_error = SD_BUS_ERROR_NULL;
  r = sd_bus_call_method(impl_->bus, kBlueZService, kManagerPath, kManagerIface,
                         "RegisterProfile", &bus_error, nullptr, "osa{sv}", kProfilePath,
                         uuid.c_str(), 2, "Name", "s", "zero2w CarLife BT client", "Role", "s",
                         "client");
  if (r < 0) {
    if (err) {
      *err = "RegisterProfile(client) failed: ";
      *err += bus_error.message && *bus_error.message ? bus_error.message : strerror(-r);
    }
    sd_bus_error_free(&bus_error);
    stop();
    return false;
  }
  sd_bus_error_free(&bus_error);
  impl_->registered = true;
  return true;
#else
  (void)uuid;
  if (err) *err = "built without sd-bus support (CARLIFE_HAVE_SD_BUS)";
  return false;
#endif
}

int BtClientProfile::connectProfile(const std::string& devicePath, const std::string& uuid,
                                    int timeoutMs, std::string* err) {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  if (!impl_ || !impl_->bus || !impl_->registered || impl_->uuid != uuid) {
    if (err) *err = "client profile 未按该 UUID 注册";
    return -1;
  }
  sd_bus_error bus_error = SD_BUS_ERROR_NULL;
  const int r = sd_bus_call_method(impl_->bus, kBlueZService, devicePath.c_str(), kDeviceIface,
                                   "ConnectProfile", &bus_error, nullptr, "s", uuid.c_str());
  if (r < 0) {
    if (err) {
      *err = "ConnectProfile failed: ";
      *err += bus_error.message && *bus_error.message ? bus_error.message : strerror(-r);
    }
    sd_bus_error_free(&bus_error);
    return -1;
  }
  sd_bus_error_free(&bus_error);

  // 等 BlueZ 通过 Profile1.NewConnection 把 fd 递过来。
  const int64_t deadline = nowMs() + (timeoutMs > 0 ? timeoutMs : 8000);
  for (;;) {
    if (impl_->connected_fd >= 0) {
      const int fd = impl_->connected_fd;
      impl_->connected_fd = -1;
      return fd;
    }
    const int pr = sd_bus_process(impl_->bus, nullptr);
    if (pr < 0) {
      if (err) *err = std::string("sd_bus_process: ") + strerror(-pr);
      return -1;
    }
    if (pr > 0) continue;
    const int64_t left = deadline - nowMs();
    if (left <= 0) {
      if (err) *err = "等待 NewConnection 超时";
      return -1;
    }
    (void)sd_bus_wait(impl_->bus, static_cast<uint64_t>(left < 200 ? left : 200) * 1000);
  }
#else
  (void)devicePath;
  (void)uuid;
  (void)timeoutMs;
  if (err) *err = "built without sd-bus support (CARLIFE_HAVE_SD_BUS)";
  return -1;
#endif
}

bool BtClientProfile::deviceUuids(const std::string& devicePath, std::vector<std::string>* out,
                                  std::string* err) {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  sd_bus* bus = nullptr;
  int r = sd_bus_open_system(&bus);
  if (r < 0) {
    if (err) *err = std::string("sd_bus_open_system: ") + strerror(-r);
    return false;
  }
  sd_bus_error bus_error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  r = sd_bus_call_method(bus, kBlueZService, devicePath.c_str(), "org.freedesktop.DBus.Properties",
                         "Get", &bus_error, &reply, "ss", kDeviceIface, "UUIDs");
  if (r < 0) {
    if (err) {
      *err = "取 Device1.UUIDs 失败: ";
      *err += bus_error.message && *bus_error.message ? bus_error.message : strerror(-r);
    }
    sd_bus_error_free(&bus_error);
    sd_bus_unref(bus);
    return false;
  }
  sd_bus_error_free(&bus_error);
  // 返回类型是 variant → 里面是 as
  r = sd_bus_message_enter_container(reply, 'v', "as");
  if (r >= 0) {
    const char* one = nullptr;
    while ((r = sd_bus_message_read(reply, "s", &one)) > 0) {
      if (out && one) out->push_back(upper(one));
    }
    (void)sd_bus_message_exit_container(reply);
  }
  sd_bus_message_unref(reply);
  sd_bus_unref(bus);
  return out ? !out->empty() : true;
#else
  (void)devicePath;
  (void)out;
  if (err) *err = "built without sd-bus support (CARLIFE_HAVE_SD_BUS)";
  return false;
#endif
}

std::string BtClientProfile::devicePathForMac(const std::string& mac) {
  std::string name = upper(mac);
  for (char& c : name) {
    if (c == ':') c = '_';
  }
  return "/org/bluez/hci0/dev_" + name;
}

void BtClientProfile::stop() {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  if (!impl_) return;
  if (impl_->bus && impl_->registered) {
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;
    (void)sd_bus_call_method(impl_->bus, kBlueZService, kManagerPath, kManagerIface,
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

bool BtClientProfile::active() const {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  return impl_ && impl_->registered;
#else
  return false;
#endif
}

}  // namespace carlife
