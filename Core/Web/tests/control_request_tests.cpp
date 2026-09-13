// /api/control 表单解析的独立回归测试（standalone main，自定义 CHECK，不用 assert：
// 交付配置可能是 Release/NDEBUG）。CMake 注册由 Codex 完成，本文件不建构建系统。
#include "wirelesscarplay/control_request.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

using namespace mvp;

namespace {
int checks = 0;
std::string_view g_body;
}  // namespace

#define CHECK(x)                                                                        \
  do {                                                                                  \
    ++checks;                                                                           \
    if (!(x)) {                                                                         \
      std::cerr << "check failed at line " << __LINE__ << " for body [" << g_body       \
                << "]: " #x "\n";                                                       \
      return 1;                                                                         \
    }                                                                                   \
  } while (0)

namespace {

// 解析成功：error 必须被清空。
bool accepted(std::string_view body, ControlEvent& out) {
  g_body = body;
  std::string error("sentinel");
  const bool ok = parse_control_request(body, out, error);
  if (!ok || !error.empty()) {
    std::cerr << "expected accept for [" << body << "], ok=" << ok << " error=[" << error
              << "]\n";
    return false;
  }
  return true;
}

// 解析失败：必须有错误信息，且 out 一个字节都不能被改写。
bool rejected(std::string_view body) {
  g_body = body;
  ControlEvent probe;
  std::memset(&probe, 0xA5, sizeof(probe));
  std::array<unsigned char, sizeof(ControlEvent)> snapshot{};
  std::memcpy(snapshot.data(), &probe, sizeof(probe));
  std::string error;
  const bool ok = parse_control_request(body, probe, error);
  if (ok) {
    std::cerr << "expected reject for [" << body << "]\n";
    return false;
  }
  if (error.empty()) {
    std::cerr << "rejected without error message for [" << body << "]\n";
    return false;
  }
  if (std::memcmp(snapshot.data(), &probe, sizeof(probe)) != 0) {
    std::cerr << "rejected but modified out for [" << body << "]\n";
    return false;
  }
  return true;
}

// 解析失败且错误原因包含指定子串（用来证明是哪一条规则先触发）。
bool rejected_with(std::string_view body, std::string_view expected) {
  g_body = body;
  ControlEvent probe;
  std::memset(&probe, 0xA5, sizeof(probe));
  std::string error;
  if (parse_control_request(body, probe, error)) {
    std::cerr << "expected reject for [" << body << "]\n";
    return false;
  }
  if (error.find(expected) == std::string::npos) {
    std::cerr << "expected error [" << expected << "] for [" << body << "], got [" << error
              << "]\n";
    return false;
  }
  return true;
}

// 定长文本字段：内容一致且紧跟一个 NUL（下游 CarLife 有界读要求完整 NUL 终止）。
template <std::size_t N>
int text_is(const std::array<char, N>& text, std::string_view expected) {
  return expected.size() + 1 <= N &&
         std::memcmp(text.data(), expected.data(), expected.size()) == 0 &&
         text[expected.size()] == 0;
}

// ── 1) touch ────────────────────────────────────────────────────────────────
int test_touch() {
  ControlEvent e;
  CHECK(accepted("type=touch&x=0&y=32767&phase=down", e));
  CHECK(e.type == ControlEvent::Type::Touch);
  CHECK(e.x == 0 && e.y == 32767);
  CHECK(e.phase == ControlEvent::TouchPhase::Down);
  // 未显式赋值的字段必须是零状态
  CHECK(e.knob_steps == 0 && e.knob_dir == ControlEvent::KnobDir::Left);
  CHECK(e.point_count == 0 && e.key[0] == 0 && e.gesture[0] == 0 && e.ctrl[0] == 0 &&
        e.dtmf[0] == 0);
  // 字段顺序无关
  CHECK(accepted("phase=move&y=2&x=1&type=touch", e));
  CHECK(e.x == 1 && e.y == 2 && e.phase == ControlEvent::TouchPhase::Move);
  CHECK(accepted("type=touch&x=1&y=2&phase=up", e));
  CHECK(e.phase == ControlEvent::TouchPhase::Up);
  // 三个字段缺一不可
  CHECK(rejected("type=touch&x=1&y=2"));
  CHECK(rejected("type=touch&y=2&phase=down"));
  CHECK(rejected("type=touch&x=1&phase=down"));
  CHECK(rejected("type=touch"));
  // 数值边界与尾随数据
  CHECK(rejected("type=touch&x=32768&y=0&phase=down"));
  CHECK(rejected("type=touch&x=0&y=32768&phase=down"));
  CHECK(rejected("type=touch&x=-1&y=0&phase=down"));
  CHECK(rejected("type=touch&x=0&y=99999999999999999999&phase=down"));
  CHECK(rejected("type=touch&x=12abc&y=0&phase=down"));
  CHECK(rejected("type=touch&x=+1&y=0&phase=down"));
  CHECK(rejected("type=touch&x=1%20&y=0&phase=down"));
  CHECK(rejected("type=touch&x=1&y=0&phase=drag"));
  // 未知字段打错字
  CHECK(rejected("type=touch&x=1&y=0&phase=down&z=1"));
  CHECK(rejected_with("type=touch&x=1&y=0&phase=down&z=1", "unknown field"));
  CHECK(rejected("type=touch&x=1&x=2&y=0&phase=down"));
  CHECK(rejected("type=touch&type=key&x=1&y=0&phase=down"));
  return 0;
}

// ── 2) key ──────────────────────────────────────────────────────────────────
int test_key() {
  ControlEvent e;
  CHECK(accepted("type=key&key=play", e));
  CHECK(e.type == ControlEvent::Type::Key);
  CHECK(text_is(e.key, "play"));
  CHECK(e.key[23] == 0);
  CHECK(e.x == 0 && e.y == 0 && e.point_count == 0);
  CHECK(e.phase == ControlEvent::TouchPhase::Up && e.knob_dir == ControlEvent::KnobDir::Left);
  CHECK(accepted("type=key&key=media.next", e));
  CHECK(text_is(e.key, "media.next"));
  // 沿用现有 /api/control 的 key=<名字|十进制标识> 约定：纯数字也照收
  CHECK(accepted("type=key&key=65535", e));
  CHECK(text_is(e.key, "65535"));
  // 23 字节上限（定长数组 24 = 23 + NUL）
  CHECK(accepted("type=key&key=" + std::string(23, 'k'), e));
  CHECK(e.key[23] == 0);
  CHECK(rejected("type=key&key="));
  CHECK(rejected("type=key&key=" + std::string(24, 'k')));
  CHECK(rejected("type=key&key=a%00b"));
  CHECK(rejected("type=key"));
  CHECK(rejected("type=key&key=play&x=1"));
  CHECK(rejected("type=key&key=play&key=next"));
  return 0;
}

// ── 3) multitouch ───────────────────────────────────────────────────────────
int test_multitouch() {
  ControlEvent e;
  CHECK(accepted("type=multitouch&phase=down&points=0,10,20,down;9,30,40,move", e));
  CHECK(e.type == ControlEvent::Type::MultiTouch);
  CHECK(e.phase == ControlEvent::TouchPhase::Down);
  CHECK(e.point_count == 2);
  CHECK(e.points[0].id == 0 && e.points[0].x == 10 && e.points[0].y == 20);
  CHECK(e.points[0].phase == static_cast<uint8_t>(ControlEvent::TouchPhase::Down));
  CHECK(e.points[1].id == 9 && e.points[1].x == 30 && e.points[1].y == 40);
  CHECK(e.points[1].phase == static_cast<uint8_t>(ControlEvent::TouchPhase::Move));
  // 未用到的触点保持零状态
  CHECK(e.points[2].id == 0 && e.points[2].x == 0 && e.points[2].y == 0 &&
        e.points[2].phase == 0);
  CHECK(e.x == 0 && e.y == 0 && e.key[0] == 0);

  std::string ten = "type=multitouch&phase=move&points=";
  for (int i = 0; i < 10; ++i) {
    if (i) ten += ';';
    ten += std::to_string(i) + ",1,2,up";
  }
  CHECK(accepted(ten, e));
  CHECK(e.point_count == 10);
  CHECK(e.phase == ControlEvent::TouchPhase::Move);

  std::string eleven = "type=multitouch&phase=move&points=";
  for (int i = 0; i < 11; ++i) {
    if (i) eleven += ';';
    eleven += std::to_string(i) + ",1,2,up";
  }
  CHECK(rejected_with(eleven, "at most 10 points"));

  CHECK(rejected("type=multitouch&phase=down&points="));
  CHECK(rejected("type=multitouch&phase=down&points=0,1,2"));
  CHECK(rejected("type=multitouch&phase=down&points=0,1,2,up,3"));
  CHECK(rejected("type=multitouch&phase=down&points=0,1,2,up;"));
  CHECK(rejected("type=multitouch&phase=down&points=;0,1,2,up"));
  CHECK(rejected("type=multitouch&phase=down&points=0,1,2,up;0,3,4,down"));
  CHECK(rejected("type=multitouch&phase=down&points=10,1,2,up"));
  CHECK(rejected("type=multitouch&phase=down&points=0,32768,2,up"));
  CHECK(rejected("type=multitouch&phase=down&points=0,1,-1,up"));
  CHECK(rejected("type=multitouch&phase=down&points=0,1,2,press"));
  CHECK(rejected("type=multitouch&phase=hold&points=0,1,2,up"));
  CHECK(rejected("type=multitouch&points=0,1,2,up"));
  CHECK(rejected("type=multitouch&phase=down"));
  CHECK(rejected("type=multitouch&phase=down&points=0,1,2,up&x=1"));
  return 0;
}

// ── 4) knob ─────────────────────────────────────────────────────────────────
int test_knob() {
  ControlEvent e;
  CHECK(accepted("type=knob&direction=left&steps=-127", e));
  CHECK(e.type == ControlEvent::Type::Knob);
  CHECK(e.knob_dir == ControlEvent::KnobDir::Left && e.knob_steps == -127);
  CHECK(accepted("type=knob&direction=right&steps=127", e));
  CHECK(e.knob_dir == ControlEvent::KnobDir::Right && e.knob_steps == 127);
  CHECK(accepted("type=knob&direction=up&steps=0", e));
  CHECK(e.knob_dir == ControlEvent::KnobDir::Up && e.knob_steps == 0);
  CHECK(accepted("type=knob&direction=down&steps=-3", e));
  CHECK(e.knob_dir == ControlEvent::KnobDir::Down && e.knob_steps == -3);
  CHECK(accepted("type=knob&direction=press&steps=0", e));
  CHECK(e.knob_dir == ControlEvent::KnobDir::Press && e.knob_steps == 0);
  CHECK(rejected("type=knob&direction=left&steps=-128"));
  CHECK(rejected("type=knob&direction=left&steps=128"));
  CHECK(rejected("type=knob&direction=left"));
  CHECK(rejected("type=knob&steps=1"));
  CHECK(rejected("type=knob"));
  CHECK(rejected("type=knob&direction=diagonal&steps=1"));
  CHECK(rejected("type=knob&direction=left&steps=+1"));
  CHECK(rejected("type=knob&direction=left&steps=1.0"));
  CHECK(rejected("type=knob&dir=left&steps=1"));
  return 0;
}

// ── 5) gesture ──────────────────────────────────────────────────────────────
int test_gesture() {
  ControlEvent e;
  CHECK(accepted("type=gesture&gesture=swipe_left", e));
  CHECK(e.type == ControlEvent::Type::Gesture);
  CHECK(text_is(e.gesture, "swipe_left"));
  CHECK(e.gesture[31] == 0);
  CHECK(accepted("type=gesture&gesture=" + std::string(31, 'g'), e));
  CHECK(e.gesture[31] == 0);
  CHECK(rejected("type=gesture&gesture=" + std::string(32, 'g')));
  CHECK(rejected("type=gesture&gesture="));
  CHECK(rejected("type=gesture&gesture=a%00"));
  CHECK(rejected("type=gesture&name=swipe_left"));
  CHECK(rejected("type=gesture"));
  return 0;
}

// ── 6) proximity ────────────────────────────────────────────────────────────
int test_proximity() {
  ControlEvent e;
  CHECK(accepted("type=proximity&state=1", e));
  CHECK(e.type == ControlEvent::Type::Proximity && e.x == 1);
  CHECK(accepted("type=proximity&state=0", e));
  CHECK(e.x == 0);
  CHECK(rejected("type=proximity&state=2"));
  CHECK(rejected("type=proximity&state=-1"));
  CHECK(rejected("type=proximity&state="));
  CHECK(rejected("type=proximity"));
  CHECK(rejected("type=proximity&state=1&near=0"));
  return 0;
}

// ── 7) voice ────────────────────────────────────────────────────────────────
int test_voice() {
  ControlEvent e;
  CHECK(accepted("type=voice&action=down", e));
  CHECK(e.type == ControlEvent::Type::Voice);
  CHECK(e.x == 0 && e.phase == ControlEvent::TouchPhase::Down);
  CHECK(accepted("type=voice&action=up", e));
  CHECK(e.x == 1 && e.phase == ControlEvent::TouchPhase::Up);
  CHECK(accepted("type=voice&action=prewarm", e));
  CHECK(e.x == 2 && e.phase == ControlEvent::TouchPhase::Move);
  CHECK(rejected("type=voice&action="));
  CHECK(rejected("type=voice&action=start"));
  CHECK(rejected("type=voice&action=down&phase=down"));
  CHECK(rejected("type=voice"));
  return 0;
}

// ── 8) telephony ────────────────────────────────────────────────────────────
int test_telephony() {
  ControlEvent e;
  CHECK(accepted("type=telephony&action=accept", e));
  CHECK(e.type == ControlEvent::Type::Telephony);
  CHECK(e.x == 0 && text_is(e.key, "call"));
  CHECK(e.dtmf[0] == 0 && e.y == 0 && e.point_count == 0);
  CHECK(accepted("type=telephony&action=hangup", e));
  CHECK(e.x == 1 && text_is(e.key, "hangup"));
  CHECK(accepted("type=telephony&action=dial&dtmf=1234567890*#", e));
  CHECK(e.x == 2 && text_is(e.key, "call"));
  CHECK(text_is(e.dtmf, "1234567890*#"));
  CHECK(accepted("type=telephony&action=dtmf&dtmf=1#", e));
  CHECK(e.x == 3 && text_is(e.key, "dtmf") && text_is(e.dtmf, "1#"));
  CHECK(accepted("type=telephony&action=mute", e));
  CHECK(e.x == 4 && text_is(e.key, "mute"));
  CHECK(accepted("type=telephony&action=hold", e));
  CHECK(e.x == 5 && text_is(e.key, "hold"));
  // DTMF 上限 15 位（char[16] = 15 + NUL）
  CHECK(accepted("type=telephony&action=dial&dtmf=123456789012345", e));
  CHECK(text_is(e.dtmf, "123456789012345"));
  // 派生字段：显式给出且与派生值一致 → 接受；不一致 → 拒绝
  CHECK(accepted("type=telephony&action=accept&x=0&key=call", e));
  CHECK(e.x == 0 && text_is(e.key, "call"));
  CHECK(rejected("type=telephony&action=accept&x=1"));
  CHECK(rejected("type=telephony&action=accept&key=answer"));
  CHECK(accepted("type=telephony&action=dial&x=2&key=call&dtmf=1", e));
  CHECK(e.x == 2 && text_is(e.key, "call") && text_is(e.dtmf, "1"));
  CHECK(rejected("type=telephony&action=dial"));            // dial 必须有 dtmf
  CHECK(rejected("type=telephony&action=dtmf"));            // dtmf 必须有 dtmf
  CHECK(rejected("type=telephony&action=accept&dtmf=1"));   // 其他 action 不许有 dtmf
  CHECK(rejected("type=telephony&action=dial&dtmf="));
  CHECK(rejected("type=telephony&action=dial&dtmf=1234567890123456"));  // 16 位
  CHECK(rejected("type=telephony&action=dial&dtmf=12a"));
  CHECK(rejected("type=telephony&action=dial&dtmf=%2B1"));
  CHECK(rejected("type=telephony&action=answer"));
  CHECK(rejected("type=telephony"));
  return 0;
}

// ── 9) vehicle ──────────────────────────────────────────────────────────────
int test_vehicle() {
  ControlEvent e;
  CHECK(accepted("type=vehicle&control=ac_temp_set&value=26", e));
  CHECK(e.type == ControlEvent::Type::VehicleCtrl);
  CHECK(e.x == 26 && text_is(e.ctrl, "ac_temp_set"));
  CHECK(accepted("type=vehicle&control=window_open&value=-32768", e));
  CHECK(e.x == -32768 && text_is(e.ctrl, "window_open"));
  CHECK(accepted("type=vehicle&control=x&value=32767", e));
  CHECK(e.x == 32767);
  CHECK(accepted("type=vehicle&control=" + std::string(31, 'c') + "&value=0", e));
  CHECK(e.ctrl[31] == 0);
  CHECK(rejected("type=vehicle&control=" + std::string(32, 'c') + "&value=0"));
  CHECK(rejected("type=vehicle&control=ac_temp_set&value=32768"));
  CHECK(rejected("type=vehicle&control=ac_temp_set&value=-32769"));
  CHECK(rejected("type=vehicle&control=ac_temp_set&value=1.5"));
  CHECK(rejected("type=vehicle&control=&value=1"));
  CHECK(rejected("type=vehicle&control=ac_temp_set"));
  CHECK(rejected("type=vehicle&value=1"));
  CHECK(rejected("type=vehicle&ctrl=ac_temp_set&value=1"));
  CHECK(rejected("type=vehicle&control=ac_temp_set&value=1&x=1"));
  return 0;
}

// ── 百分号编码与结构严格性 ──────────────────────────────────────────────────
int test_encoding_and_structure() {
  ControlEvent e;
  CHECK(accepted("type=gesture&gesture=swipe+left", e));
  CHECK(text_is(e.gesture, "swipe left"));
  CHECK(accepted("type=key&key=a%2Bb", e));
  CHECK(text_is(e.key, "a+b"));
  CHECK(accepted("type=key&key=%70lay", e));
  CHECK(text_is(e.key, "play"));
  // %26 解码成字段值里的 '&'，绝不当成新的字段分隔符
  CHECK(accepted("type=key&key=a%26b", e));
  CHECK(e.type == ControlEvent::Type::Key && text_is(e.key, "a&b"));
  CHECK(accepted("type=vehicle&control=ac%5Ftemp%5Fset&value=1", e));
  CHECK(text_is(e.ctrl, "ac_temp_set"));
  // 字段名里的 %XX 正常解码
  CHECK(accepted("type=touch&%78=1&y=2&phase=down", e));
  CHECK(e.x == 1 && e.y == 2);
  // 非法/截断转义
  CHECK(rejected("type=key&key=100%"));
  CHECK(rejected("type=key&key=100%2"));
  CHECK(rejected("type=key&key=100%zz"));
  CHECK(rejected("type=key&key=100%2z"));
  CHECK(rejected("type=key&key=%"));
  CHECK(rejected("type%3Dtouch&x=1&y=2&phase=down"));
  // 解码出 NUL：字段值与字段名都拒绝
  CHECK(rejected("type=key&key=a%00b"));
  CHECK(rejected("type=key&%00=1&key=play"));
  // 空字段、没有 '=' 的字段、重复字段
  CHECK(rejected(""));
  CHECK(rejected("&"));
  CHECK(rejected("type=touch&"));
  CHECK(rejected("type=touch&&x=1&y=2&phase=down"));
  CHECK(rejected("type=touch&x&y=2&phase=down"));
  CHECK(rejected("type"));
  CHECK(rejected("type="));
  CHECK(rejected("type=bogus&x=1"));
  CHECK(rejected_with("type=bogus&x=1", "unknown type"));
  return 0;
}

// ── 上限：body 长度 / 字段数 / 字段名与字段值长度 ───────────────────────────
int test_limits() {
  const std::string prefix = "type=key&key=play&pad=";
  // 4096 字节正好在界内 → 换成"字段值过长"而不是"body too large"
  CHECK(rejected_with(prefix + std::string(4096 - prefix.size(), 'p'), "field value is too long"));
  // 4097 字节 → 先撞 body 长度上限（长度在解析任何字段之前就判）
  CHECK(rejected_with(prefix + std::string(4097 - prefix.size(), 'p'), "body too large"));
  CHECK(rejected_with("type=key&key=play&pad=" + std::string(20000, 'p'), "body too large"));

  // 字段名上限 32 字节
  CHECK(rejected_with("type=key&" + std::string(33, 'n') + "=1&key=play",
                      "field name is too long"));
  // 32 字节字段名在界内（会被"未知字段"拒，而不是长度上限）
  CHECK(rejected_with("type=key&" + std::string(32, 'n') + "=1&key=play", "unknown field"));

  // 字段数上限 16：16 个字段能进入类型校验，17 个字段先被字段数拒
  std::string fields16 = "type=touch&x=1&y=2&phase=down";
  for (int i = 0; i < 12; ++i) fields16 += "&p" + std::to_string(i) + "=1";
  CHECK(rejected_with(fields16, "unknown field for this type"));
  CHECK(rejected_with(fields16 + "&p12=1", "too many form fields"));
  return 0;
}

// ── 原子性：失败不动 out，成功整条替换 ──────────────────────────────────────
int test_atomicity() {
  ControlEvent e;
  std::string error;
  CHECK(parse_control_request("type=key&key=media.next", e, error));
  CHECK(error.empty() && text_is(e.key, "media.next"));
  // 成功解析必须整条替换（上一个事件的残留字段不能留下来）
  CHECK(parse_control_request("type=touch&x=7&y=8&phase=down", e, error));
  CHECK(e.type == ControlEvent::Type::Touch && e.key[0] == 0 && e.gesture[0] == 0 &&
        e.dtmf[0] == 0);
  // 失败：一个字节都不动
  std::array<unsigned char, sizeof(ControlEvent)> snapshot{};
  std::memcpy(snapshot.data(), &e, sizeof(e));
  error.clear();
  CHECK(!parse_control_request("type=touch&x=7&y=8&phase=drag", e, error));
  CHECK(!error.empty());
  CHECK(std::memcmp(snapshot.data(), &e, sizeof(e)) == 0);
  error.clear();
  CHECK(!parse_control_request("type=multitouch&phase=down&points=0,1", e, error));
  CHECK(std::memcmp(snapshot.data(), &e, sizeof(e)) == 0);
  CHECK(e.x == 7 && e.y == 8 && e.phase == ControlEvent::TouchPhase::Down);
  return 0;
}

}  // namespace

int main() {
  struct Case {
    const char* name;
    int (*run)();
  };
  const Case cases[] = {
      {"touch", test_touch},
      {"key", test_key},
      {"multitouch", test_multitouch},
      {"knob", test_knob},
      {"gesture", test_gesture},
      {"proximity", test_proximity},
      {"voice", test_voice},
      {"telephony", test_telephony},
      {"vehicle", test_vehicle},
      {"encoding_and_structure", test_encoding_and_structure},
      {"limits", test_limits},
      {"atomicity", test_atomicity},
  };
  for (const Case& item : cases) {
    const int result = item.run();
    if (result != 0) {
      std::cerr << "control_request_tests: case failed: " << item.name << "\n";
      return 1;
    }
  }
  std::cout << "control_request_tests: " << checks << " checks passed\n";
  return 0;
}
