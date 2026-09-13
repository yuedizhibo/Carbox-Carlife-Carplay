#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CPMF 本地扩展接口 —— 本文件由 Input 侧拥有。
// 注意：这些是本项目的 IPC 编号，不是 iAP2 线协议编号；定义/解码不证明
// 引擎侧已接通。真实引擎接线缺口见 Input/API_AUDIT/README.md。
//
// 为什么单独一个文件：基础 CPMF 定义在 Core/Convert/include/wirelesscarplay/
// catplay_media_protocol.hpp（Core 拥有，本任务禁止改动）。基础定义的 Parser 用
// known_type() 白名单，遇到新类型会判 Invalid，所以这里**复用**它的帧格式常量与
// 字节序助手、静态校验函数，自己实现"新类型也认"的解码与解析器：
//   * 帧格式：64 字节头 'CPMF' + ver(1,0) + hdrlen(BE16=64) + type(BE16) + flags(BE16)
//            + stream_id(BE32) + session_id(BE64) + sequence(BE64) + pts(BE64)
//            + payload_bytes(BE32) + p0..p3(BE32×4) + 保留(BE32=0)，载荷跟随其后。
//   * 旧类型（0x0001–0x0023 / 0x8001–0x8002）语义与编号**完全不变**，校验直接回调
//     Core 的 valid_declared()/valid_static()，保证既有媒体通路一字不改。
//
// 新类型的取值区间刻意避开旧编号：
//   下行（引擎→车机）0x0030–0x004F；上行（车机→引擎）0x8010–0x801F。
//
// 每个消息的字段布局见下方注释，并在 Input/WirelessCarPlay/README.md 有协议表；
// 每条都给出 iAP2 / CarPlay 侧的来源（消息号出处见
// Temp/audit/00-iap2-authoritative-inventory.md）。
// ─────────────────────────────────────────────────────────────────────────────
#include "wirelesscarplay/catplay_media_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace mvp::catplay_media::ext {

// 旧类型沿用 Core 的 Type（本命名空间在外层 mvp::catplay_media 里，名字直接可见）。
// 只用一个整数做 transport seam：类型未知时**跳过而不拆会话**（汽车协议上不理解即丢）。

// 触点点（与 mvp::ControlEvent::Point 同构，但本层不依赖 Core 模型）。
// kMaxTouchPoints 必须与 mvp::kMaxMultiTouch 一致（都是 10）。
constexpr std::size_t kMaxTouchPoints = 10;
struct TouchPoint { int16_t x{}, y{}; uint8_t id{}, phase{}; };

// ── 新消息类型 ──────────────────────────────────────────────────────────────
// 下行（引擎/手机 → 我们）。iAP2 出处见每行注释。
enum class DType : uint16_t {
  // Now Playing 0x5000–0x5003（元数据/封面/进度）
  Metadata       = 0x0030,  // payload=title\0artist\0album\0album_artist\0app\0genre\0；p0=flags(bit0 valid,bit1 playing)，p1=duration_ms，p2=position_ms，p3=track_no<<16|track_count
  ArtworkChunk   = 0x0031,  // payload=封面字节；p0=chunk_index，p1=chunk_count，p2=total_bytes，p3=revision
  LyricsChunk    = 0x0032,  // payload=LRC 文本；p0=chunk_index，p1=chunk_count，p2=total_bytes，p3=revision
  MediaLibrary   = 0x0033,  // 媒体库 Media Library 0x4C00–0x4C09；p0=op，p1=position_ms，p2=revision，p3=items
  // 路线指引 Route Guidance 0x5200–0x5203（逐向导航）
  // payload 固定 464 字节（全 BE，见 kNavPayloadBytes / put_navigation_payload）；
  // p0=maneuver_code(**原值，绝不重编码**)，p1=distance_to_maneuver_m，p2=time_remaining_s，
  // p3=flags(bit0 active)。
  Navigation     = 0x0034,
  // 车辆状态 0xA100–0xA102 + 定位 Location 0xFFFA–0xFFFC
  Vehicle        = 0x0035,  // payload=80B BE 固定结构（见 vehicle_payload_*）
  // 电话 Telephony 0x4154–0x4161
  Telephony      = 0x0036,  // payload=caller\0number\0；p0=call_state，p1=duration_s，p2=signal<<8|battery，p3=flags(bit0 dtmf,bit1 valid)
  ContactsChunk  = 0x0037,  // payload=name\0number\0…；p0=chunk_index，p1=chunk_count，p2=total_count，p3=revision
  CallLogChunk   = 0x0038,  // 同上布局
  // 音频仲裁：导航播报时压低媒体（CarPlay 侧 AudioGain 只给绝对增益，这里是语义化的闪避）
  Ducking        = 0x0039,  // p0=nav_active，p1=media_ppm，p2=nav_ppm，p3=transition_ms
  // 显示：safe area / 副屏平面 / 校准 / 昼夜 / 帧率
  SafeArea       = 0x003A,  // p0=top<<16|bottom，p1=left<<16|right，p2=width<<16|height
  AuxPlane       = 0x003B,  // p0=enabled，p1=x<<16|y，p2=w<<16|h，p3=primary_plane
  Calibration    = 0x003C,  // p0=gamma<<16|contrast，p1=saturation
  DayNight       = 0x003D,  // p0=day_night(0=日 1=夜)
  FrameRate      = 0x003E,  // p0=target_fps，p1=actual_fps
  // 文件传输 / OTA / 激活 / 内容加密 / 多设备
  FileTransfer   = 0x003F,  // payload=file\0；p0=active，p1=bytes_done，p2=bytes_total
  Ota            = 0x0040,  // payload=version\0；p0=state，p1=progress_pct
  Activation     = 0x0041,  // p0=state(0 未知 1 未激活 2 激活中 3 已激活)
  ContentEncryption = 0x0042,  // p0=mode(0 关 1 要求 2 已启用)
  MultiSession   = 0x0043,  // payload=id\0id\0…；p0=count，p1=active_index
  // 无障碍 / HID / 接近 / 能力声明
  AssistiveTouch = 0x0044,  // p0=enabled
  VoiceOverState = 0x0045,  // p0=enabled
  HidModeState   = 0x0046,  // p0=mode
  ProximityState = 0x0047,  // p0=state(0 远 1 近)
  Capability     = 0x0048,  // p0=max_multi_touch，p1=touchpad<<8|knob，p2=flags(bit0 voice,bit1 hid,bit2 voiceover)
};

// 上行（我们 → 引擎/手机）。旧的上行只有 RequestKeyframe(0x8001) 与 Touch(0x8002)。
enum class UType : uint16_t {
  Key            = 0x8010,  // payload=键名；p0=keycode，p1=action(0 down,1 up)
  MultiTouch     = 0x8011,  // payload=point_count×8B（BE：int16 x,int16 y,uint8 id,uint8 phase,uint16 res）；p0=point_count
  Knob           = 0x8012,  // p0=dir(0 左 1 右 2 上 3 下 4 按下)，p1=steps(有符号)
  Gesture        = 0x8013,  // payload=手势名
  ProximityEvent = 0x8014,  // p0=state
  Voice          = 0x8015,  // p0=action(0 按下 1 松开 2 长按)
  TelephonyCtrl  = 0x8016,  // payload=DTMF 数字串；p0=op(0 接听 1 挂断 2 拨号 3 DTMF 4 静音 5 保持)
  VehicleCtrl    = 0x8017,  // payload=车控名；p0=value
  HidModeSet     = 0x8018,  // p0=mode
  VoiceOverSet   = 0x8019,  // p0=enabled
  AssistiveTouchSet = 0x801A,  // p0=enabled
};

inline bool is_down(DType t) { switch (t) { case DType::Metadata: case DType::ArtworkChunk: case DType::LyricsChunk: case DType::MediaLibrary: case DType::Navigation: case DType::Vehicle: case DType::Telephony: case DType::ContactsChunk: case DType::CallLogChunk: case DType::Ducking: case DType::SafeArea: case DType::AuxPlane: case DType::Calibration: case DType::DayNight: case DType::FrameRate: case DType::FileTransfer: case DType::Ota: case DType::Activation: case DType::ContentEncryption: case DType::MultiSession: case DType::AssistiveTouch: case DType::VoiceOverState: case DType::HidModeState: case DType::ProximityState: case DType::Capability: return true; default: return false; } }
inline bool is_up(UType t) { switch (t) { case UType::Key: case UType::MultiTouch: case UType::Knob: case UType::Gesture: case UType::ProximityEvent: case UType::Voice: case UType::TelephonyCtrl: case UType::VehicleCtrl: case UType::HidModeSet: case UType::VoiceOverSet: case UType::AssistiveTouchSet: return true; default: return false; } }

// 各消息载荷上限（用于 valid_declared_ext 的早期拒绝）。
constexpr std::size_t kMaxMetadataBytes = 768;   // 6 个字段各 ≤128
constexpr std::size_t kMaxArtworkTotal   = 128u * 1024u;  // 与 mvp::kMaxArtwork 对齐
constexpr std::size_t kMaxLyricsTotal    = 4096;          // 与 mvp::kMaxLyrics 对齐
constexpr std::size_t kNavPayloadBytes   = 464;   // 导航逐向固定载荷，布局见 put_navigation_payload
constexpr std::size_t kVehiclePayloadBytes = 80;
constexpr std::size_t kMaxCallerBytes    = 224;
constexpr std::size_t kMaxContactsChunk  = 1024;
constexpr std::size_t kMaxCtrlText       = 192;   // 上行文本（键名/手势名/车控名/文件名）

inline std::size_t max_payload(uint16_t t) {
  switch (static_cast<DType>(t)) {
    case DType::Metadata: return kMaxMetadataBytes;
    case DType::ArtworkChunk: return kMaxArtworkTotal;
    case DType::LyricsChunk: return kMaxLyricsTotal;
    case DType::Navigation: return kNavPayloadBytes;
    case DType::Vehicle: return kVehiclePayloadBytes;
    case DType::Telephony: return kMaxCallerBytes;
    case DType::ContactsChunk: case DType::CallLogChunk: return kMaxContactsChunk;
    case DType::FileTransfer: case DType::Ota: return kMaxCtrlText;
    case DType::MultiSession: return 256;
    default: return 0;   // 纯 p0..p3 的消息
  }
}

// ── 帧解码（新类型 + 旧类型都认；payload 上限与 Core 一致）──────────────────
inline bool decode_header_ext(const uint8_t* raw, Header& h) {
  if (raw[0] != 'C' || raw[1] != 'P' || raw[2] != 'M' || raw[3] != 'F' || raw[4] != 1 || raw[5] != 0 ||
      be16(raw + 6) != kHeaderBytes || be32(raw + 60) != 0) return false;
  h.type = static_cast<Type>(be16(raw + 8));
  h.flags = be16(raw + 10);
  h.stream_id = be32(raw + 12);
  h.session_id = be64(raw + 16);
  h.sequence = be64(raw + 24);
  h.pts = be64(raw + 32);
  h.payload_bytes = be32(raw + 40);
  h.p0 = be32(raw + 44); h.p1 = be32(raw + 48); h.p2 = be32(raw + 52); h.p3 = be32(raw + 56);
  // 帧格式合法即通过解码；类型白名单交给 valid_declared_ext。
  // 这样遇到不认识的类型时 payload_bytes 仍能让我们安全跳过整条记录。
  return h.payload_bytes <= kMaxWirePayloadBytes;
}

inline bool known_or_ext(uint16_t t) {
  return known_type(static_cast<Type>(t)) || is_down(static_cast<DType>(t)) || is_up(static_cast<UType>(t));
}

inline bool valid_declared_ext(const Header& h) {
  if (known_type(h.type)) return valid_declared(h);   // 旧类型严格沿用 Core 的校验
  const uint16_t t = static_cast<uint16_t>(h.type);
  if (is_up(static_cast<UType>(t))) return h.payload_bytes <= kMaxCtrlText;
  if (is_down(static_cast<DType>(t))) {
    // 下行：允许带 session_id（内容属于当前会话），stream_id 必须为 0，payload 受限。
    if (h.stream_id) return false;
    const std::size_t cap = max_payload(t);
    if (h.payload_bytes > cap) return false;
    // 纯标量消息不许带载荷，避免把语义藏进没人读的字节里。
    return !(cap == 0 && h.payload_bytes != 0);
  }
  // 未知类型：放行（有 payload_bytes 就能整条跳过），由 handler 计为丢弃而非撕连接。
  return true;
}

inline bool valid_static_ext(const Header& h, std::span<const uint8_t> payload) {
  if (known_type(h.type)) return valid_static(h, payload);   // 旧类型严格沿用 Core 的校验
  if (payload.size() != h.payload_bytes) return false;
  const uint16_t t16 = static_cast<uint16_t>(h.type);
  if (!known_or_ext(t16)) return true;   // 未知：跳过（不校验语义）
  switch (static_cast<DType>(t16)) {
    case DType::Metadata: return payload.size() > 0 && payload.size() <= kMaxMetadataBytes;
    case DType::ArtworkChunk: return h.p0 < h.p1 && h.p1 > 0 && h.p1 <= 256 && h.p2 > 0 && h.p2 <= kMaxArtworkTotal && payload.size() > 0;
    case DType::LyricsChunk: return h.p0 < h.p1 && h.p1 > 0 && h.p1 <= 64 && h.p2 > 0 && h.p2 <= kMaxLyricsTotal && payload.size() > 0;
    case DType::MediaLibrary: return h.p0 <= 5;
    case DType::Navigation: return payload.size() == kNavPayloadBytes;
    case DType::Vehicle: return payload.size() == kVehiclePayloadBytes;
    case DType::Telephony: return h.p0 <= 4;
    case DType::ContactsChunk: case DType::CallLogChunk: return h.p0 < h.p1 && h.p1 > 0 && h.p1 <= 64 && payload.size() > 0;
    case DType::Ducking: return h.p0 <= 1 && h.p1 <= 1000000 && h.p2 <= 1000000;
    case DType::SafeArea: case DType::AuxPlane: case DType::Calibration:
    case DType::DayNight: case DType::FrameRate: return true;
    case DType::FileTransfer: return h.p0 <= 1 && h.p1 <= h.p2;
    case DType::Ota: return h.p0 <= 4 && h.p1 <= 100;
    case DType::Activation: return h.p0 <= 3;
    case DType::ContentEncryption: return h.p0 <= 2;
    case DType::MultiSession: return h.p0 <= 8 && h.p1 <= h.p0;
    case DType::AssistiveTouch: case DType::VoiceOverState: return h.p0 <= 1;
    case DType::HidModeState: return h.p0 <= 4;
    case DType::ProximityState: return h.p0 <= 1;
    case DType::Capability: return h.p0 <= 10;
    default: break;
  }
  switch (static_cast<UType>(t16)) {
    case UType::Key: return h.p1 <= 1;
    case UType::MultiTouch: return h.p0 > 0 && h.p0 <= kMaxTouchPoints && payload.size() == std::size_t(h.p0) * 8;
    case UType::Knob: return h.p0 <= 4;
    case UType::Gesture: case UType::VehicleCtrl: return payload.size() > 0;
    case UType::ProximityEvent: return h.p0 <= 1;
    case UType::Voice: return h.p0 <= 2;
    case UType::TelephonyCtrl: return h.p0 <= 5;
    case UType::HidModeSet: return h.p0 <= 4;
    case UType::VoiceOverSet: case UType::AssistiveTouchSet: return h.p0 <= 1;
    default: return false;
  }
}

// ── 解析器：与 Core::Parser 同构，只是解码/校验换成 *_ext ────────────────────
class Parser {
 public:
  void reset() { header_used_ = payload_used_ = 0; have_header_ = false; }
  ParseResult push(const uint8_t* data, std::size_t size, RecordHandler handler, void* context) {
    while (size) {
      if (!have_header_) {
        const std::size_t take = std::min(size, kHeaderBytes - header_used_);
        std::memcpy(header_.data() + header_used_, data, take);
        header_used_ += take; data += take; size -= take;
        if (header_used_ != kHeaderBytes) continue;
        if (!decode_header_ext(header_.data(), current_) || !valid_declared_ext(current_))
          return ParseResult::Invalid;
        have_header_ = true; payload_used_ = 0;
        if (!current_.payload_bytes) {
          if (!valid_static_ext(current_, {}) || !handler(context, current_, {})) return ParseResult::HandlerRejected;
          reset();
        }
        continue;
      }
      const std::size_t take = std::min(size, std::size_t(current_.payload_bytes) - payload_used_);
      std::memcpy(payload_.data() + payload_used_, data, take);
      payload_used_ += take; data += take; size -= take;
      if (payload_used_ != current_.payload_bytes) continue;
      if (!valid_static_ext(current_, {payload_.data(), payload_used_}) ||
          !handler(context, current_, {payload_.data(), payload_used_}))
        return ParseResult::HandlerRejected;
      reset();
    }
    return ParseResult::NeedMore;
  }
 private:
  std::array<uint8_t, kHeaderBytes> header_{};
  std::array<uint8_t, kMaxWirePayloadBytes> payload_{};
  Header current_{};
  std::size_t header_used_{}, payload_used_{};
  bool have_header_{};
};

// ── 载荷字段助手（全部 Big-Endian，与帧头一致，避免歧义）───────────────────
inline void put_f32(uint8_t* p, float v) { uint32_t bits; std::memcpy(&bits, &v, 4); put_be32(p, bits); }
inline float get_f32(const uint8_t* p) { uint32_t bits = be32(p); float v; std::memcpy(&v, &bits, 4); return v; }
inline void put_f64(uint8_t* p, double v) { uint64_t bits; std::memcpy(&bits, &v, 8); put_be64(p, bits); }
inline double get_f64(const uint8_t* p) { uint64_t bits = be64(p); double v; std::memcpy(&v, &bits, 8); return v; }

// 车况载荷 80 字节布局（BE）：
//  0 int32 speed_kph | 4 int32 rpm | 8 int32 fuel_pct | 12 int32 range_km
//  16 int32 outside_temp_c | 20 uint32 odometer_km | 24 double latitude | 32 double longitude
//  40 float heading_deg | 44 uint16 doors | 46 uint8 gear | 47 uint8 night_mode
//  48 uint8 lights | 49 uint8 parking_brake | 50 uint8 flags(bit0 valid) | 51 保留
//  52 char vin[24] | 76 uint32 保留
constexpr std::size_t kVehicleVinOffset = 52, kVehicleVinBytes = 24;
inline void put_vehicle_payload(std::array<uint8_t, kVehiclePayloadBytes>& b, int32_t speed_kph, int32_t rpm,
                                int32_t fuel_pct, int32_t range_km, int32_t outside_temp_c, uint32_t odometer_km,
                                double latitude, double longitude, float heading_deg, uint16_t doors, uint8_t gear,
                                uint8_t night_mode, uint8_t lights, uint8_t parking_brake, bool valid,
                                std::string_view vin) {
  b.fill(0);
  put_be32(b.data() + 0, uint32_t(speed_kph)); put_be32(b.data() + 4, uint32_t(rpm));
  put_be32(b.data() + 8, uint32_t(fuel_pct)); put_be32(b.data() + 12, uint32_t(range_km));
  put_be32(b.data() + 16, uint32_t(outside_temp_c)); put_be32(b.data() + 20, odometer_km);
  put_f64(b.data() + 24, latitude); put_f64(b.data() + 32, longitude); put_f32(b.data() + 40, heading_deg);
  put_be16(b.data() + 44, doors); b[46] = gear; b[47] = night_mode; b[48] = lights;
  b[49] = parking_brake; b[50] = valid ? 1 : 0;
  const std::size_t n = std::min<std::size_t>(vin.size(), kVehicleVinBytes - 1);
  std::memcpy(b.data() + kVehicleVinOffset, vin.data(), n);
}

// 文本助手：把定长 char[] 当 UTF-8 串用，按 UTF-8 边界回退（绝不劈开多字节字符）。
template <std::size_t N>
inline void copy_utf8(std::array<char, N>& dst, std::string_view src) {
  dst.fill(0);
  std::size_t n = std::min(src.size(), N - 1);
  while (n > 0 && n < src.size() && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
  std::memcpy(dst.data(), src.data(), n);
}
inline std::string_view as_view(const char* p, std::size_t cap) { return std::string_view(p, ::strnlen(p, cap)); }

// ── 导航逐向载荷（固定 464 字节，全 BE）──────────────────────────────────────
// 为什么不用 '\0' 连接文本：文本后面还要跟二进制字段（剩余距离/ETA/车道位图），
// 用 '\0' 连接会让接收侧无法判断“这是最后一个字符串还是二进制段的开始”。
// 所以四种文本字段一律定长 NUL 填充，偏移固定，跨语言实现不会歧义。
//
//   0..191   road_name            char[192]   （与 mvp::kMaxTextLong 同宽）
//   192..383 next_road_name       char[192]
//   384..415 icon                 char[32]    （转向图标名，原样透传供页面选图）
//   416..447 destination          char[32]
//   448..451 distance_remaining_m u32
//   452..459 eta_epoch_s          u64
//   460..461 lane_bitmap          u16
//   462      destination_reached  u8
//   463      保留                 u8
constexpr std::size_t kNavRoadOffset = 0, kNavRoadBytes = 192;
constexpr std::size_t kNavNextRoadOffset = 192, kNavNextRoadBytes = 192;
constexpr std::size_t kNavIconOffset = 384, kNavIconBytes = 32;
constexpr std::size_t kNavDestOffset = 416, kNavDestBytes = 32;
constexpr std::size_t kNavDistanceRemainingOffset = 448;
constexpr std::size_t kNavEtaOffset = 452;
constexpr std::size_t kNavLaneOffset = 460;
constexpr std::size_t kNavDestReachedOffset = 462;

struct NavigationFields {
  std::string_view road{}, next_road{}, icon{}, destination{};
  uint32_t distance_remaining_m{}; uint64_t eta_epoch_s{};
  uint16_t lane_bitmap{}; uint8_t destination_reached{};
};

inline void put_navigation_payload(std::array<uint8_t, kNavPayloadBytes>& b, const NavigationFields& f) {
  b.fill(0);
  const auto put_text = [&b](std::size_t off, std::size_t cap, std::string_view s) {
    std::size_t n = std::min(s.size(), cap - 1);
    // 按 UTF-8 边界回退，绝不劈开多字节字符（踩过的坑）。
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    std::memcpy(b.data() + off, s.data(), n);
  };
  put_text(kNavRoadOffset, kNavRoadBytes, f.road);
  put_text(kNavNextRoadOffset, kNavNextRoadBytes, f.next_road);
  put_text(kNavIconOffset, kNavIconBytes, f.icon);
  put_text(kNavDestOffset, kNavDestBytes, f.destination);
  put_be32(b.data() + kNavDistanceRemainingOffset, f.distance_remaining_m);
  put_be64(b.data() + kNavEtaOffset, f.eta_epoch_s);
  put_be16(b.data() + kNavLaneOffset, f.lane_bitmap);
  b[kNavDestReachedOffset] = f.destination_reached;
}

inline std::string_view nav_text(std::span<const uint8_t> p, std::size_t off, std::size_t cap) {
  return as_view(reinterpret_cast<const char*>(p.data()) + off, cap);
}

// 把 '\0' 分隔的字段序列按序取出（解码侧用；不分配内存）。
class FieldReader {
 public:
  FieldReader(std::span<const uint8_t> p) : p_(p) {}
  std::string_view next() {
    if (off_ >= p_.size()) return {};
    const char* base = reinterpret_cast<const char*>(p_.data()) + off_;
    const std::size_t room = p_.size() - off_;
    const std::size_t n = ::strnlen(base, room);
    off_ += n + (n < room ? 1 : 0);
    return std::string_view(base, n);
  }
 private:
  std::span<const uint8_t> p_;
  std::size_t off_{};
};

// ── 上行编码器（车机 → 引擎）。全部定长、无堆分配。──────────────────────────
struct OutRecord { std::array<uint8_t, kHeaderBytes + kMaxCtrlText> bytes{}; std::size_t size{}; };

inline OutRecord encode_up(UType t, uint32_t p0 = 0, uint32_t p1 = 0, uint32_t p2 = 0, uint32_t p3 = 0,
                           std::string_view text = {}, std::span<const uint8_t> blob = {}) {
  OutRecord out{};
  Header h{}; h.type = static_cast<Type>(t); h.p0 = p0; h.p1 = p1; h.p2 = p2; h.p3 = p3;
  const std::size_t body = blob.empty() ? std::min<std::size_t>(text.size(), kMaxCtrlText) : blob.size();
  h.payload_bytes = uint32_t(body);
  auto head = encode_header(h);
  std::memcpy(out.bytes.data(), head.data(), head.size());
  if (body) {
    if (!blob.empty()) std::memcpy(out.bytes.data() + kHeaderBytes, blob.data(), body);
    else std::memcpy(out.bytes.data() + kHeaderBytes, text.data(), body);
  }
  out.size = kHeaderBytes + body;
  return out;
}

// 多触点：每点 8 字节（int16 x、int16 y、uint8 id、uint8 phase、uint16 保留）。
inline OutRecord encode_multitouch(std::span<const TouchPoint> pts) {
  std::array<uint8_t, kMaxTouchPoints * 8> blob{};
  const std::size_t n = std::min<std::size_t>(pts.size(), kMaxTouchPoints);
  for (std::size_t i = 0; i < n; ++i) {
    uint8_t* p = blob.data() + i * 8;
    put_be16(p + 0, uint16_t(pts[i].x)); put_be16(p + 2, uint16_t(pts[i].y));
    p[4] = pts[i].id; p[5] = pts[i].phase; put_be16(p + 6, 0);
  }
  return encode_up(UType::MultiTouch, n, 0, 0, 0, {}, {blob.data(), n * 8});
}

} // namespace mvp::catplay_media::ext
