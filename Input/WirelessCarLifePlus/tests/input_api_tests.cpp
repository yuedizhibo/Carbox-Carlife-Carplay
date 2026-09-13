// 输入层四个「接口集成」缺陷的回归测试（契约：Temp/pi-carlife-contract.md）。
//
// 为什么单独一个可执行文件、并且是纯内存测试：
//   * 这四处缺陷都不是编解码问题（编解码在 unit_features 里已覆盖），而是
//     “函数真的调用了没有”的接线问题：校验缝没被用、能力位写死、模块状态只打日志、
//     麦克风源没有落点。要证明修好了，必须【真的调用】这些函数并断言可观察结果；
//   * 因此这里不起会话、不连 socket、不碰蓝牙、不需要任何硬件；
//   * 只用 heap 上的 SessionCore / RealMediaStore 构造真实的 CarLifeInputAdapter，
//     并通过【基类 HostSink&】调用 —— 这样断言的是多态覆盖本身，而不是某个具名函数。
//
// 覆盖：
//   1. Session::verifyAuth —— seam 调用参数、true/false/throw、nullptr 时的 trust/dev/deny
//   2. Session::buildFeatureConfig —— 内容加密与多点触控的组合、其余能力位不动
//   3. Session::handleModuleStatus —— 合法/畸形载荷、有无宿主机
//   4. CarLifeInputAdapter::takeMicrophone —— 有效样本、缺源/false/0 帧/越界/抛异常
//      以及 Session 侧对“宿主机回报帧数越界”的防御（takeMicFrame）
//   5. CarLifeInputAdapter::onEncryption —— Off/Advertised/KeyReceived/Ready/未知态 → 0/1/1/2/1
//      （经真实 Core 的 LinkState 读回，输入走 HostSink& 虚调用）
//   6. on_control 语音键 —— Down/Move → VR_START、Up → VR_STOP
//   7. on_control 旋钮 —— 方向选键、步数展开成重复键、Press 恒一次、0 格不发、
//      >32 格原子拒绝、-32768 不溢出、队列没空间时整批拒绝且不动既有控制
//   8. on_control DTMF —— 逐位下发 0..9 与 * #、非法字符/缺 NUL 终止/超长/没空间整条拒绝
//   9. 麦克风准备握手 —— PREPARE_START 经宿主机回调决定是否回 PREPARE_DONE，
//      失败/异常/无宿主都不回，且绝不打开录音状态
//  10. 命令型回传 —— 转前台（0x18025，空载荷）与模块控制（0x18028，singular
//      CarlifeModuleStatus）的入队边界（非法 id/负状态/未连接/队列满）与线上字节
//      [08,01,10,02]（不是 ModuleStatusList）
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "carlife/carlife_input.h"
#include "carlife/encryption.h"
#include "carlife/service_types.h"
#include "carlife/session.h"
#include "carlife/wire.h"

namespace {

int g_pass = 0;
int g_fail = 0;

// 自定义 check：返回失败计数，而不是 assert（Release 下 assert 会被去掉）。
void check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
    std::printf("PASS  %s\n", what);
  } else {
    ++g_fail;
    std::printf("FAIL  %s\n", what);
  }
}

// session.cpp 里 dev 模式的摘要算法（匿名命名空间，不在头文件里）：FNV-1a → "sim-%016llx"。
// dev 模式是我们自己的模拟器路径，要在测试里构造一个“应当通过”的值就必须镜像它；
// 这里只用来驱动 dev 分支，不参与任何真机判定。
std::string dev_digest(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[24];
  std::snprintf(buf, sizeof(buf), "sim-%016llx", static_cast<unsigned long long>(h));
  return buf;
}

}  // namespace

namespace carlife {

// ── Session 私有入口的测试访问器（session.h 里已声明为 friend）──
// 只转调被验证的那几个入口；不把会话内部状态改成 public，也不为测试新增运行期 API。
struct SessionApiTestAccess {
  static bool verify(Session& s, const std::string& seed, const std::string& value) {
    return s.verifyAuth(seed, value);
  }
  static pb::FeatureConfigList build_features(Session& s) { return s.buildFeatureConfig(); }
  static bool ensure_keypair(Session& s) { return s.cipher_.ensure_keypair(); }
  static bool has_keypair(Session& s) { return s.cipher_.has_keypair(); }
  static void frame(Session& s, uint32_t channel, uint32_t type,
                    std::vector<uint8_t> payload = {}) {
    Frame f; f.channel = channel; f.serviceType = type; f.payload = std::move(payload);
    s.handleFrame(f);
  }
  static bool recording(const Session& s) { return s.mic_recording_; }
  static void poll_mic(Session& s) { s.pollMicrophone(); }
  static std::size_t mic_capacity(const Session& s) { return s.micBuffer_.size(); }
  static bool take_mic_frame(Session& s, std::vector<uint8_t>* payload) {
    return s.takeMicFrame(payload);
  }
  static void module_status(Session& s, const std::vector<uint8_t>& payload) {
    Frame f;
    f.channel = ch::CMD;
    f.serviceType = msg::MODULE_STATUS;
    f.payload = payload;
    s.handleModuleStatus(f);
  }
  // 麦克风准备握手（0x10071 → 0x18072）：见 session.cpp 的 handleMicRecordPrepare。
  static void mic_prepare_start(Session& s) { frame(s, ch::CMD, msg::MIC_RECORD_PREPARE_START); }
  // “决定回 PREPARE_DONE”的次数（无 socket 时可观察的唯一出站判据）。
  static uint64_t prepare_done_attempts(const Session& s) {
    return s.micPrepareDoneAttempts_.load();
  }
};

// ── 校验缝的三种实现：判真 / 判假 / 抛异常 ──
struct SeamTrue final : VerifySeam {
  int calls = 0;
  std::string seed, value;
  bool verify(const std::string& s, const std::string& v) override {
    ++calls;
    seed = s;
    value = v;
    return true;
  }
  const char* name() const override { return "test-true"; }
};
struct SeamFalse final : VerifySeam {
  int calls = 0;
  bool verify(const std::string&, const std::string&) override {
    ++calls;
    return false;
  }
  const char* name() const override { return "test-false"; }
};
struct SeamThrow final : VerifySeam {
  bool verify(const std::string&, const std::string&) override {
    throw std::runtime_error("seam boom");
  }
  const char* name() const override { return "test-throw"; }
};
struct SeamThrowUnknown final : VerifySeam {
  bool verify(const std::string&, const std::string&) override { throw 42; }
  const char* name() const override { return "test-throw-any"; }
};

// ── 只关心 onModuleStatus 的宿主机 ──
struct ModuleSink final : HostSink {
  int calls = 0;
  std::string detail;
  void onModuleStatus(const std::string& d) override {
    ++calls;
    detail = d;
  }
};

// ── 只做上行麦克风的宿主机（直接验证 Session 的容量防御，绕过适配器）──
struct MicSink final : HostSink {
  int calls = 0;
  std::size_t capacity_seen = 0;
  std::size_t frames_to_report = 0;
  bool accept = true;
  bool takeMicrophone(int16_t* dst, std::size_t capacity, std::size_t& frames_out) override {
    ++calls;
    capacity_seen = capacity;
    if (!accept) return false;
    frames_out = frames_to_report;
    if (frames_to_report && frames_to_report <= capacity) {
      for (std::size_t i = 0; i < frames_to_report; ++i) {
        dst[i] = static_cast<int16_t>(1000 + static_cast<int>(i));
      }
    }
    return true;
  }
};

// ── 注入用的麦克风源：可控的“好/坏”两种行为 ──
struct MicSourceHarness {
  int calls = 0;
  std::size_t capacity_seen = 0;
  std::vector<int16_t> samples{11, 22, 33};
  bool result = true;      // 源自己报的成功/失败
  bool report_zero = false;  // 故意报 0 帧
  bool overflow = false;     // 故意报一个超过容量的帧数
  bool throw_now = false;    // 故意抛异常
  MicrophoneSource fn() {
    return [this](int16_t* dst, std::size_t capacity, std::size_t& out_frames) -> bool {
      ++calls;
      capacity_seen = capacity;
      if (throw_now) throw std::runtime_error("mic source boom");
      if (!result) return false;
      std::size_t n = report_zero ? 0 : samples.size();
      if (overflow) n = capacity + 7;
      // 源自己遵守契约：最多写 capacity 个样本（越界帧数只体现在 out_frames 上）。
      const std::size_t write = (n < capacity) ? n : capacity;
      for (std::size_t i = 0; i < write; ++i) {
        dst[i] = samples[i % samples.size()];
      }
      out_frames = n;
      return true;
    };
  }
};

// ── 准备握手的宿主机：可控地“做完 / 没做 / 抛异常” ──
struct PrepareSink final : HostSink {
  int calls = 0;
  bool result = false;
  bool throw_now = false;
  bool prepareMicrophone() override {
    ++calls;
    if (throw_now) throw std::runtime_error("prepare boom");
    return result;
  }
};
struct PrepareThrowUnknown final : HostSink {
  int calls = 0;
  bool prepareMicrophone() override {
    ++calls;
    throw 7;
  }
};

// 把适配器里已排队的控制全部按 FIFO 取出（断言“发了什么/发了几条”）。
std::vector<carlife::HostControl> drain(HostSink& sink) {
  std::vector<carlife::HostControl> out;
  carlife::HostControl c;
  while (sink.takeControl(c)) out.push_back(c);
  return out;
}

// 只保留 keycode（非 Key 类型记 -1），方便直接跟期望序列比。
std::vector<int32_t> keycodes(const std::vector<carlife::HostControl>& controls) {
  std::vector<int32_t> out;
  out.reserve(controls.size());
  for (const auto& c : controls) {
    out.push_back(c.type == carlife::HostControl::Type::Key ? c.keycode : -1);
  }
  return out;
}

// 向适配器塞满 N 条单点触控（用 Down 相，避免被 Move 合并）。
void fill_touches(carlife::CarLifeInputAdapter& adapter, int count) {
  for (int i = 0; i < count; ++i) {
    mvp::ControlEvent ev;
    ev.type = mvp::ControlEvent::Type::Touch;
    ev.phase = mvp::ControlEvent::TouchPhase::Down;
    ev.x = i + 1;
    ev.y = 1;
    adapter.on_control(ev);
  }
}

void knob_event(carlife::CarLifeInputAdapter& adapter, mvp::ControlEvent::KnobDir dir,
                int16_t steps) {
  mvp::ControlEvent ev;
  ev.type = mvp::ControlEvent::Type::Knob;
  ev.knob_dir = dir;
  ev.knob_steps = steps;
  adapter.on_control(ev);
}

// DTMF：数字串写进 dtmf，可选键名写进 key（都按定长数组的真实容量写，保证有 NUL）。
void dtmf_event(carlife::CarLifeInputAdapter& adapter, const char* digits,
                const char* key_name = "") {
  mvp::ControlEvent ev;
  ev.type = mvp::ControlEvent::Type::Telephony;
  std::snprintf(ev.dtmf.data(), ev.dtmf.size(), "%s", digits);
  std::snprintf(ev.key.data(), ev.key.size(), "%s", key_name);
  adapter.on_control(ev);
}

// 真实的适配器宿主机：heap 上的 SessionCore + RealMediaStore + CarLifeInputAdapter。
struct AdapterHarness {
  std::unique_ptr<mvp::SessionCore> core{std::make_unique<mvp::SessionCore>()};
  std::unique_ptr<mvp::RealMediaStore> media{std::make_unique<mvp::RealMediaStore>()};
  std::unique_ptr<CarLifeInputAdapter> adapter;
  explicit AdapterHarness(SessionConfig cfg) {
    adapter = std::make_unique<CarLifeInputAdapter>(*core, *media, std::move(cfg));
  }
  HostSink& sink() { return *adapter; }
};

}  // namespace carlife

namespace {

std::vector<uint8_t> module_payload(const std::vector<std::pair<int32_t, int32_t>>& items) {
  carlife::PbWriter w;
  for (const auto& it : items) {
    carlife::PbWriter sub;
    sub.fieldInt32(1, it.first);
    sub.fieldInt32(2, it.second);
    w.fieldMessage(2, sub);
  }
  return w.data();
}

int32_t feature_value(const carlife::pb::FeatureConfigList& list, const char* key, bool* found) {
  for (const auto& c : list.configs) {
    if (c.key == key) {
      *found = true;
      return c.value;
    }
  }
  *found = false;
  return -1;
}

}  // namespace

int main() {
  {
    carlife::SdkPhoneVerifier verifier;
    check(verifier.verify("seed-x", "1bd204e2f7888c7667d664146b37a111"), "SDK MD5 reference vector");
    check(verifier.verify("XiaomiMi113112:34:56", "b0c6a9a4ba2d496b4694c44a954deb27"), "SDK device/time seed");
    check(verifier.verify("\xf0\x9f\x98\x80", "396a311fcb4f37bb613ed8d4e975399f"), "SDK JNI modified UTF-8 supplementary scalar");
    check(!verifier.verify("seed-y", "1bd204e2f7888c7667d664146b37a111"), "wrong device rejected");
    check(!verifier.verify("seed-x", "1BD204E2F7888C7667D664146B37A111"), "SDK exact lowercase comparison");
    check(!verifier.verify(std::string("a\0b", 3), std::string(32, '0')), "NUL seed rejected");
    check(!verifier.verify("\xc0\xaf", std::string(32, '0')), "overlong UTF-8 rejected");
    check(!verifier.verify("\xed\xa0\x80", std::string(32, '0')), "unpaired surrogate rejected");
    check(!verifier.verify(std::string(4097, 'a'), std::string(32, '0')), "seed bound enforced");
    carlife::SessionConfig cfg;
    carlife::Session s(cfg);
    check(cfg.authMode == "sdk", "production default is real SDK validation");
    check(carlife::SessionApiTestAccess::verify(s, "seed-x", "1bd204e2f7888c7667d664146b37a111"), "default Session calls SDK verifier");
    check(!carlife::SessionApiTestAccess::verify(s, "seed-x", "wrong"), "default Session fails closed");
    cfg.authMode = "typo";
    carlife::Session invalid(cfg);
    check(!carlife::SessionApiTestAccess::verify(invalid, "seed-x", dev_digest(cfg.authSecret + "|seed-x")), "unknown auth mode does not fall through to dev");
  }
  using namespace carlife;

  {
    SessionConfig cfg;
    AdapterHarness h(cfg);
    std::vector<uint32_t> seen;
    h.adapter->setAppEventHandler([&](uint32_t type) { seen.push_back(type); });
    Session s(cfg);
    s.setHost(&h.sink());
    const std::vector<uint32_t> events{msg::GO_TO_DESKTOP, msg::SCREEN_ON, msg::SCREEN_OFF,
      msg::USER_PRESENT, msg::FOREGROUND, msg::BACKGROUND, msg::REQUEST_GO_TO_FOREGROUND,
      msg::GO_TO_FOREGROUND_RESPONSE};
    for (auto type : events) SessionApiTestAccess::frame(s, ch::CMD, type);
    check(seen == events, "all application lifecycle messages reach the host without echo");
    SessionApiTestAccess::frame(s, ch::CMD, msg::SCREEN_ON, {1});
    check(seen == events, "unexpected lifecycle payload does not reach the host");
    h.adapter->setAppEventHandler([](uint32_t) { throw std::runtime_error("private detail"); });
    SessionApiTestAccess::frame(s, ch::CMD, msg::FOREGROUND);
    check(!SessionApiTestAccess::recording(s), "application callback failure does not start recording");
    h.adapter->setAppEventHandler(nullptr);
    SessionApiTestAccess::frame(s, ch::CMD, msg::BACKGROUND);

    mvp::ControlEvent dial;
    dial.type = mvp::ControlEvent::Type::Telephony;
    dial.x = 2;
    std::memcpy(dial.dtmf.data(), "12", 3);
    h.adapter->on_control(dial);
    check(keycodes(drain(h.sink())) == std::vector<int32_t>{keycode::NUMBER_0 + 1,
      keycode::NUMBER_0 + 2, keycode::PHONE_CALL}, "dial sends the full number followed by PHONE_CALL");
    fill_touches(*h.adapter, 30);
    h.adapter->on_control(dial);
    check(drain(h.sink()).size() == 30, "dial requires room for digits AND final call key atomically");
  }

  // ═══ 1. verifyAuth：校验缝 ═══
  {
    SessionConfig cfg;
    cfg.authMode = "deny";  // 故意与 seam 的结果相反，用来证明 seam 真的被调用了
    Session s(cfg);
    SeamTrue seam;
    s.setVerifySeam(&seam);
    check(SessionApiTestAccess::verify(s, "seed-1", "value-1"),
          "verifyAuth 调用 seam，并返回它的结果（true 覆盖 deny 模式）");
    check(seam.calls == 1 && seam.seed == "seed-1" && seam.value == "value-1",
          "verifyAuth 传给 seam 的就是 (seed, encryptValue) 原样参数");
  }
  {
    SessionConfig cfg;
    cfg.authMode = "trust";
    Session s(cfg);
    SeamFalse seam;
    s.setVerifySeam(&seam);
    check(!SessionApiTestAccess::verify(s, "seed", "value"),
          "verifyAuth 返回 seam 的 false（false 覆盖 trust 模式）");
    check(seam.calls == 1, "seam 只被调用一次");
  }
  {
    SessionConfig cfg;
    cfg.authMode = "trust";
    Session s(cfg);
    SeamThrow seam;
    s.setVerifySeam(&seam);
    check(!SessionApiTestAccess::verify(s, "seed", "value"),
          "seam 抛 std::exception → fail closed（返回 false）");
  }
  {
    SessionConfig cfg;
    cfg.authMode = "trust";
    Session s(cfg);
    SeamThrowUnknown seam;
    s.setVerifySeam(&seam);
    check(!SessionApiTestAccess::verify(s, "seed", "value"),
          "seam 抛非 std 异常 → fail closed（返回 false）");
  }
  // 不设 seam（nullptr）：原有 trust/dev/deny 三种模式必须保持不变。
  {
    SessionConfig cfg;
    Session s(cfg);
    s.setVerifySeam(nullptr);
    cfg.authMode = "trust";
    Session st(cfg);
    check(SessionApiTestAccess::verify(st, "seed", "value"), "seam=nullptr + trust → 仍然放行");
    cfg.authMode = "deny";
    Session sd(cfg);
    check(!SessionApiTestAccess::verify(sd, "seed", "value"), "seam=nullptr + deny → 仍然拒绝");
    cfg.authMode = "dev";
    Session sv(cfg);
    const std::string good = dev_digest(cfg.authSecret + "|" + "seed-x");
    check(SessionApiTestAccess::verify(sv, "seed-x", good), "seam=nullptr + dev → 正确摘要通过");
    check(!SessionApiTestAccess::verify(sv, "seed-x", "wrong"), "seam=nullptr + dev → 错误摘要拒绝");
  }

  // ═══ 2. 能力协商：CONTENT_ENCRYPTION 与 MULTI_TOUCH ═══
  {
    SessionConfig cfg;
    cfg.contentEncryption = true;
    cfg.btName = "bt-x";
    cfg.btMac = "AA:BB:CC:DD:EE:FF";
    Session s(cfg);
    bool found = false;
    const pb::FeatureConfigList no_key = SessionApiTestAccess::build_features(s);
    check(feature_value(no_key, feature::kContentEncryption, &found) == 0 && found,
          "配置开启但还没有 RSA 密钥对 → CONTENT_ENCRYPTION=0");
    check(feature_value(no_key, feature::kMultiTouch, &found) == 1 && found,
          "multiTouch 默认开 → MULTI_TOUCH=1");
    check(no_key.configs.size() == 17u, "能力表条目数不变（没有顺手多报能力位）");
    check(feature_value(no_key, feature::kConnectType, &found) == 2 &&
              feature_value(no_key, feature::kUsbMtu, &found) == 16384 &&
              feature_value(no_key, feature::kIFrameInterval, &found) == 1 &&
              feature_value(no_key, feature::kAacSupport, &found) == 0 &&
              feature_value(no_key, feature::kAudioTransmissionMode, &found) == 0 &&
              feature_value(no_key, feature::kMediaSampleRate, &found) == 44100 &&
              feature_value(no_key, feature::kVoiceMic, &found) == 0 &&
              feature_value(no_key, feature::kVoiceWakeup, &found) == 0 &&
              feature_value(no_key, feature::kInputDisable, &found) == 0 &&
              feature_value(no_key, feature::kMusicHud, &found) == 0 &&
              feature_value(no_key, feature::kEngineType, &found) == 0,
          "其余能力位取值与修复前一致（没有被放宽）");
    check(no_key.huBtName == cfg.btName && no_key.huBtMac == cfg.btMac && no_key.huBtAudioSupport,
          "BT 名称/MAC/音频支持照原样带上");

    check(SessionApiTestAccess::ensure_keypair(s), "本机可以真的生成 RSA 密钥对（非模拟结果）");
    check(SessionApiTestAccess::has_keypair(s), "密钥对已就绪");
    const pb::FeatureConfigList with_key = SessionApiTestAccess::build_features(s);
    check(feature_value(with_key, feature::kContentEncryption, &found) == 1 && found,
          "配置开启且真的持有密钥对 → CONTENT_ENCRYPTION=1");

    s.setMultiTouch(false);
    const pb::FeatureConfigList no_multi = SessionApiTestAccess::build_features(s);
    check(feature_value(no_multi, feature::kMultiTouch, &found) == 0 && found,
          "multiTouch=false → MULTI_TOUCH=0");
    check(feature_value(no_multi, feature::kContentEncryption, &found) == 1,
          "关掉多点触控不影响内容加密位");
  }
  {
    // 配置关闭（但密钥对确实存在）：仍然必须是 0，不能因为“有密钥”就报 1。
    SessionConfig cfg;
    cfg.contentEncryption = false;
    Session s(cfg);
    check(SessionApiTestAccess::ensure_keypair(s), "配置关闭时也能生成密钥对（为了测这一组合）");
    bool found = false;
    const pb::FeatureConfigList f = SessionApiTestAccess::build_features(s);
    check(feature_value(f, feature::kContentEncryption, &found) == 0 && found,
          "配置关闭 → 即使有密钥对也报 CONTENT_ENCRYPTION=0");
  }

  // ═══ 3. MODULE_STATUS → HostSink::onModuleStatus ═══
  {
    SessionConfig cfg;
    Session s(cfg);
    ModuleSink sink;
    s.setHost(&sink);
    SessionApiTestAccess::module_status(s, module_payload({{3, 7}, {9, 0}}));
    check(sink.calls == 1, "合法 MODULE_STATUS 触发 host_->onModuleStatus");
    check(sink.detail == "MODULE_STATUS module3=7 module9=0",
          "回调 detail 与日志字符串一致（逐项 moduleID=statusID）");

    // 畸形：field2 声明 5 字节但只剩 2 字节 → 解码失败，绝不能回调。
    SessionApiTestAccess::module_status(s, std::vector<uint8_t>{0x12, 0x05, 0x08, 0x01});
    check(sink.calls == 1, "畸形 MODULE_STATUS（长度越界）不触发回调");
    check(sink.detail == "MODULE_STATUS module3=7 module9=0", "畸形载荷不改动上一次的 detail");

    // 畸形：field1 varint 没有值（截断）。
    SessionApiTestAccess::module_status(s, std::vector<uint8_t>{0x08});
    check(sink.calls == 1, "畸形 MODULE_STATUS（截断 varint）不触发回调");

    // 畸形：非法 wiretype。
    SessionApiTestAccess::module_status(s, std::vector<uint8_t>{0x0F});
    check(sink.calls == 1, "畸形 MODULE_STATUS（非法 wiretype）不触发回调");
  }
  {
    SessionConfig cfg;
    Session s(cfg);  // 没有宿主机
    SessionApiTestAccess::module_status(s, module_payload({{1, 2}}));
    check(true, "没有宿主机时 MODULE_STATUS 只记日志、不崩（走到这里即通过）");
  }

  // ═══ 4a. Session 侧的上行麦克风容量防御 ═══
  {
    SessionConfig cfg;
    Session s(cfg);
    MicSink sink;
    s.setHost(&sink);
    std::vector<uint8_t> payload;

    sink.frames_to_report = 3;
    check(SessionApiTestAccess::take_mic_frame(s, &payload), "容量内的帧正常构包");
    check(payload.size() == 6 && payload[0] == 0xE8 && payload[1] == 0x03,
          "PCM16 小端样本原样进包（1000 = 0x03E8）");
    check(sink.capacity_seen == SessionApiTestAccess::mic_capacity(s),
          "会话交给宿主机的容量就是 micBuffer_ 的容量");

    sink.frames_to_report = SessionApiTestAccess::mic_capacity(s) + 1000;
    payload.assign(4, 0xEE);
    check(!SessionApiTestAccess::take_mic_frame(s, &payload),
          "宿主机回报的帧数超过缓冲容量 → 丢弃整帧");
    check(payload.empty(), "越界帧不构包（载荷为空，绝不按未校验的帧数 memcpy）");

    sink.frames_to_report = 0;
    payload.assign(4, 0xEE);
    check(!SessionApiTestAccess::take_mic_frame(s, &payload) && payload.empty(),
          "宿主机报 0 帧 → 视为无数据");

    sink.accept = false;
    payload.assign(4, 0xEE);
    check(!SessionApiTestAccess::take_mic_frame(s, &payload) && payload.empty(),
          "宿主机返回 false → 无数据");
  }
  {
    SessionConfig cfg;
    Session s(cfg);  // 没有宿主机
    std::vector<uint8_t> payload{1, 2, 3};
    check(!SessionApiTestAccess::take_mic_frame(s, &payload) && payload.empty(),
          "没有宿主机 → 无数据且载荷为空");
    check(!SessionApiTestAccess::take_mic_frame(s, nullptr), "空出参被拒绝");
  }

  // ═══ 4b. 适配器通过 HostSink& 提供上行麦克风 ═══
  {
    SessionConfig cfg;
    MicSourceHarness src;   // 先声明：适配器持有的 std::function 引用它
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();  // 必须通过基类接口调用（多态覆盖才是被验证的对象）

    int16_t buf[8];
    for (auto& v : buf) v = -1;
    std::size_t frames = 99;
    check(!sink.takeMicrophone(buf, 8, frames) && frames == 0,
          "没注入麦克风源 → false 且 frames_out=0（不自己合成 PCM）");
    check(!sink.takeMicrophone(nullptr, 8, frames) && frames == 0, "空缓冲被拒绝");
    check(!sink.takeMicrophone(buf, 0, frames) && frames == 0, "零容量被拒绝");
    check(src.calls == 0, "参数非法时源根本没被调用");

    h.adapter->setMicrophoneSource(src.fn());
    check(sink.microphoneAvailable(), "injected microphone source reports available");
    src.samples = {100, -200, 300};
    for (auto& v : buf) v = -1;
    frames = 99;
    check(sink.takeMicrophone(buf, 8, frames), "注入源后 takeMicrophone 返回 true");
    check(frames == 3, "frames_out = 源实际写入的帧数");
    check(src.calls == 1 && src.capacity_seen == 8, "源收到的容量就是调用者给的容量");
    check(buf[0] == 100 && buf[1] == -200 && buf[2] == 300 && buf[3] == -1,
          "有效样本原样保留，且没有越过 frames_out 写缓冲");

    frames = 99;
    check(!sink.takeMicrophone(nullptr, 8, frames) && frames == 0,
          "注入源后空缓冲仍被拒绝，frames_out 归零");
    check(src.calls == 1, "空缓冲不会打到源");

    src.result = false;  // 源报“这次没有数据”
    frames = 99;
    check(!sink.takeMicrophone(buf, 8, frames) && frames == 0, "源返回 false → 拒绝，frames_out=0");

    src.result = true;
    src.report_zero = true;  // 源报 0 帧
    frames = 99;
    check(!sink.takeMicrophone(buf, 8, frames) && frames == 0, "源报 0 帧 → 拒绝，frames_out=0");

    src.report_zero = false;
    src.overflow = true;  // 源报的帧数超过容量
    frames = 99;
    check(!sink.takeMicrophone(buf, 8, frames) && frames == 0,
          "源报的帧数超过容量 → 拒绝，frames_out=0");

    src.overflow = false;
    src.throw_now = true;  // 源抛异常
    frames = 99;
    check(!sink.takeMicrophone(buf, 8, frames) && frames == 0,
          "源抛异常 → 捕获后返回 false 且 frames_out=0（异常绝不穿回收包线程）");

    src.throw_now = false;
    frames = 99;
    check(sink.takeMicrophone(buf, 8, frames) && frames == 3 && buf[0] == 100,
          "恢复正常后仍然可用（异常没有破坏状态）");
  }
  {
    // 端到端：Session 通过真实适配器取到帧，样本逐字节一致。
    SessionConfig cfg;
    MicSourceHarness src;
    AdapterHarness h(cfg);
    src.samples = {100, -200, 300, 400};
    h.adapter->setMicrophoneSource(src.fn());
    Session s(cfg);
    s.setHost(&h.sink());
    std::vector<uint8_t> payload;
    check(SessionApiTestAccess::take_mic_frame(s, &payload),
          "Session 从注入的适配器取到一帧上行麦克风");
    check(payload.size() == src.samples.size() * 2, "载荷长度 = 帧数 × 2（单声道 PCM16）");
    bool bytes_ok = payload.size() == 8;
    for (std::size_t i = 0; bytes_ok && i < src.samples.size(); ++i) {
      int16_t v = 0;
      std::memcpy(&v, payload.data() + i * 2, sizeof(v));
      bytes_ok = (v == src.samples[i]);
    }
    check(bytes_ok, "样本逐字节原样进包");
    check(src.capacity_seen == SessionApiTestAccess::mic_capacity(s),
          "适配器把会话给的容量原样转给源");

    // 适配器挡不住的越界只可能来自“自己就是宿主机”的实现；这里从适配器这一侧再确认：
    src.overflow = true;
    payload.assign(4, 0xEE);
    check(!SessionApiTestAccess::take_mic_frame(s, &payload) && payload.empty(),
          "源报越界帧 → 适配器拒绝 → Session 得到“无数据”");
  }

  {
    SessionConfig cfg;
    MicSink sink;
    sink.accept = false; // No sockets required: only exercise recording request dispatch.
    Session s(cfg);
    s.setHost(&sink);
    SessionApiTestAccess::poll_mic(s);
    check(sink.calls == 0, "idle session never polls microphone");
    SessionApiTestAccess::frame(s, ch::VR, msg::VR_AUDIO_DATA, {1, 2});
    check(!SessionApiTestAccess::recording(s) && sink.calls == 0,
          "downstream VR audio does not request upstream microphone samples");
    SessionApiTestAccess::frame(s, ch::CMD, msg::MIC_RECORD_WAKEUP_START);
    check(SessionApiTestAccess::recording(s), "wakeup recording command activates microphone");
    SessionApiTestAccess::poll_mic(s);
    check(sink.calls == 1, "recording poll calls microphone source");
    SessionApiTestAccess::frame(s, ch::CMD, msg::MIC_RECORD_END);
    SessionApiTestAccess::poll_mic(s);
    check(!SessionApiTestAccess::recording(s) && sink.calls == 1,
          "record end stops microphone polling");
    SessionApiTestAccess::frame(s, ch::CMD, msg::MIC_RECORD_RECOG_START);
    check(SessionApiTestAccess::recording(s), "recognition recording command activates microphone");
  }
  {
    SessionConfig cfg;
    AdapterHarness h(cfg);
    Session s(cfg);
    s.setHost(&h.sink());
    bool found = false;
    check(!h.sink().microphoneAvailable() &&
          feature_value(SessionApiTestAccess::build_features(s), feature::kVoiceMic, &found) == 0 && found,
          "no source: do not advertise microphone capability");
    MicSourceHarness src;
    h.adapter->setMicrophoneSource(src.fn());
    check(feature_value(SessionApiTestAccess::build_features(s), feature::kVoiceMic, &found) == 1 && found,
          "injected source: advertise microphone capability");
  }
  // ═══ 5. onEncryption → LinkState::content_encryption（经 HostSink& 调用） ═══
  {
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();  // 必须走基类虚函数，断言的是多态覆盖本身
    auto mapped = [&h] { return h.core->snapshot().link.content_encryption; };

    sink.onEncryption(static_cast<uint8_t>(EncryptionState::Off), "off", 0, 0);
    check(mapped() == 0, "Encryption=Off → LinkState=0（关；改动前错报 1=要求）");
    sink.onEncryption(static_cast<uint8_t>(EncryptionState::Advertised), "advertised", 3, 1);
    check(mapped() == 1, "Encryption=Advertised → LinkState=1（要求中）");
    sink.onEncryption(static_cast<uint8_t>(EncryptionState::KeyReceived), "key", 3, 1);
    check(mapped() == 1, "Encryption=KeyReceived → LinkState=1（还没就绪）");
    sink.onEncryption(static_cast<uint8_t>(EncryptionState::Ready), "ready", 9, 2);
    check(mapped() == 2, "Encryption=Ready → LinkState=2（已启用）");
    sink.onEncryption(0x7F, "unknown", 9, 2);
    check(mapped() == 1, "未知态 → LinkState=1（绝不宣称已启用）");
    check(h.adapter->status().encryption_state == 0x7F,
          "status() 里保留的是原始状态值（没有被映射改写）");
    check(h.adapter->status().encrypted_messages == 9 &&
              h.adapter->status().decrypt_failures == 2,
          "累计加解密计数原样进状态快照");
  }

  // ═══ 6. 语音键：按下/移动 → VR_START，抬起 → VR_STOP ═══
  {
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();
    auto voice = [&h](mvp::ControlEvent::TouchPhase phase) {
      mvp::ControlEvent ev;
      ev.type = mvp::ControlEvent::Type::Voice;
      ev.phase = phase;
      h.adapter->on_control(ev);
    };
    voice(mvp::ControlEvent::TouchPhase::Down);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::VR_START},
          "Voice Down → KEYCODE_VR_START(0x21)");
    voice(mvp::ControlEvent::TouchPhase::Move);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::VR_START},
          "Voice Move（长按中）→ VR_START");
    voice(mvp::ControlEvent::TouchPhase::Up);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::VR_STOP},
          "Voice Up → KEYCODE_VR_STOP(0x22)（改动前永远只发 START）");
    check(drain(sink).empty(), "三条语音事件正好产生三条控制，不多不少");
  }

  // ═══ 7. 旋钮：方向选键 + 步数展开 + 原子拒绝 ═══
  {
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();
    using KD = mvp::ControlEvent::KnobDir;

    knob_event(*h.adapter, KD::Right, 3);
    check(keycodes(drain(sink)) ==
              std::vector<int32_t>{keycode::MOVE_RIGHT, keycode::MOVE_RIGHT, keycode::MOVE_RIGHT},
          "旋钮 Right +3 → 3 次 MOVE_RIGHT（改动前只发 1 次）");
    knob_event(*h.adapter, KD::Left, -2);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::MOVE_LEFT, keycode::MOVE_LEFT},
          "旋钮 Left -2 → 2 次 MOVE_LEFT（正负决定方向，绝对值决定次数）");
    knob_event(*h.adapter, KD::Up, 1);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::MOVE_UP}, "旋钮 Up +1 → MOVE_UP");
    knob_event(*h.adapter, KD::Down, -1);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::MOVE_DOWN},
          "旋钮 Down -1 → MOVE_DOWN");

    const uint64_t dropped_before = h.adapter->status().controls_dropped;
    knob_event(*h.adapter, KD::Up, 0);
    check(drain(sink).empty() && h.adapter->status().controls_dropped == dropped_before + 1,
          "旋钮 0 格 → 不发任何键（只记一次丢弃）");
    knob_event(*h.adapter, KD::Press, 0);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::OK}, "旋钮 Press 0 格 → 仍发一次 OK");
    knob_event(*h.adapter, KD::Press, 7);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::OK},
          "旋钮 Press 带步长 → 只发一次 OK（按下不含步长语义）");
    knob_event(*h.adapter, KD::Press, -32768);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::OK},
          "Press + INT16_MIN → 仍只发一次（不溢出）");

    const uint64_t dropped_before_min = h.adapter->status().controls_dropped;
    knob_event(*h.adapter, KD::Down, -32768);  // |INT16_MIN| = 32768 > 32
    check(drain(sink).empty() &&
              h.adapter->status().controls_dropped == dropped_before_min + 1,
          "|步数| = 32768 → 整批拒绝（不溢出、不发半个串）");
    knob_event(*h.adapter, KD::Right, 33);
    check(drain(sink).empty() && h.adapter->status().controls_dropped == dropped_before_min + 2,
          "33 格 → 整批拒绝，且只记一次丢弃（不是 33 个半截脉冲）");
    knob_event(*h.adapter, KD::Right, 32);
    check(drain(sink).size() == 32u, "32 格（上限）→ 全部下发");
  }
  {
    // 队列没空间：整批拒绝，且【既有控制一个都不能丢】。
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();
    fill_touches(*h.adapter, 31);  // 队列容量 32
    const uint64_t dropped_before = h.adapter->status().controls_dropped;
    knob_event(*h.adapter, mvp::ControlEvent::KnobDir::Right, 2);  // 31 + 2 > 32
    check(h.adapter->status().controls_dropped == dropped_before + 2,
          "旋钮脉冲串放不下 → 整批计入丢弃（2 个）");
    const auto kept = drain(sink);
    check(kept.size() == 31u && kept.front().type == HostControl::Type::Touch &&
              kept.front().x == 1 && kept.back().x == 31,
          "既有 31 条控制一条不丢、顺序不变（绝不清空队列腾位置）");
  }

  // ═══ 8. DTMF：逐位下发 + 整串原子拒绝 ═══
  {
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();

    dtmf_event(*h.adapter, "1");
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::NUMBER_0 + 1}, "DTMF \"1\" → 1 个数字键");
    dtmf_event(*h.adapter, "123");
    check(keycodes(drain(sink)) ==
              std::vector<int32_t>{keycode::NUMBER_0 + 1, keycode::NUMBER_0 + 2, keycode::NUMBER_0 + 3},
          "DTMF \"123\" → 三个键按序下发（改动前只发第 1 位）");
    dtmf_event(*h.adapter, "0");
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::NUMBER_0}, "DTMF \"0\" → NUMBER_0");
    dtmf_event(*h.adapter, "*#");
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::NUMBER_STAR, keycode::NUMBER_POUND},
          "DTMF \"*#\" → NUMBER_STAR/NUMBER_POUND（有真实键码，不是自造映射）");
    dtmf_event(*h.adapter, "123456789012345");  // 15 位 = 上限
    check(drain(sink).size() == 15u, "DTMF 15 位（上限）→ 全部下发");

    uint64_t dropped = h.adapter->status().controls_dropped;
    dtmf_event(*h.adapter, "12A");
    check(drain(sink).empty() && h.adapter->status().controls_dropped == dropped + 1,
          "DTMF 含非法字符（A）→ 整条拒绝，一个键都不发");
    dtmf_event(*h.adapter, "12 3");
    check(drain(sink).empty() && h.adapter->status().controls_dropped == dropped + 2,
          "DTMF 含空格 → 整条拒绝");
    dtmf_event(*h.adapter, "+1");
    check(drain(sink).empty() && h.adapter->status().controls_dropped == dropped + 3,
          "DTMF 含 '+' → 整条拒绝（表里有 NUMBER_ADD，但它不是 DTMF 字符）");

    // 没有 NUL 终止（＝残缺/越界串）：必须整条拒绝，且不得越界读。
    {
      mvp::ControlEvent ev;
      ev.type = mvp::ControlEvent::Type::Telephony;
      std::memset(ev.dtmf.data(), '1', ev.dtmf.size());  // 16 个字节全填满，无 NUL
      h.adapter->on_control(ev);
    }
    check(drain(sink).empty() && h.adapter->status().controls_dropped == dropped + 4,
          "DTMF 定长数组里没有 NUL 终止 → 整条拒绝（不做越界读）");

    // 电话硬键（没有数字串）：按名字映射，也要有界读。
    dtmf_event(*h.adapter, "", "call");
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::PHONE_CALL},
          "Telephony 无数字串 + key=\"call\" → KEYCODE_PHONE_CALL");
    dtmf_event(*h.adapter, "", "hangup");
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::PHONE_END},
          "Telephony 无数字串 + key=\"hangup\" → KEYCODE_PHONE_END");
    {
      mvp::ControlEvent ev;
      ev.type = mvp::ControlEvent::Type::Telephony;
      std::memset(ev.key.data(), 'X', ev.key.size());  // 无 NUL 的键名
      h.adapter->on_control(ev);
    }
    check(drain(sink).empty() && h.adapter->status().controls_dropped == dropped + 5,
          "Telephony 键名没有 NUL 终止 → 拒绝（不做越界读）");
    dtmf_event(*h.adapter, "", "nope");
    check(drain(sink).empty() && h.adapter->status().controls_dropped == dropped + 6,
          "Telephony 无法识别的键名 → 丢弃，不发 keycode 0");
  }
  {
    // 硬键（T::Key）也一样：无 NUL 的键名必须拒绝，而不是读到数组之外。
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();
    mvp::ControlEvent ev;
    ev.type = mvp::ControlEvent::Type::Key;
    std::memset(ev.key.data(), 'X', ev.key.size());
    h.adapter->on_control(ev);
    check(drain(sink).empty() && h.adapter->status().controls_dropped == 1,
          "硬键 key 数组无 NUL 终止 → 拒绝（不发 keycode 0）");
    mvp::ControlEvent ok;
    ok.type = mvp::ControlEvent::Type::Key;
    std::snprintf(ok.key.data(), ok.key.size(), "%s", "home");
    h.adapter->on_control(ok);
    check(keycodes(drain(sink)) == std::vector<int32_t>{keycode::HOME},
          "硬键正常名字仍然照旧映射（修复没有影响正常路径）");
  }
  {
    // DTMF 放不下：整条拒绝，且既有控制不动。
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();
    fill_touches(*h.adapter, 31);
    const uint64_t dropped = h.adapter->status().controls_dropped;
    dtmf_event(*h.adapter, "12");  // 31 + 2 > 32
    check(h.adapter->status().controls_dropped == dropped + 2,
          "DTMF 放不下 → 整条计入丢弃（2 个）");
    const auto kept = drain(sink);
    check(kept.size() == 31u && kept.front().x == 1 && kept.back().x == 31,
          "DTMF 放不下时既有 31 条控制不丢（不部分下发）");
  }

  // ═══ 9. 麦克风准备握手（0x10071 → 0x18072） ═══
  {
    SessionConfig cfg;
    PrepareSink sink;  // 先声明：保证 Session 析构时宿主机仍然活着
    Session s(cfg);
    s.setHost(&sink);
    SessionApiTestAccess::mic_prepare_start(s);
    check(sink.calls == 1, "PREPARE_START 调到宿主机 prepareMicrophone（透过多态 HostSink）");
    check(SessionApiTestAccess::prepare_done_attempts(s) == 1,
          "提示音失败 → 参考 onError 路径仍回 PREPARE_DONE");
    check(!SessionApiTestAccess::recording(s), "PREPARE_START 不启动录音（准备 ≠ 录音）");

    sink.result = true;
    SessionApiTestAccess::mic_prepare_start(s);
    check(SessionApiTestAccess::prepare_done_attempts(s) == 2,
          "宿主机同步返回 true → 回 PREPARE_DONE（0x18072）");
    check(sink.calls == 2 && !SessionApiTestAccess::recording(s),
          "准备成功后也不打开上行麦克风（本机不假装硬件已就绪）");

    sink.throw_now = true;
    SessionApiTestAccess::mic_prepare_start(s);
    check(sink.calls == 3 && SessionApiTestAccess::prepare_done_attempts(s) == 3,
          "提示音异常 → 被拦下，结束准备阶段，不启用录音");
  }
  {
    SessionConfig cfg;
    Session s(cfg);  // 没有宿主机
    SessionApiTestAccess::mic_prepare_start(s);
    check(SessionApiTestAccess::prepare_done_attempts(s) == 1,
          "没有提示音提供方 → 结束准备阶段");
  }
  {
    SessionConfig cfg;
    PrepareThrowUnknown sink;
    Session s(cfg);
    s.setHost(&sink);
    SessionApiTestAccess::mic_prepare_start(s);
    check(sink.calls == 1 && SessionApiTestAccess::prepare_done_attempts(s) == 1,
          "非 std 提示音异常 → 同样结束准备阶段");
  }
  {
    // 适配器侧的实现：默认无回调 → false；注入后返回回调结果；回调抛异常 → false。
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();
    check(!sink.prepareMicrophone(), "适配器默认没有准备回调 → false（提示音不可用）");
    bool called = false;
    h.adapter->setMicPrepareCallback([&called] {
      called = true;
      return true;
    });
    check(sink.prepareMicrophone() && called, "注入回调后原样返回回调结果");
    h.adapter->setMicPrepareCallback([]() -> bool { return false; });
    check(!sink.prepareMicrophone(), "回调返回 false → false（回调说了算）");
    h.adapter->setMicPrepareCallback([]() -> bool { throw std::runtime_error("prepare boom"); });
    check(!sink.prepareMicrophone(), "回调抛异常 → 捕获后 false（不穿回收包线程）");
  }
  {
    // 端到端：Session → 真实适配器 → 注入的准备回调 → DONE 决策。
    SessionConfig cfg;
    AdapterHarness h(cfg);
    h.adapter->setMicPrepareCallback([] { return true; });
    Session s(cfg);
    s.setHost(&h.sink());
    SessionApiTestAccess::mic_prepare_start(s);
    check(SessionApiTestAccess::prepare_done_attempts(s) == 1,
          "端到端：宿主机准备完成 → Session 回 PREPARE_DONE");
    check(!SessionApiTestAccess::recording(s), "端到端：准备完成不会打开上行麦克风");
    h.adapter->setMicPrepareCallback(nullptr);
    SessionApiTestAccess::mic_prepare_start(s);
    check(SessionApiTestAccess::prepare_done_attempts(s) == 2,
          "端到端：没有提示音回调 → 结束准备阶段");
  }

  // ═══ 10. 命令型回传：转前台（0x18025）/ 模块控制（0x18028） ═══
  {
    // 命令 ID 逐字钉死：两套参考实现取值一致（C++ 车机库
    // CTranRecvPackageProcess.h:116,119 与 Android SDK ServiceTypes.kt:128,131）。
    check(msg::GO_TO_FOREGROUND == 0x00018025u, "GO_TO_FOREGROUND = 0x00018025");
    check(msg::MODULE_CONTROL == 0x00018028u, "MODULE_CONTROL = 0x00018028");

    // 纯编码器：singular CarlifeModuleStatus{moduleID=1, statusID=2} 的线上字节就是
    // [08 01 10 02]（field1 varint=1、field2 varint=2），一条命令 = 一个包。
    HostControl c;
    c.type = HostControl::Type::ModuleControl;
    c.module_id = 1;
    c.status_id = 2;
    ControlCommand command;
    check(encodeControlCommand(c, &command), "ModuleControl 是命令型控制");
    check(command.service_type == msg::MODULE_CONTROL, "模块控制走 CMD 通道的 0x18028");
    const std::vector<uint8_t> expected{0x08, 0x01, 0x10, 0x02};
    check(command.payload == expected,
          "模块控制载荷 = [08,01,10,02]（singular，非 ModuleStatusList）");
    // 反证“不是 List 格式”：List 会把每条状态包在 field2 的 length-delimited 里，
    // 而这里 field2 是 varint —— 同一个 List 解码器读这条载荷读不出任何条目。
    pb::ModuleStatusList as_list;
    check(pb::ModuleStatusList::decode(command.payload.data(), command.payload.size(), &as_list) &&
              as_list.items.empty(),
          "该载荷不是 CarlifeModuleStatusList（List 解码器读不出条目）");

    // 转前台：0x18025 + 空载荷（参考实现 cmdGoToForeground 只写包头、dataSize=0）。
    HostControl fg;
    fg.type = HostControl::Type::Foreground;
    check(encodeControlCommand(fg, &command) && command.service_type == msg::GO_TO_FOREGROUND &&
              command.payload.empty(),
          "转前台 = 0x18025 + 空载荷");

    // 现有控制类型不受影响：它们不是命令型，继续走原来的触控/硬键分支。
    HostControl touch;
    touch.type = HostControl::Type::Touch;
    HostControl key;
    key.type = HostControl::Type::Key;
    key.keycode = keycode::HOME;
    check(!encodeControlCommand(touch, &command) && !encodeControlCommand(key, &command),
          "触控/硬键不被命令型分支截走（原路径不变）");
    check(!encodeControlCommand(touch, nullptr), "空出参不被写入");

    // 编码器本身不做范围校验（边界在适配器）：负数按 proto int32 语义编成符号扩展的
    // 10 字节 varint，不截断成 uint、也不当成“空字段”静默丢弃。
    HostControl neg;
    neg.type = HostControl::Type::ModuleControl;
    neg.module_id = -1;
    neg.status_id = 0;
    check(!encodeControlCommand(neg, &command),
          "独立 HostSink 也不能绕过模块参数验证");
  }
  {
    // 未连接：队列没人消费，两条命令都拒绝。
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();
    check(!h.adapter->requestForeground(), "未连接：转前台被拒绝");
    check(!h.adapter->requestModuleControl(1, 2), "未连接：模块控制被拒绝");
    check(drain(sink).empty(), "未连接时不入队");
    check(h.adapter->status().controls_dropped == 2, "未连接的两次拒绝都计数");

    // 连上（Established 是会话可以收发包的稳定态，也是适配器 connected_ 的唯一置位点）。
    sink.onState(State::Established, "test");
    const uint64_t dropped = h.adapter->status().controls_dropped;
    const uint64_t sent = h.adapter->status().controls_sent;

    // 边界：module_id 必须在 1..7，status_id 必须非负。
    check(!h.adapter->requestModuleControl(0, 0), "module_id=0 被拒绝");
    check(!h.adapter->requestModuleControl(8, 0), "module_id=8（超上限）被拒绝");
    check(!h.adapter->requestModuleControl(-1, 0), "module_id=-1 被拒绝");
    check(!h.adapter->requestModuleControl(1, -1), "status_id=-1 被拒绝");
    check(!h.adapter->requestModuleControl(7, -2147483647 - 1), "status_id=INT32_MIN 被拒绝");
    check(drain(sink).empty(), "非法参数不入队");
    check(h.adapter->status().controls_dropped == dropped + 5, "5 次非法参数都计数");
    check(h.adapter->status().controls_sent == sent, "拒绝不会增加“已入队”计数");

    // 合法值：1..7 与 status=0 都放行（非负即可，不限定枚举）。
    check(h.adapter->requestModuleControl(1, 2), "已连接：module=1 status=2 入队成功");
    check(h.adapter->requestForeground(), "已连接：转前台入队成功");
    check(h.adapter->requestModuleControl(4, 0), "status=0 合法（非负即可）");
    check(h.adapter->requestModuleControl(7, 2147483647), "module=7 与 int32 上限状态合法");
    check(h.adapter->status().controls_sent == sent + 4,
          "成功入队 4 条（true 只表示已入队，计入 controls_sent）");
    const auto cmds = drain(sink);
    check(cmds.size() == 4u, "4 条命令按 FIFO 取出");
    check(cmds[0].type == HostControl::Type::ModuleControl && cmds[0].module_id == 1 &&
              cmds[0].status_id == 2,
          "第 1 条 = ModuleControl module 1 / status 2");
    check(cmds[1].type == HostControl::Type::Foreground, "第 2 条 = Foreground");
    check(cmds[2].module_id == 4 && cmds[2].status_id == 0, "status=0 原样保存");
    check(cmds[3].module_id == 7 && cmds[3].status_id == 2147483647, "边界值原样保存");
    // 整条路径：适配器入队 → 会话取出 → 编码，产出的就是线上 (serviceType, 载荷)。
    ControlCommand wire;
    check(encodeControlCommand(cmds[0], &wire) && wire.service_type == msg::MODULE_CONTROL &&
              wire.payload == std::vector<uint8_t>({0x08, 0x01, 0x10, 0x02}),
          "队列里的模块控制编码成线上 [08,01,10,02]");
    check(encodeControlCommand(cmds[1], &wire) && wire.service_type == msg::GO_TO_FOREGROUND &&
              wire.payload.empty(),
          "队列里的转前台编码成 0x18025 空载荷");

    // 断开后同样拒绝（connected_ 在 Failed/Closed 上回落）。
    sink.onState(State::Closed, "bye");
    check(!h.adapter->requestForeground() && !h.adapter->requestModuleControl(1, 0),
          "断开后拒绝（没有会话在跑）");
  }
  {
    // 队列满：整条拒绝，且【既有控制一条都不丢】（绝不为了塞命令腾位置）。
    SessionConfig cfg;
    AdapterHarness h(cfg);
    HostSink& sink = h.sink();
    sink.onState(State::Established, "test");
    fill_touches(*h.adapter, 32);  // 正好填满有界控制队列
    const uint64_t dropped = h.adapter->status().controls_dropped;
    check(!h.adapter->requestForeground(), "队列满：转前台被拒绝");
    check(!h.adapter->requestModuleControl(1, 2), "队列满：模块控制被拒绝");
    check(h.adapter->status().controls_dropped == dropped + 2, "两条都计入丢弃");
    const auto kept = drain(sink);
    check(kept.size() == 32u && kept.front().type == HostControl::Type::Touch &&
              kept.front().x == 1 && kept.back().x == 32,
          "队列满时既有 32 条控制不丢（命令不挤掉在路上的控制）");
  }

  std::printf("\ninput-api: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
