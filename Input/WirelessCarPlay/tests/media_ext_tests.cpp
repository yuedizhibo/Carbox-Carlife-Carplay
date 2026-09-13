// CarPlay 全量接口端到端测试。
//
// 结构：一个 AF_UNIX 生产者扮演引擎（catplay_c2a）侧，按 CPMF 帧格式下发**全部新类型**，
// 断言它们落到 SessionCore 的对应字段；再反向注入控制事件，断言**上行记录真的发出去了**
// （而不是像以前那样被 queue_control 直接丢弃）。
//
// 独立编译（root CMakeLists 归集成方维护，本任务不允许改）：
//   Input/WirelessCarPlay/tests/run_ext_tests.sh
#include "wirelesscarplay/catplay_media_client.hpp"
#include "wirelesscarplay/catplay_media_ext.hpp"
#include "wirelesscarplay/catplay_media_protocol.hpp"
#include "wirelesscarplay/real_media_store.hpp"
#include <array>
#include <cstring>
#include <memory>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

using namespace mvp;
using namespace mvp::catplay_media;
namespace ext = mvp::catplay_media::ext;

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; return 1; } } while (false)

static std::array<uint8_t, kHeaderBytes> raw(const Header& h) { return encode_header(h); }

static bool write_record(int fd, Header h, std::span<const uint8_t> p = {}) {
  h.payload_bytes = uint32_t(p.size());
  auto b = raw(h);
  return send(fd, b.data(), b.size(), MSG_NOSIGNAL) == ssize_t(b.size()) &&
         (p.empty() || send(fd, p.data(), p.size(), MSG_NOSIGNAL) == ssize_t(p.size()));
}

static std::span<const uint8_t> bytes(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// 下行记录构造：类型取 ext::DType，字段走 p0..p3 + 载荷。
static Header down(ext::DType t, uint32_t p0 = 0, uint32_t p1 = 0, uint32_t p2 = 0, uint32_t p3 = 0) {
  Header h{};
  h.type = static_cast<Type>(uint16_t(t));
  h.session_id = 42; h.p0 = p0; h.p1 = p1; h.p2 = p2; h.p3 = p3;
  return h;
}

static bool wait_for(const std::function<bool()>& f, int ms = 1500) {
  for (int i = 0; i < ms / 5; ++i) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  return f();
}

// 读一条完整的（64B 头 + 载荷）上行记录；前后端都会用到。
static bool read_record(int fd, Header& h, std::vector<uint8_t>& payload) {
  std::array<uint8_t, kHeaderBytes> head{};
  const ssize_t n = recv(fd, head.data(), head.size(), MSG_WAITALL);
  if (n != ssize_t(head.size())) return false;
  if (!ext::decode_header_ext(head.data(), h)) return false;
  payload.assign(h.payload_bytes, 0);
  if (!h.payload_bytes) return true;
  return recv(fd, payload.data(), payload.size(), MSG_WAITALL) == ssize_t(payload.size());
}

static std::string text_of(const std::vector<uint8_t>& v) {
  return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

int main() {
  CHECK(CatPlayMediaClient::hard_key_code("home") == 0x20002);
  CHECK(CatPlayMediaClient::hard_key_code("back") == 0x20003);
  CHECK(CatPlayMediaClient::hard_key_code("wheel_right") == 0x20009);
  CHECK(CatPlayMediaClient::hard_key_code("enter") == 0x20001);
  CHECK(CatPlayMediaClient::hard_key_code("6") == 6);
  CHECK(CatPlayMediaClient::hard_key_code("7") == 0);
  CHECK(CatPlayMediaClient::hard_key_code("65537") == 0);
  CHECK(CatPlayMediaClient::hard_key_code("4294967297") == 0);
  CHECK(CatPlayMediaClient::hard_key_code("tel.0") == ((1u << 16) | 5));
  {
    const std::array<char, 3> exact{'a','b','c'};
    std::array<char, 8> copied{};
    ext::copy_utf8(copied, std::string_view(exact.data(), exact.size()));
    CHECK(std::string(copied.data()) == "abc");
    const std::array<char, 3> utf8{char(0xe4), char(0xb8), char(0xad)};
    std::array<char, 3> truncated{};
    ext::copy_utf8(truncated, std::string_view(utf8.data(), utf8.size()));
    CHECK(truncated[0] == 0);
  }
  const std::string path = "/tmp/cpmf-ext-" + std::to_string(getpid());
  const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(listener >= 0);
  unlink(path.c_str());
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  CHECK(bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(listener, 2) == 0);

  // 【栈帧】三个对象按值放栈会让 main() 的栈帧达到 ~4.86 MiB
  // （SessionCore 406 KiB + RealMediaStore 3.25 MiB + CatPlayMediaClient 1.2 MiB），
  // 与"大对象放栈 ⇒ main() 击穿 8 MiB 栈 ⇒ 服务启动即崩"是同一类风险。
  // unique_ptr + 引用别名：既离开栈，又不需改任何调用点（core./store./client. 全部照旧）。
  // 声明顺序保证析构时 client 先于 store/core 销毁，符合依赖方向。
  auto core_owned = std::make_unique<SessionCore>();
  SessionCore& core = *core_owned;
  auto store_owned = std::make_unique<RealMediaStore>();
  RealMediaStore& store = *store_owned;
  auto client_owned = std::make_unique<CatPlayMediaClient>(core, store, path);
  CatPlayMediaClient& client = *client_owned;
  std::atomic<bool> down_done{false}, up_done{false}, alive_after_unknown{false}, nav_checked{false};

  std::thread producer([&] {
    const int c = accept(listener, nullptr, nullptr);
    if (c < 0) return;
    timeval tv{0, 400000};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 握手：ServerHello 的 p0..p3 是固定值（Core 的 valid_static 会校验）
    (void)write_record(c, Header{Type::ServerHello, 0, 0, 0, 0, 0, 0, kMaxWirePayloadBytes, 1000, 7, 1});
    (void)write_record(c, Header{Type::Heartbeat, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0});
    (void)write_record(c, Header{Type::SessionBegin, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0});

    // ── 1) 元数据：title\0artist\0album\0album_artist\0app\0genre\0
    {
      const std::string blob = std::string("Test Song") + '\0' + "Test Artist" + '\0' + "Test Album" + '\0' +
                               "Album Artist" + '\0' + "Music" + '\0' + "Rock" + '\0';
      // p0 bit0=valid bit1=playing；p1=duration；p2=position；p3=track<<16|count
      (void)write_record(c, down(ext::DType::Metadata, 0x3, 210000, 42000, (7u << 16) | 12u),
                         {reinterpret_cast<const uint8_t*>(blob.data()), blob.size()});
    }
    // ── 2) 封面：两片共 6 字节，最后一片时交付
    (void)write_record(c, down(ext::DType::ArtworkChunk, 0, 2, 6, 0x11), bytes("JPE"));
    (void)write_record(c, down(ext::DType::ArtworkChunk, 1, 2, 6, 0x11), bytes("G!!"));
    // ── 3) 歌词
    {
      const std::string lrc = "[00:01.00]line one\n";
      (void)write_record(c, down(ext::DType::LyricsChunk, 0, 1, uint32_t(lrc.size()), 0x22),
                         {reinterpret_cast<const uint8_t*>(lrc.data()), lrc.size()});
    }
    // ── 4) 媒体库：revision 然后 seek
    (void)write_record(c, down(ext::DType::MediaLibrary, 5, 0, 0x33));
    (void)write_record(c, down(ext::DType::MediaLibrary, 2, 9000));
    // ── 5) 导航逐向：定长 464 字节载荷。p0 故意用一个**枚举范围之外**的原值，
    //        用来证明 maneuver_code 是原样透传而不是被重编码（合同明确要求不编映射表）。
    {
      constexpr uint32_t kRawOutOfRange = 250;   // 大于 Maneuver 的枚举上限
      std::array<uint8_t, ext::kNavPayloadBytes> nav{};
      ext::NavigationFields f{};
      f.road = "Main St"; f.next_road = "Oak Ave";
      f.icon = "turn-right"; f.destination = "Home";
      f.distance_remaining_m = 12345; f.eta_epoch_s = 1700000000ull;
      f.lane_bitmap = 0x0005; f.destination_reached = 1;
      ext::put_navigation_payload(nav, f);
      (void)write_record(c, down(ext::DType::Navigation, kRawOutOfRange, 350, 120, 1), nav);
    }
    // ── 6) 车况：80 字节 BE 固定结构
    {
      std::array<uint8_t, ext::kVehiclePayloadBytes> v{};
      ext::put_vehicle_payload(v, 88, 2400, 61, 420, 19, 54321, 45.4215, -75.6972, 187.5f, 0x1, 3, 1, 2, 0, true, "VINTEST");
      (void)write_record(c, down(ext::DType::Vehicle), v);
    }
    // ── 7) 电话
    {
      const std::string tel = std::string("Caller Name") + '\0' + "+15551234567" + '\0';
      (void)write_record(c, down(ext::DType::Telephony, 3, 75, (4u << 8) | 88, 0x1),
                         {reinterpret_cast<const uint8_t*>(tel.data()), tel.size()});
    }
    // ── 8) 通讯录 / 通话记录
    (void)write_record(c, down(ext::DType::ContactsChunk, 0, 1, 37, 0x44), bytes("x"));
    (void)write_record(c, down(ext::DType::CallLogChunk, 0, 1, 11, 0x45), bytes("y"));
    // ── 9) 闪避：导航播报中，媒体压到 1/3
    (void)write_record(c, down(ext::DType::Ducking, 1, 333333, 1000000, 300));
    // ── 10) safe area / 副屏 / 校准 / 昼夜 / 帧率
    (void)write_record(c, down(ext::DType::SafeArea, (60u << 16) | 40u, (20u << 16) | 20u, (1920u << 16) | 1080u));
    (void)write_record(c, down(ext::DType::AuxPlane, 1, (0u << 16) | 700u, (800u << 16) | 300u, 1));
    (void)write_record(c, down(ext::DType::Calibration, (105u << 16) | 95u, 110));
    (void)write_record(c, down(ext::DType::DayNight, 1));
    (void)write_record(c, down(ext::DType::FrameRate, 60, 48));
    // ── 11) 文件传输 / OTA / 激活 / 内容加密 / 多设备
    (void)write_record(c, down(ext::DType::FileTransfer, 1, 4096, 8192), bytes("ft"));
    (void)write_record(c, down(ext::DType::Ota, 2, 55), bytes("2.0"));
    (void)write_record(c, down(ext::DType::Activation, 3));
    (void)write_record(c, down(ext::DType::ContentEncryption, 2));
    {
      const std::string ids = std::string("phone-a") + '\0' + "phone-b" + '\0';
      (void)write_record(c, down(ext::DType::MultiSession, 2, 1), {reinterpret_cast<const uint8_t*>(ids.data()), ids.size()});
    }
    // ── 12) 无障碍 / HID / 接近 / 能力
    (void)write_record(c, down(ext::DType::AssistiveTouch, 1));
    (void)write_record(c, down(ext::DType::VoiceOverState, 1));
    (void)write_record(c, down(ext::DType::HidModeState, 3));
    (void)write_record(c, down(ext::DType::ProximityState, 1));
    (void)write_record(c, down(ext::DType::Capability, 5, (1u << 8) | 1, 0x7));
    // ── 13) 音频：p0=AudioType(2=Media) → 角色应映射为 Media、声道 2。
    // AudioStart 是**旧类型**，Core 的 valid_static 要求 16 字节描述符（payload[0]=96, [1]=1, [2]<=1, [3]=0），
    // 所以这里必须给一份合法的，否则分帧校验会判 HandlerRejected 并撕掉整条连接。
    // 这一组把"活跃音频流 id 表"的不变式全钉在线上：重复快照不累加、AudioEnd 只摘匹配的那条、
    // 结束未登记的流不下溢、陈旧会话的音频记录不改镜像。最终应恰剩 1 条流（stream 8）。
    {
      std::array<uint8_t, 16> desc{};
      desc[0] = 96; desc[1] = 1; desc[2] = 0; desc[3] = 0;
      (void)write_record(c, Header{Type::AudioStart, 0, 7, 42, 0, 0, 16, 2, 48000, 2, 1}, desc);
      (void)write_record(c, Header{Type::AudioStart, 0, 7, 42, 0, 0, 16, 2, 48000, 2, 1}, desc);   // 重复快照
      (void)write_record(c, Header{Type::AudioStart, 0, 8, 42, 0, 0, 16, 2, 44100, 2, 1}, desc);
      (void)write_record(c, Header{Type::AudioEnd, 0, 7, 42, 0, 0, 0, 4, 0, 0, 0});
      (void)write_record(c, Header{Type::AudioEnd, 0, 99, 42, 0, 0, 0, 4, 0, 0, 0});              // 未登记：不得下溢
      (void)write_record(c, Header{Type::AudioStart, 0, 9, 4242, 0, 0, 16, 2, 96000, 2, 1}, desc); // 陈旧会话
    }

    // ── 13b) 陈旧/空会话的扩展记录：整条跳过，不得改 Core（也绝不拆会话）──
    // 用 DayNight p0=0 与一份标题为 "Stale" 的 Metadata：只要有一条漏进去，
    // main 里既有的 day_night==1 / title=="Test Song" 断言就会当场失败。
    {
      Header stale_day = down(ext::DType::DayNight, 0);
      stale_day.session_id = 4242;                       // 陈旧会话
      (void)write_record(c, stale_day);
      Header no_session_day = down(ext::DType::DayNight, 0);
      no_session_day.session_id = 0;                     // 无会话
      (void)write_record(c, no_session_day);
      Header stale_meta = down(ext::DType::Metadata, 0x3, 1, 2, 3);
      stale_meta.session_id = 4242;
      const std::string blob = std::string("Stale") + '\0' + "S" + '\0' + "S" + '\0' + "S" + '\0' + "S" + '\0' + "S" + '\0';
      (void)write_record(c, stale_meta, {reinterpret_cast<const uint8_t*>(blob.data()), blob.size()});
    }
    // ── 14) 未知类型：必须跳过而不撕会话
    (void)write_record(c, Header{static_cast<Type>(0x7F00), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});

    // 先等车机把下行记录消化完再注入上行：route_control 依赖 active_ 已经指向本输入，
    // 否则事件会被直接丢掉（第一版测试就是这么假失败的：下行还没处理完就注入，
    // active_ 为空 → routed=0、control_dropped++、队列里永远是空的）。
    for (int i = 0; i < 400 && std::string(core.snapshot().active.data()) != "catplay-real"; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (int i = 0; i < 400 && core.snapshot().display.target_fps != 60; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    down_done = true;
    // 等 main 先把第一条导航的断言跑完，再发第二条：否则两个不同 p0 会抢同一份状态。
    for (int i = 0; i < 800 && !nav_checked.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // ── 14b) 导航第二条：p0 落在本协议枚举范围内 → maneuver 顺序透传，code 仍原值
    {
      std::array<uint8_t, ext::kNavPayloadBytes> nav{};
      ext::NavigationFields f{};
      f.road = "Second St"; f.next_road = "Third Ave"; f.icon = "turn-left"; f.destination = "Work";
      ext::put_navigation_payload(nav, f);
      (void)write_record(c, down(ext::DType::Navigation, uint32_t(Maneuver::Right), 42, 9, 1), nav);
    }
    // ── 15) 上行：逐个注入控制，逐条读回并校验
    // 失败时把**收到的**类型/字段原样打出来：用例是顺序执行的，看到哪一条对不上
    // 就知道是哪个用例失败了（比只在断言处抛行号更有信息量）。
    auto expect_up = [&](ControlEvent e, ext::UType t, auto&& verify, const char* name) {
      core.route_control(e);
      Header h{};
      std::vector<uint8_t> p;
      if (!read_record(c, h, p)) { std::fprintf(stderr, "[ext-test] 上行 %s: 读记录失败/超时\n", name); return false; }
      if (uint16_t(h.type) != uint16_t(t)) {
        std::fprintf(stderr, "[ext-test] 上行 %s: 类型 0x%04X != 期望 0x%04X\n", name, unsigned(h.type), unsigned(uint16_t(t)));
        return false;
      }
      if (h.session_id != 42 || h.sequence == 0 || !verify(h, p)) {
        std::fprintf(stderr, "[ext-test] 上行 %s: 字段不符 p0=%u p1=%u p2=%u p3=%u payload=%zu\n",
                     name, h.p0, h.p1, h.p2, h.p3, p.size());
        return false;
      }
      return true;
    };

    { ControlEvent e; e.type = ControlEvent::Type::Key; std::memcpy(e.key.data(), "media.next", 11);
      if (!expect_up(e, ext::UType::Key, [](const Header& h, const std::vector<uint8_t>& p) {
            return h.p0 == ((0u << 16) | 4u) && h.p1 == 0 && text_of(p) == "media.next"; }, "Key/media.next")) return; }
    { ControlEvent e; e.type = ControlEvent::Type::Key; std::memcpy(e.key.data(), "tel.mute", 9);
      if (!expect_up(e, ext::UType::Key, [](const Header& h, const std::vector<uint8_t>&) {
            return h.p0 == ((1u << 16) | 4u); }, "Key/tel.mute")) return; }
    { ControlEvent e; e.type = ControlEvent::Type::MultiTouch; e.point_count = 2;
      e.points[0] = {100, 200, 0, 1}; e.points[1] = {300, 400, 1, 1};
      if (!expect_up(e, ext::UType::MultiTouch, [](const Header& h, const std::vector<uint8_t>& p) {
            return h.p0 == 2 && p.size() == 16 && be16(p.data()) == 100 && be16(p.data() + 2) == 200 &&
                   be16(p.data() + 8) == 300 && be16(p.data() + 10) == 400 && p[4] == 0 && p[12] == 1; }, "MultiTouch")) return; }
    { ControlEvent e; e.type = ControlEvent::Type::Knob; e.knob_dir = ControlEvent::KnobDir::Left; e.knob_steps = -3;
      if (!expect_up(e, ext::UType::Knob, [](const Header& h, const std::vector<uint8_t>&) {
            return h.p0 == uint32_t(ControlEvent::KnobDir::Left) && int32_t(h.p1) == -3; }, "Knob")) return; }
    { ControlEvent e; e.type = ControlEvent::Type::Gesture; std::memcpy(e.gesture.data(), "swipe_left", 11);
      if (!expect_up(e, ext::UType::Gesture, [](const Header&, const std::vector<uint8_t>& p) {
            return text_of(p) == "swipe_left"; }, "Gesture")) return; }
    { ControlEvent e; e.type = ControlEvent::Type::Proximity; e.x = 1;
      if (!expect_up(e, ext::UType::ProximityEvent, [](const Header& h, const std::vector<uint8_t>&) {
            return h.p0 == 1; }, "Proximity")) return; }
    { ControlEvent e; e.type = ControlEvent::Type::Voice; e.x = 2;
      if (!expect_up(e, ext::UType::Voice, [](const Header& h, const std::vector<uint8_t>&) {
            return h.p0 == 2; }, "Voice")) return; }
    { ControlEvent e; e.type = ControlEvent::Type::Telephony; e.x = 3; std::memcpy(e.dtmf.data(), "123#", 5);
      if (!expect_up(e, ext::UType::TelephonyCtrl, [](const Header& h, const std::vector<uint8_t>& p) {
            return h.p0 == 3 && text_of(p) == "123#"; }, "Telephony/DTMF")) return; }
    { ControlEvent e; e.type = ControlEvent::Type::VehicleCtrl; e.x = 26; std::memcpy(e.ctrl.data(), "ac_temp_set", 12);
      if (!expect_up(e, ext::UType::VehicleCtrl, [](const Header& h, const std::vector<uint8_t>& p) {
            return h.p0 == 26 && text_of(p) == "ac_temp_set"; }, "VehicleCtrl")) return; }
    // 触控仍走旧的 Touch 类型（带 session/sequence），确认既有通路没被改坏
    { ControlEvent e; e.type = ControlEvent::Type::Touch; e.phase = ControlEvent::TouchPhase::Down; e.x = 640; e.y = 360;
      if (!expect_up(e, static_cast<ext::UType>(uint16_t(Type::Touch)), [](const Header& h, const std::vector<uint8_t>&) {
            return h.session_id == 42 && h.sequence > 0 &&
                   h.p0 == uint32_t(ControlEvent::TouchPhase::Down) && h.p1 == 640 && h.p2 == 360; }, "Touch(旧类型)")) return; }
    up_done = true;
    // 未知类型之后会话仍然活着
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    alive_after_unknown = client.connected();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    close(c);
  });

  client.start();
  // 任何 CHECK 失败都会提前 return；这里保证 producer 一定被 join，
  // 否则 std::thread 析构时会 terminate（原来的测试就有这个毛病）。
  struct Joiner { std::thread& t; ~Joiner() { if (t.joinable()) t.join(); } } joiner{producer};
  CHECK(wait_for([&] { return down_done.load(); }, 3000));
  CHECK(wait_for([&] { return core.snapshot().media.valid; }));

  const auto s = core.snapshot();
  // 元数据
  CHECK(std::string(s.media.title.data()) == "Test Song");
  CHECK(std::string(s.media.artist.data()) == "Test Artist");
  CHECK(std::string(s.media.album.data()) == "Test Album");
  CHECK(std::string(s.media.album_artist.data()) == "Album Artist");
  CHECK(std::string(s.media.app.data()) == "Music");
  CHECK(std::string(s.media.genre.data()) == "Rock");
  CHECK(s.media.duration_ms == 210000 && s.media.position_ms == 9000);  // 被 MediaLibrary seek 覆盖
  CHECK(s.media.playing == 1 && s.media.track_number == 7 && s.media.track_count == 12);
  CHECK(s.media.media_library_revision == 0x33);
  // 封面
  CHECK(s.media.has_artwork && s.media.artwork_bytes == 6 && s.media.artwork_revision == 0x11);
  { const char* data = nullptr; std::size_t size = 0; uint32_t rev = 0;
    CHECK(core.artwork(&data, &size, &rev) && size == 6 && rev == 0x11 && std::string(data, size) == "JPEG!!"); }
  // 歌词
  CHECK(s.media.has_lyrics);
  { std::string_view lrc; uint32_t rev = 0;
    CHECK(core.lyrics(&lrc, &rev) && rev == 0x22 && lrc == "[00:01.00]line one\n"); }
  // 导航逐向：这一轮已经落到 SessionCore（不再是客户端私有缓存）
  CHECK(s.nav.valid && s.nav.active == 1);
  // maneuver_code 必须**原值透传**：250 越出枚举范围，所以 maneuver 留 None，但码值一字不改。
  CHECK(s.nav.maneuver_code == 250);
  CHECK(s.nav.maneuver == Maneuver::None);
  CHECK(s.nav.distance_to_maneuver_m == 350 && s.nav.time_remaining_s == 120);
  CHECK(s.nav.distance_remaining_m == 12345 && s.nav.eta_epoch_s == 1700000000ull);
  CHECK(s.nav.lane_bitmap == 0x0005 && s.nav.destination_reached == 1);
  CHECK(std::string(s.nav.road_name.data()) == "Main St" && std::string(s.nav.next_road_name.data()) == "Oak Ave");
  CHECK(std::string(s.nav.icon.data()) == "turn-right" && std::string(s.nav.destination.data()) == "Home");
  nav_checked = true;
  // 第二段：枚举范围内的原值 → maneuver 顺序透传；同时验证幂等（重设同值必须返回 false）。
  CHECK(wait_for([&] { const auto n = core.snapshot().nav;
                       return n.maneuver_code == uint32_t(Maneuver::Right) && n.maneuver == Maneuver::Right; }, 3000));
  { const auto n2 = core.snapshot().nav;
    CHECK(std::string(n2.road_name.data()) == "Second St" && std::string(n2.icon.data()) == "turn-left");
    CHECK(n2.distance_to_maneuver_m == 42 && n2.time_remaining_s == 9);
    CHECK(!core.set_navigation_state(n2)); }   // 同值必须返回 false
  // 车况
  CHECK(s.vehicle.valid && s.vehicle.speed_kph == 88 && s.vehicle.rpm == 2400 && s.vehicle.gear == 3);
  CHECK(s.vehicle.fuel_pct == 61 && s.vehicle.range_km == 420 && s.vehicle.outside_temp_c == 19);
  CHECK(s.vehicle.latitude > 45.4 && s.vehicle.latitude < 45.5 && s.vehicle.longitude < -75.6);
  CHECK(s.vehicle.doors == 0x1 && s.vehicle.lights == 2 && s.vehicle.odometer_km == 54321);
  CHECK(std::string(s.vehicle.vin.data()) == "VINTEST");
  CHECK(s.vehicle.heading_deg > 187.0f && s.vehicle.heading_deg < 188.0f);
  // 电话 / 通讯录
  CHECK(s.telephony.call_state == 3 && s.telephony.call_duration_s == 75);
  CHECK(s.telephony.signal_bars == 4 && s.telephony.battery_pct == 88 && s.telephony.dtmf_supported == 1);
  CHECK(std::string(s.telephony.caller.data()) == "Caller Name");
  CHECK(std::string(s.telephony.caller_number.data()) == "+15551234567");
  CHECK(s.telephony.contact_count == 37 && s.telephony.contacts_ready == 1);
  CHECK(s.telephony.call_log_count == 11 && s.telephony.call_log_ready == 1);
  // 音频闪避
  CHECK(s.audio.nav_active == 1 && s.audio.media_volume_ppm == 333333 && s.audio.nav_volume_ppm == 1000000);
  CHECK(s.audio.duck_ratio_ppm == 333333 && s.audio.duck_transition_ms == 300);
  // 显示
  CHECK(s.display.safe_top == 60 && s.display.safe_bottom == 40 && s.display.safe_left == 20 && s.display.safe_right == 20);
  CHECK(s.display.width == 1920 && s.display.height == 1080);
  CHECK(s.display.aux_enabled == 1 && s.display.aux_y == 700 && s.display.aux_w == 800 && s.display.aux_h == 300);
  CHECK(s.display.primary_plane == 1);
  CHECK(s.display.gamma_pct == 105 && s.display.contrast_pct == 95 && s.display.saturation_pct == 110);
  CHECK(s.display.day_night == 1);
  CHECK(s.display.target_fps == 60 && s.display.actual_fps == 48);
  // 链路
  CHECK(s.link.file_transfer_active == 1 && s.link.file_transfer_bytes == 4096 && s.link.file_transfer_total == 8192);
  CHECK(s.link.ota_state == 2 && s.link.activation_state == 3 && s.link.content_encryption == 2);
  CHECK(s.link.session_count == 2 && std::string(s.link.active_session.data()) == "phone-b");
  // 无障碍 / HID / 能力
  CHECK(s.input.assistive_touch == 1 && s.input.voiceover == 1 && s.input.hid_mode == 3);
  CHECK(s.input.proximity == 1 && s.input.multi_touch_points == 5 && s.input.touchpad == 1 && s.input.knob == 1);
  // 音频角色/编码镜像（CarPlay AudioType 2 = Media，旧描述符 → PCM16）
  CHECK(s.audio.active_role == AudioRole::Media && s.audio.channels == 2 && s.audio.sample_rate == 44100);
  CHECK(s.audio.codec == AudioCodec::Pcm16);
  // 音频流计数：重复的 AudioStart 快照不累加，AudioEnd 只摘匹配的那条，未登记的结束不下溢，
  // 陈旧会话的 AudioStart 不进镜像 → 最终恰剩 1 条流，速率/编码来自最后一条被接受的快照。
  CHECK(wait_for([&] {
    const auto a = core.snapshot().audio;
    return a.simultaneous_streams == 1 && a.sample_rate == 44100 &&
           a.codec == AudioCodec::Pcm16 && a.active_role == AudioRole::Media && a.channels == 2;
  }));
  // 未知类型被跳过而不是拆会话
  CHECK(client.record_skipped() >= 1);

  CHECK(wait_for([&] { return up_done.load(); }, 3000));
  // 硬键回传记进状态（交互面）：必须等上行真的发出去了再取新快照，否则是竞态。
  { const auto s2 = core.snapshot();
    CHECK(s2.input.last_key_code == ((1u << 16) | 4u)); }   // 最后一次是 tel.mute
  CHECK(wait_for([&] { return alive_after_unknown.load(); }, 3000));

  client.stop();
  producer.join();
  close(listener);
  unlink(path.c_str());
  std::cout << "carplay ext checks executed: " << checks << '\n';
  return 0;
}
