#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// /api/control 请求体 → mvp::ControlEvent 的**严格有界**表单解析（纯语法层）。
//
// 本文件只做 application/x-www-form-urlencoded 解析与字段校验：不执行、不注入、
// 不探测平台能力、不产生任何副作用。返回 true **只表示"形式合法"**，不表示该命令
// 被平台支持、被路由接受或已被执行。例：telephony action=hold 的 key="hold" 在现有
// CarLife 键码表（Input/WirelessCarLifePlus/src/keycode_map.cpp）里**没有**对应项，
// 这里照样解析通过，但下游会按"不认识的键名"丢弃——绝不为了"接通"而编造映射。
//
// 解析规则（全是硬上限；任一条不满足即整条拒绝，且 out 保持调用前的值不变）：
//   * body ≤ 4096 字节：先判长度，再逐字段写入定长缓冲，不做与输入等长的堆分配；
//   * 字段数 ≤ 16，字段名（解码后）1..32 字节，字段值（解码后）≤ 1024 字节；
//   * '+' → 空格；'%XX' 必须是两位十六进制，非法/截断转义 → 拒绝；
//     解码后出现 NUL（如 %00）→ 拒绝；空字段、没有 '=' 的字段、重复字段名 → 拒绝；
//   * 先按 '&' 和 '=' 切分、再逐段解码，所以 %26 解码成字段值里的 '&'，
//     绝不会把它当成新的字段分隔符；
//   * 每个 type 只接受自己的字段集合，类型里多余的字段按拼写错误 → 拒绝；
//   * 整数是纯十进制（可带一个前导 '-'，无 '+'、无空白），必须整串消费完、
//     不得溢出、不得有尾随数据（"12abc" 视为非法）。
//
// 不依赖 JSON、不依赖第三方库、不假设字符串以 NUL 结尾（字段按长度视图保存）；
// 但写回 ControlEvent 的定长文本数组一律补足 0，保证下游（CarLife 适配器要求
// 完整 NUL 终止的有界读）不会读到未初始化字节。
//
// 成功时除显式赋值的字段外，其余字段都是"零状态"（type=Touch(0)、phase=Up(0)、
// knob_dir=Left(0)，其余标量/数组全 0），见 zero_event()。
//
// 事件形式（字段顺序无关，缺一不可的都会校验）：
//   type=touch&x=0..32767&y=0..32767&phase=down|move|up
//   type=key&key=<1..23 字节>
//   type=multitouch&phase=down|move|up&points=id,x,y,phase;id,x,y,phase（≤10 点，id 0..9 唯一）
//   type=knob&direction=left|right|up|down|press&steps=-127..127
//   type=gesture&gesture=<1..31 字节>
//   type=proximity&state=0|1
//   type=voice&action=down|up|prewarm
//   type=telephony&action=accept|hangup|dial|dtmf|mute|hold[&dtmf=<1..15 位 0-9*#>]
//   type=vehicle&control=<1..31 字节>&value=-32768..32767
//
// 派生字段（不接受客户端随手编值，给了就必须与派生值一致）：
//   proximity  x=state
//   voice      x=0|1|2（down|up|prewarm）且 phase=Down|Up|Move
//   telephony  x=0..5（accept|hangup|dial|dtmf|mute|hold 的序号）且 key=call|hangup|call|dtmf|mute|hold
//   vehicle    ctrl=control、x=value
// ─────────────────────────────────────────────────────────────────────────────
#include "core/session_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

namespace mvp {

namespace control_request_detail {

// ── 硬上限 ──────────────────────────────────────────────────────────────────
constexpr std::size_t kMaxBodyBytes = 4096;
constexpr std::size_t kMaxFields = 16;
constexpr std::size_t kMaxFieldNameBytes = 32;
constexpr std::size_t kMaxFieldValueBytes = 1024;

// 定长文本字段的可用字节数（不含末尾 NUL），与 Core 模型容量绑定。
constexpr std::size_t kMaxHardKeyBytes = 23;   // ControlEvent::key
constexpr std::size_t kMaxGestureNameBytes = 31;  // ControlEvent::gesture
constexpr std::size_t kMaxCtrlNameBytes = 31;  // ControlEvent::ctrl
constexpr std::size_t kMaxDtmfDigits = 15;     // ControlEvent::dtmf
static_assert(sizeof(ControlEvent::key) == kMaxHardKeyBytes + 1,
              "ControlEvent::key 容量变了：硬键名上限必须同步修改");
static_assert(sizeof(ControlEvent::gesture) == kMaxGestureNameBytes + 1,
              "ControlEvent::gesture 容量变了：手势名上限必须同步修改");
static_assert(sizeof(ControlEvent::ctrl) == kMaxCtrlNameBytes + 1,
              "ControlEvent::ctrl 容量变了：车控名上限必须同步修改");
static_assert(sizeof(ControlEvent::dtmf) == kMaxDtmfDigits + 1,
              "ControlEvent::dtmf 容量变了：DTMF 上限必须同步修改");
static_assert(sizeof(ControlEvent::points) / sizeof(ControlEvent::Point) == kMaxMultiTouch,
              "ControlEvent::points 容量必须等于 kMaxMultiTouch");

// 解码后的字段（定长、无堆）。字段名/值按长度保存，不依赖 NUL。
struct Field {
  std::array<char, kMaxFieldNameBytes> name{};
  std::array<char, kMaxFieldValueBytes> value{};
  std::size_t name_size{};
  std::size_t value_size{};
};

struct Form {
  std::array<Field, kMaxFields> fields{};
  std::size_t count{};
};

inline std::string_view field_value(const Field& field) {
  return std::string_view(field.value.data(), field.value_size);
}

inline const Field* find_field(const Form& form, std::string_view name) {
  for (std::size_t i = 0; i < form.count; ++i) {
    const Field& field = form.fields[i];
    if (field.name_size == name.size() &&
        std::memcmp(field.name.data(), name.data(), name.size()) == 0) {
      return &field;
    }
  }
  return nullptr;
}

inline bool same_name(const Field& field, std::string_view name) {
  return field.name_size == name.size() &&
         std::memcmp(field.name.data(), name.data(), name.size()) == 0;
}

// 所有字段名（含 "type"）都必须在允许集合里，否则按拼写错误拒绝。
inline bool allow_fields(const Form& form, std::initializer_list<std::string_view> allowed,
                        std::string& error) {
  for (std::size_t i = 0; i < form.count; ++i) {
    const Field& field = form.fields[i];
    bool known = false;
    for (std::string_view name : allowed) {
      if (same_name(field, name)) { known = true; break; }
    }
    if (!known) {
      error.assign("unknown field for this type: ");
      error.append(field.name.data(), field.name_size);
      return false;
    }
  }
  return true;
}

inline bool require_field(const Form& form, std::string_view name, const Field*& out,
                          std::string& error) {
  out = find_field(form, name);
  if (!out) {
    error.assign("missing field: ");
    error.append(name);
    return false;
  }
  return true;
}

inline int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// '+' → 空格；%XX 严格解码；非法/截断转义、解码出 NUL、超出容量都拒绝。
inline bool percent_decode(std::string_view raw, char* out, std::size_t capacity,
                           std::size_t& out_size, std::string& error, const char* what) {
  out_size = 0;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    unsigned char byte = static_cast<unsigned char>(c);
    if (c == '+') {
      byte = ' ';
    } else if (c == '%') {
      if (i + 2 >= raw.size()) {
        error.assign("truncated percent escape in ").append(what);
        return false;
      }
      const int high = hex_value(raw[i + 1]);
      const int low = hex_value(raw[i + 2]);
      if (high < 0 || low < 0) {
        error.assign("invalid percent escape in ").append(what);
        return false;
      }
      byte = static_cast<unsigned char>(high * 16 + low);
      i += 2;
    }
    if (byte == 0) {
      error.assign("embedded NUL in ").append(what);
      return false;
    }
    if (out_size >= capacity) {
      error.assign(what).append(" is too long");
      return false;
    }
    out[out_size++] = static_cast<char>(byte);
  }
  return true;
}

// 先按 '&' 与 '=' 切分再解码，故 %26 只会在字段值里变成 '&'。
inline bool parse_form(std::string_view body, Form& form, std::string& error) {
  if (body.size() > kMaxBodyBytes) {
    error = "body too large";
    return false;
  }
  if (body.empty()) {
    error = "empty body";
    return false;
  }
  std::size_t begin = 0;
  for (;;) {
    const std::size_t amp = body.find('&', begin);
    const std::size_t end = (amp == std::string_view::npos) ? body.size() : amp;
    const std::string_view segment = body.substr(begin, end - begin);
    if (segment.empty()) {
      error = "empty form field";
      return false;
    }
    const std::size_t eq = segment.find('=');
    if (eq == std::string_view::npos) {
      error = "form field has no '='";
      return false;
    }
    if (form.count >= kMaxFields) {
      error = "too many form fields";
      return false;
    }
    Field& field = form.fields[form.count];
    std::size_t name_size = 0;
    std::size_t value_size = 0;
    if (!percent_decode(segment.substr(0, eq), field.name.data(), kMaxFieldNameBytes, name_size,
                        error, "field name")) {
      return false;
    }
    if (name_size == 0) {
      error = "empty field name";
      return false;
    }
    if (!percent_decode(segment.substr(eq + 1), field.value.data(), kMaxFieldValueBytes,
                        value_size, error, "field value")) {
      return false;
    }
    field.name_size = name_size;
    field.value_size = value_size;
    for (std::size_t i = 0; i < form.count; ++i) {
      if (form.fields[i].name_size == name_size &&
          std::memcmp(form.fields[i].name.data(), field.name.data(), name_size) == 0) {
        error = "duplicate field";
        return false;
      }
    }
    ++form.count;
    if (amp == std::string_view::npos) return true;
    begin = amp + 1;
  }
}

// 纯十进制、整串消费、不得溢出。
inline bool parse_int(std::string_view text, bool allow_negative, long long& out) {
  if (text.empty()) return false;
  std::size_t i = 0;
  bool negative = false;
  if (text[0] == '-') {
    if (!allow_negative) return false;
    negative = true;
    i = 1;
  }
  if (i >= text.size()) return false;
  long long value = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const long long digit = c - '0';
    if (value > (std::numeric_limits<long long>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = negative ? -value : value;
  return true;
}

inline bool int_in_range(std::string_view text, bool allow_negative, long long low, long long high,
                         std::string_view name, long long& out, std::string& error) {
  if (!parse_int(text, allow_negative, out) || out < low || out > high) {
    error.assign(name);
    error.append(" must be ").append(std::to_string(low)).append("..").append(std::to_string(high));
    return false;
  }
  return true;
}

inline bool touch_phase_from(std::string_view text, ControlEvent::TouchPhase& out) {
  if (text == "down") { out = ControlEvent::TouchPhase::Down; return true; }
  if (text == "move") { out = ControlEvent::TouchPhase::Move; return true; }
  if (text == "up") { out = ControlEvent::TouchPhase::Up; return true; }
  return false;
}

inline bool knob_dir_from(std::string_view text, ControlEvent::KnobDir& out) {
  if (text == "left") { out = ControlEvent::KnobDir::Left; return true; }
  if (text == "right") { out = ControlEvent::KnobDir::Right; return true; }
  if (text == "up") { out = ControlEvent::KnobDir::Up; return true; }
  if (text == "down") { out = ControlEvent::KnobDir::Down; return true; }
  if (text == "press") { out = ControlEvent::KnobDir::Press; return true; }
  return false;
}

template <std::size_t N>
inline void copy_text(std::array<char, N>& dst, std::string_view text) {
  // dst 在 zero_event() 里已全 0，这里只覆盖前 text.size() 字节 → 末尾必然是 NUL。
  std::memcpy(dst.data(), text.data(), text.size());
}

// telephony action → (x, key)。key 取自现有 CarLife 键码表认识的键名
// （keycode_map.cpp：call→PHONE_CALL、hangup→PHONE_END、mute→MUTE；dial 走
// dtmf 数字串、dtmf 走数字串，所以 key 只作标注）。hold 在现有表里没有对应键码，
// 因此 key="hold" 明确**不是**已支持的平台操作，下游会丢弃——这里不编造映射。
struct TelephonyForm {
  std::string_view action;
  int x;
  std::string_view key;
  bool digits_required;
};

constexpr TelephonyForm kTelephonyForms[] = {
    {"accept", 0, "call", false},
    {"hangup", 1, "hangup", false},
    {"dial", 2, "call", true},
    {"dtmf", 3, "dtmf", true},
    {"mute", 4, "mute", false},
    {"hold", 5, "hold", false},
};

inline const TelephonyForm* telephony_form(std::string_view action) {
  for (const TelephonyForm& form : kTelephonyForms) {
    if (form.action == action) return &form;
  }
  return nullptr;
}

// "零状态"事件：所有标量/数组全 0，三个有默认成员的枚举显式取 0 值枚举项。
inline ControlEvent zero_event() {
  ControlEvent event{};
  event.type = ControlEvent::Type::Touch;             // 0
  event.phase = ControlEvent::TouchPhase::Up;          // 0
  event.knob_dir = ControlEvent::KnobDir::Left;        // 0
  return event;
}

}  // namespace control_request_detail

// ── 公共入口 ────────────────────────────────────────────────────────────────
// 返回 true 表示"形式合法"（不是"命令被支持/被执行"），此时 out 已被整条改写；
// 返回 false 时 out 一个字节都不动，error 里是拒绝原因。
inline bool parse_control_request(std::string_view body, ControlEvent& out, std::string& error) {
  using namespace control_request_detail;
  error.clear();

  Form form{};
  if (!parse_form(body, form, error)) return false;

  const Field* type_field = find_field(form, "type");
  if (!type_field) {
    error = "missing type";
    return false;
  }
  const std::string_view type = field_value(*type_field);

  if (type == "touch") {
    if (!allow_fields(form, {"type", "x", "y", "phase"}, error)) return false;
    const Field* fx = nullptr;
    const Field* fy = nullptr;
    const Field* fp = nullptr;
    if (!require_field(form, "x", fx, error)) return false;
    if (!require_field(form, "y", fy, error)) return false;
    if (!require_field(form, "phase", fp, error)) return false;
    long long x = 0;
    long long y = 0;
    if (!int_in_range(field_value(*fx), false, 0, 32767, "x", x, error)) return false;
    if (!int_in_range(field_value(*fy), false, 0, 32767, "y", y, error)) return false;
    ControlEvent::TouchPhase phase{};
    if (!touch_phase_from(field_value(*fp), phase)) {
      error = "phase must be down|move|up";
      return false;
    }
    ControlEvent candidate = zero_event();
    candidate.type = ControlEvent::Type::Touch;
    candidate.x = static_cast<int>(x);
    candidate.y = static_cast<int>(y);
    candidate.phase = phase;
    out = candidate;
    return true;
  }

  if (type == "key") {
    if (!allow_fields(form, {"type", "key"}, error)) return false;
    const Field* fk = nullptr;
    if (!require_field(form, "key", fk, error)) return false;
    const std::string_view name = field_value(*fk);
    if (name.empty() || name.size() > kMaxHardKeyBytes) {
      error = "key must be 1..23 bytes";
      return false;
    }
    ControlEvent candidate = zero_event();
    candidate.type = ControlEvent::Type::Key;
    copy_text(candidate.key, name);
    out = candidate;
    return true;
  }

  if (type == "multitouch") {
    if (!allow_fields(form, {"type", "phase", "points"}, error)) return false;
    const Field* fp = nullptr;
    const Field* fpts = nullptr;
    if (!require_field(form, "phase", fp, error)) return false;
    if (!require_field(form, "points", fpts, error)) return false;
    ControlEvent::TouchPhase phase{};
    if (!touch_phase_from(field_value(*fp), phase)) {
      error = "phase must be down|move|up";
      return false;
    }
    const std::string_view points = field_value(*fpts);
    if (points.empty()) {
      error = "points must not be empty";
      return false;
    }
    ControlEvent candidate = zero_event();
    candidate.type = ControlEvent::Type::MultiTouch;
    candidate.phase = phase;
    bool id_seen[kMaxMultiTouch] = {};
    std::size_t start = 0;
    for (;;) {
      const std::size_t semi = points.find(';', start);
      const std::size_t end = (semi == std::string_view::npos) ? points.size() : semi;
      const std::string_view item = points.substr(start, end - start);
      if (item.empty()) {
        error = "empty multitouch point";
        return false;
      }
      if (static_cast<std::size_t>(candidate.point_count) >= kMaxMultiTouch) {
        error = "at most 10 points";
        return false;
      }
      std::string_view token[4] = {};
      std::size_t count = 0;
      std::size_t pos = 0;
      for (;;) {
        const std::size_t comma = item.find(',', pos);
        const std::size_t token_end = (comma == std::string_view::npos) ? item.size() : comma;
        const std::string_view part = item.substr(pos, token_end - pos);
        if (part.empty() || count == 4) {
          error = "each point must be id,x,y,phase";
          return false;
        }
        token[count++] = part;
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
      }
      if (count != 4) {
        error = "each point must be id,x,y,phase";
        return false;
      }
      long long id = 0;
      long long x = 0;
      long long y = 0;
      if (!int_in_range(token[0], false, 0, 9, "point id", id, error)) return false;
      if (!int_in_range(token[1], false, 0, 32767, "point x", x, error)) return false;
      if (!int_in_range(token[2], false, 0, 32767, "point y", y, error)) return false;
      ControlEvent::TouchPhase point_phase{};
      if (!touch_phase_from(token[3], point_phase)) {
        error = "point phase must be up|down|move";
        return false;
      }
      if (id_seen[static_cast<std::size_t>(id)]) {
        error = "duplicate point id";
        return false;
      }
      id_seen[static_cast<std::size_t>(id)] = true;
      ControlEvent::Point& point = candidate.points[candidate.point_count];
      point.id = static_cast<uint8_t>(id);
      point.x = static_cast<int16_t>(x);
      point.y = static_cast<int16_t>(y);
      point.phase = static_cast<uint8_t>(point_phase);
      ++candidate.point_count;
      if (semi == std::string_view::npos) break;
      start = semi + 1;
    }
    out = candidate;
    return true;
  }

  if (type == "knob") {
    if (!allow_fields(form, {"type", "direction", "steps"}, error)) return false;
    const Field* fdir = nullptr;
    const Field* fsteps = nullptr;
    if (!require_field(form, "direction", fdir, error)) return false;
    if (!require_field(form, "steps", fsteps, error)) return false;
    ControlEvent::KnobDir direction{};
    if (!knob_dir_from(field_value(*fdir), direction)) {
      error = "direction must be left|right|up|down|press";
      return false;
    }
    long long steps = 0;
    if (!int_in_range(field_value(*fsteps), true, -127, 127, "steps", steps, error)) return false;
    ControlEvent candidate = zero_event();
    candidate.type = ControlEvent::Type::Knob;
    candidate.knob_dir = direction;
    candidate.knob_steps = static_cast<int16_t>(steps);
    out = candidate;
    return true;
  }

  if (type == "gesture") {
    if (!allow_fields(form, {"type", "gesture"}, error)) return false;
    const Field* fg = nullptr;
    if (!require_field(form, "gesture", fg, error)) return false;
    const std::string_view name = field_value(*fg);
    if (name.empty() || name.size() > kMaxGestureNameBytes) {
      error = "gesture must be 1..31 bytes";
      return false;
    }
    ControlEvent candidate = zero_event();
    candidate.type = ControlEvent::Type::Gesture;
    copy_text(candidate.gesture, name);
    out = candidate;
    return true;
  }

  if (type == "proximity") {
    if (!allow_fields(form, {"type", "state"}, error)) return false;
    const Field* fs = nullptr;
    if (!require_field(form, "state", fs, error)) return false;
    long long state = 0;
    if (!int_in_range(field_value(*fs), false, 0, 1, "state", state, error)) return false;
    ControlEvent candidate = zero_event();
    candidate.type = ControlEvent::Type::Proximity;
    candidate.x = static_cast<int>(state);  // state → x（标量参数走 x）
    out = candidate;
    return true;
  }

  if (type == "voice") {
    if (!allow_fields(form, {"type", "action"}, error)) return false;
    const Field* fa = nullptr;
    if (!require_field(form, "action", fa, error)) return false;
    const std::string_view action = field_value(*fa);
    ControlEvent candidate = zero_event();
    candidate.type = ControlEvent::Type::Voice;
    if (action == "down") {
      candidate.x = 0;
      candidate.phase = ControlEvent::TouchPhase::Down;
    } else if (action == "up") {
      candidate.x = 1;
      candidate.phase = ControlEvent::TouchPhase::Up;
    } else if (action == "prewarm") {
      candidate.x = 2;
      candidate.phase = ControlEvent::TouchPhase::Move;  // Move=按住/预热
    } else {
      error = "action must be down|up|prewarm";
      return false;
    }
    out = candidate;
    return true;
  }

  if (type == "telephony") {
    if (!allow_fields(form, {"type", "action", "dtmf", "x", "key"}, error)) return false;
    const Field* fa = nullptr;
    if (!require_field(form, "action", fa, error)) return false;
    const TelephonyForm* form_ = telephony_form(field_value(*fa));
    if (!form_) {
      error = "action must be accept|hangup|dial|dtmf|mute|hold";
      return false;
    }
    const Field* fdtmf = find_field(form, "dtmf");
    if (form_->digits_required && !fdtmf) {
      error = "dial|dtmf require dtmf digits";
      return false;
    }
    if (!form_->digits_required && fdtmf) {
      error = "dtmf digits are only allowed for dial|dtmf";
      return false;
    }
    if (fdtmf) {
      const std::string_view digits = field_value(*fdtmf);
      if (digits.empty() || digits.size() > kMaxDtmfDigits) {
        error = "dtmf must be 1..15 digits";
        return false;
      }
      for (char c : digits) {
        if (!((c >= '0' && c <= '9') || c == '*' || c == '#')) {
          error = "dtmf allows 0-9 * # only";
          return false;
        }
      }
    }
    // 派生字段：客户端若显式给出 x/key，必须与派生值一致（否则是拼写错误）。
    const Field* fx = find_field(form, "x");
    if (fx) {
      long long x = 0;
      if (!int_in_range(field_value(*fx), false, 0, 5, "x", x, error)) return false;
      if (x != form_->x) {
        error = "x does not match action";
        return false;
      }
    }
    const Field* fk = find_field(form, "key");
    if (fk && field_value(*fk) != form_->key) {
      error = "key does not match action";
      return false;
    }
    ControlEvent candidate = zero_event();
    candidate.type = ControlEvent::Type::Telephony;
    candidate.x = form_->x;
    copy_text(candidate.key, form_->key);
    if (fdtmf) copy_text(candidate.dtmf, field_value(*fdtmf));
    out = candidate;
    return true;
  }

  if (type == "vehicle") {
    if (!allow_fields(form, {"type", "control", "value"}, error)) return false;
    const Field* fc = nullptr;
    const Field* fv = nullptr;
    if (!require_field(form, "control", fc, error)) return false;
    if (!require_field(form, "value", fv, error)) return false;
    const std::string_view control = field_value(*fc);
    if (control.empty() || control.size() > kMaxCtrlNameBytes) {
      error = "control must be 1..31 bytes";
      return false;
    }
    long long value = 0;
    if (!int_in_range(field_value(*fv), true, -32768, 32767, "value", value, error)) return false;
    ControlEvent candidate = zero_event();
    candidate.type = ControlEvent::Type::VehicleCtrl;
    copy_text(candidate.ctrl, control);
    candidate.x = static_cast<int>(value);
    out = candidate;
    return true;
  }

  error.assign("unknown type: ");
  error.append(type.data(), type.size());
  return false;
}

}  // namespace mvp
