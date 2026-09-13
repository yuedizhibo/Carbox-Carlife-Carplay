#include "carlife/pairing_agent.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
#include <systemd/sd-bus.h>
#endif

namespace carlife {

#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
namespace {
constexpr const char* kAgentPath = "/zero2w/agent/pairing";
constexpr const char* kAgentIface = "org.bluez.Agent1";
constexpr const char* kBlueZService = "org.bluez";
constexpr const char* kManagerPath = "/org/bluez";
constexpr const char* kManagerIface = "org.bluez.AgentManager1";
// KeyboardDisplay：既能确认数字比对（同意 PIN），也能在 legacy 配对时给出 PIN。
// NoInputNoOutput 在 bluez>=5.55 上不可用（见头文件注释）。
constexpr const char* kCapability = "KeyboardDisplay";

void setError(std::string* err, const char* what, int r) {
  if (!err) return;
  *err = std::string(what) + ": " + strerror(-r);
}
}  // namespace

struct PairingAgent::Impl {
  sd_bus* bus = nullptr;
  sd_bus_slot* slot = nullptr;
  std::string pin{"0000"};
  bool registered = false;
  // 【关键】BlueZ 回调 agent 的过程发生在这条【自己的】D-Bus 连接上。
  // 它必须被持续处理（sd_bus_process/sd_bus_wait），否则配对请求
  // （RequestConfirmation / RequestPinCode）永远得不到应答 ——
  // 手机端表现为「PIN 不正确 / 一直连接中」。
  // 板上实测：不加这个线程时，/zero2w/agent/pairing 根本不出现在我们的总线名下。
  std::thread worker;
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> confirmations{0};
  std::atomic<uint64_t> pin_requests{0};
  std::string last_device;

  static int readDevice(sd_bus_message* m, const char** device) {
    return sd_bus_message_read(m, "o", device);
  }

  // 同意：这是「同意 PIN」的核心回调。手机显示 6 位数要求比对时走到这里。
  static int RequestConfirmation(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<Impl*>(userdata);
    const char* device = nullptr;
    uint32_t passkey = 0;
    int r = readDevice(m, &device);
    if (r < 0) return r;
    r = sd_bus_message_read(m, "u", &passkey);
    if (r < 0) return r;
    self->confirmations.fetch_add(1);
    if (device) self->last_device = device;
    return sd_bus_reply_method_return(m, nullptr);   // 同意
  }

  static int RequestAuthorization(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<Impl*>(userdata);
    const char* device = nullptr;
    int r = readDevice(m, &device);
    if (r < 0) return r;
    self->confirmations.fetch_add(1);
    if (device) self->last_device = device;
    return sd_bus_reply_method_return(m, nullptr);   // 同意
  }

  static int AuthorizeService(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<Impl*>(userdata);
    const char* device = nullptr;
    const char* uuid = nullptr;
    int r = readDevice(m, &device);
    if (r < 0) return r;
    r = sd_bus_message_read(m, "s", &uuid);
    if (r < 0) return r;
    self->confirmations.fetch_add(1);
    if (device) self->last_device = device;
    (void)uuid;
    return sd_bus_reply_method_return(m, nullptr);   // 同意
  }

  // legacy 配对：手机要 PIN 时给出固定 PIN，而不是拒绝。
  static int RequestPinCode(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<Impl*>(userdata);
    const char* device = nullptr;
    int r = readDevice(m, &device);
    if (r < 0) return r;
    self->pin_requests.fetch_add(1);
    if (device) self->last_device = device;
    return sd_bus_reply_method_return(m, "s", self->pin.c_str());
  }

  static int RequestPasskey(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<Impl*>(userdata);
    const char* device = nullptr;
    int r = readDevice(m, &device);
    if (r < 0) return r;
    self->pin_requests.fetch_add(1);
    if (device) self->last_device = device;
    return sd_bus_reply_method_return(m, "u", 0u);
  }

  // 显示类回调：我们无屏，直接接受（不做任何输出）。
  static int DisplayPinCode(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, nullptr);
  }
  static int DisplayPasskey(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, nullptr);
  }
  static int Release(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, nullptr);
  }
  static int Cancel(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, nullptr);
  }

  static const sd_bus_vtable kVtable[];
};

const sd_bus_vtable PairingAgent::Impl::kVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", Impl::Release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode", "o", "s", Impl::RequestPinCode, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode", "os", "", Impl::DisplayPinCode, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", Impl::RequestPasskey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", Impl::DisplayPasskey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", Impl::RequestConfirmation,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", Impl::RequestAuthorization,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService", "os", "", Impl::AuthorizeService, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", Impl::Cancel, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};
#endif

PairingAgent::PairingAgent() = default;
PairingAgent::~PairingAgent() { stop(); }

bool PairingAgent::start(const std::string& pin, std::string* err) {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  stop();
  impl_ = std::make_unique<Impl>();
  if (!pin.empty()) impl_->pin = pin;

  int r = sd_bus_open_system(&impl_->bus);
  if (r < 0) {
    setError(err, "sd_bus_open_system", r);
    impl_.reset();
    return false;
  }
  r = sd_bus_add_object_vtable(impl_->bus, &impl_->slot, kAgentPath, kAgentIface, Impl::kVtable,
                               impl_.get());
  if (r < 0) {
    setError(err, "sd_bus_add_object_vtable(agent)", r);
    stop();
    return false;
  }

  sd_bus_error bus_error = SD_BUS_ERROR_NULL;
  r = sd_bus_call_method(impl_->bus, kBlueZService, kManagerPath, kManagerIface, "RegisterAgent",
                         &bus_error, nullptr, "os", kAgentPath, kCapability);
  if (r < 0) {
    if (err) {
      *err = "RegisterAgent failed: ";
      *err += bus_error.message && *bus_error.message ? bus_error.message : strerror(-r);
    }
    sd_bus_error_free(&bus_error);
    stop();
    return false;
  }
  // 抢占默认 agent：与 bt-agent 共存时以我们为准。
  sd_bus_error bus_error2 = SD_BUS_ERROR_NULL;
  (void)sd_bus_call_method(impl_->bus, kBlueZService, kManagerPath, kManagerIface,
                           "RequestDefaultAgent", &bus_error2, nullptr, "o", kAgentPath);
  sd_bus_error_free(&bus_error2);
  sd_bus_error_free(&bus_error);
  impl_->registered = true;

  // 启动专职线程处理本连接上的消息（agent 回调）。
  impl_->worker = std::thread([impl = impl_.get()] {
    while (!impl->stopping.load()) {
      const int r = sd_bus_process(impl->bus, nullptr);
      if (r < 0) break;
      if (r > 0) continue;                       // 还有待处理消息，立刻继续
      (void)sd_bus_wait(impl->bus, 200000);       // 最长 200ms，保证能及时退出
    }
  });
  return true;
#else
  (void)pin;
  if (err) *err = "built without sd-bus support (CARLIFE_HAVE_SD_BUS)";
  return false;
#endif
}

void PairingAgent::stop() {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  if (!impl_) return;
  // 【顺序很重要】先停线程并 join，再动总线对象 —— 保证同一时刻只有一个线程使用 bus。
  impl_->stopping.store(true);
  if (impl_->worker.joinable()) impl_->worker.join();
  if (impl_->bus && impl_->registered) {
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;
    (void)sd_bus_call_method(impl_->bus, kBlueZService, kManagerPath, kManagerIface,
                             "UnregisterAgent", &bus_error, nullptr, "o", kAgentPath);
    sd_bus_error_free(&bus_error);
  }
  impl_->slot = sd_bus_slot_unref(impl_->slot);
  if (impl_->bus) impl_->bus = sd_bus_flush_close_unref(impl_->bus);
  impl_.reset();
#else
  impl_.reset();
#endif
}

bool PairingAgent::active() const {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  return impl_ && impl_->registered;
#else
  return false;
#endif
}

uint64_t PairingAgent::confirmations() const {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  return impl_ ? impl_->confirmations.load() : 0;
#else
  return 0;
#endif
}

uint64_t PairingAgent::pin_requests() const {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  return impl_ ? impl_->pin_requests.load() : 0;
#else
  return 0;
#endif
}

std::string PairingAgent::last_device() const {
#if defined(__linux__) && defined(CARLIFE_HAVE_SD_BUS)
  return impl_ ? impl_->last_device : std::string();
#else
  return {};
#endif
}

}  // namespace carlife
