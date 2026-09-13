#pragma once

// Bounded read-only schema-v1 client for cp-native telemetry.  This is not a
// CarPlay transport: a reply only reports what the independent sidecar observed.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__unix__)
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace mvp {
constexpr std::size_t kCpNativeTelemetryReplyCap = 16U * 1024U;
constexpr std::size_t kCpNativeTelemetryTextCap = 160U;
// Retained for existing callers; schema parsing below provides the stronger gate.
inline bool bounded_json_object(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  if (text.size() < 2 || text.size() > kCpNativeTelemetryReplyCap || text.front() != '{' || text.back() != '}') return false;
  for (unsigned char byte : text) if (byte < 0x20U && byte != '\t') return false;
  return true;
}

enum class CpOfferState { Unsupported, NotOffered, NotNegotiated, Negotiating, Active, Failed };
inline constexpr std::string_view cp_offer_text(CpOfferState state) {
  switch (state) {
    case CpOfferState::Unsupported: return "unsupported";
    case CpOfferState::NotOffered: return "not-offered";
    case CpOfferState::NotNegotiated: return "not-negotiated";
    case CpOfferState::Negotiating: return "negotiating";
    case CpOfferState::Active: return "active";
    case CpOfferState::Failed: return "failed";
  }
  return "unsupported";
}

struct CpNativeTelemetryStatus {
  bool available{};
  uint16_t schema_version{};
  uint64_t session_id{}, last_updated_ms{};
  bool connected{}, negotiated{}, ready{};
  bool wireless_listener_ready{}, wired_endpoint_ready{}, iphone_connected{}, vehicle_connected{}, bridge_active{};
  std::array<char, kCpNativeTelemetryTextCap + 1> lifecycle{}, last_error{};
  CpOfferState bluetooth{CpOfferState::Unsupported}, wifi_bonjour{CpOfferState::Unsupported};
  CpOfferState iap2{CpOfferState::Unsupported}, mfi{CpOfferState::Unsupported}, hap{CpOfferState::Unsupported};
  CpOfferState rtsp{CpOfferState::Unsupported}, wired_usb{CpOfferState::Unsupported};
  CpOfferState main_screen{CpOfferState::Unsupported}, second_screen{CpOfferState::Unsupported};
  CpOfferState instrument_screen{CpOfferState::Unsupported}, media_audio{CpOfferState::Unsupported};
  CpOfferState microphone{CpOfferState::Unsupported}, hid{CpOfferState::Unsupported};
  CpOfferState vehicle{CpOfferState::Unsupported};
};

struct CpNativeTelemetryReply { CpNativeTelemetryStatus status{}; std::string json{}; };

namespace detail {
class JsonReader {
 public:
  explicit JsonReader(std::string_view input) : input_(input) {}
  void ws() { while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t' || input_[pos_] == '\r' || input_[pos_] == '\n')) ++pos_; }
  bool done() { ws(); return pos_ == input_.size(); }
  bool ch(char value) { ws(); if (pos_ == input_.size() || input_[pos_] != value) return false; ++pos_; return true; }
  bool literal(std::string_view value) { ws(); if (input_.substr(pos_, value.size()) != value) return false; pos_ += value.size(); return true; }
  bool boolean(bool& out) { if (literal("true")) { out = true; return true; } if (literal("false")) { out = false; return true; } return false; }
  bool number(uint64_t& out) {
    ws();
    if (pos_ == input_.size() || input_[pos_] < '0' || input_[pos_] > '9') return false;
    if (input_[pos_] == '0' && pos_ + 1 < input_.size() && input_[pos_ + 1] >= '0' && input_[pos_ + 1] <= '9') return false;
    uint64_t value = 0;
    do { const unsigned digit = unsigned(input_[pos_] - '0'); if (value > (UINT64_MAX - digit) / 10) return false; value = value * 10 + digit; ++pos_; } while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9');
    out = value; return true;
  }
  bool string(std::string_view& raw) {
    ws(); if (pos_ == input_.size() || input_[pos_++] != '\"') return false;
    const std::size_t begin = pos_;
    while (pos_ < input_.size()) {
      const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
      if (c == '\"') { raw = input_.substr(begin, pos_ - begin - 1); return valid_utf8(raw); }
      if (c < 0x20) return false;
      if (c == '\\') { if (pos_ == input_.size()) return false; const char escape = input_[pos_++]; if (escape == 'u') { for (int i = 0; i < 4; ++i) if (pos_ == input_.size() || !hex(input_[pos_++])) return false; } else if (escape != '\"' && escape != '\\' && escape != '/' && escape != 'b' && escape != 'f' && escape != 'n' && escape != 'r' && escape != 't') return false; }
    }
    return false;
  }
  bool skip() {
    ws(); if (pos_ == input_.size()) return false;
    const char c = input_[pos_];
    if (c == '\"') { std::string_view ignored; return string(ignored); }
    if (c == '{') { ++pos_; ws(); if (pos_ < input_.size() && input_[pos_] == '}') { ++pos_; return true; } while (true) { std::string_view key; if (!string(key) || !ch(':') || !skip()) return false; if (ch('}')) return true; if (!ch(',')) return false; } }
    if (c == '[') { ++pos_; ws(); if (pos_ < input_.size() && input_[pos_] == ']') { ++pos_; return true; } while (true) { if (!skip()) return false; if (ch(']')) return true; if (!ch(',')) return false; } }
    if (c == '-' || (c >= '0' && c <= '9')) return skip_number();
    return literal("true") || literal("false") || literal("null");
  }
 private:
  bool skip_number() {
    if (input_[pos_] == '-') {
      ++pos_;
      if (pos_ == input_.size() || input_[pos_] < '0' || input_[pos_] > '9') return false;
    }
    if (input_[pos_] == '0') {
      ++pos_;
      if (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') return false;
    } else {
      if (input_[pos_] < '1' || input_[pos_] > '9') return false;
      do { ++pos_; } while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9');
    }
    if (pos_ < input_.size() && input_[pos_] == '.') {
      ++pos_;
      const std::size_t fraction = pos_;
      while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
      if (pos_ == fraction) return false;
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
      const std::size_t exponent = pos_;
      while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
      if (pos_ == exponent) return false;
    }
    return true;
  }
  static bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
  static bool valid_utf8(std::string_view value) { for (std::size_t i=0; i<value.size();) { const unsigned char c=static_cast<unsigned char>(value[i++]); if (c < 0x80) continue; unsigned count=0; uint32_t code=0; if ((c&0xe0)==0xc0) { count=1; code=c&0x1f; } else if ((c&0xf0)==0xe0) { count=2; code=c&0x0f; } else if ((c&0xf8)==0xf0) { count=3; code=c&0x07; } else return false; if (i+count>value.size()) return false; for (unsigned j=0;j<count;++j) { const unsigned char next=static_cast<unsigned char>(value[i++]); if ((next&0xc0)!=0x80) return false; code=(code<<6)|(next&0x3f); } if ((count==1&&code<0x80)||(count==2&&code<0x800)||(count==3&&(code<0x10000||code>0x10ffff||(code>=0xd800&&code<=0xdfff)))) return false; } return true; }
  std::string_view input_; std::size_t pos_{};
};

inline bool copy_json_string(std::string_view raw, std::array<char, kCpNativeTelemetryTextCap + 1>& out) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < raw.size();) {
    unsigned char c = static_cast<unsigned char>(raw[i++]);
    if (c == '\\') { if (i == raw.size()) return false; const char e = raw[i++]; if (e == 'u') { if (i + 4 > raw.size()) return false; for (int j = 0; j < 4; ++j) if (!((raw[i + j] >= '0' && raw[i + j] <= '9') || (raw[i + j] >= 'a' && raw[i + j] <= 'f') || (raw[i + j] >= 'A' && raw[i + j] <= 'F'))) return false; i += 4; c = '?'; } else { switch (e) { case '\"': c = '\"'; break; case '\\': c = '\\'; break; case '/': c = '/'; break; case 'b': case 'f': case 'n': case 'r': case 't': c = ' '; break; default: return false; } } }
    if (c < 0x20 || n >= kCpNativeTelemetryTextCap) return false;
    out[n++] = static_cast<char>(c);
  }
  out[n] = 0; return true;
}
inline bool offer(std::string_view text, CpOfferState& out) {
  if (text == "unsupported") out = CpOfferState::Unsupported;
  else if (text == "not-offered") out = CpOfferState::NotOffered;
  else if (text == "not-negotiated") out = CpOfferState::NotNegotiated;
  else if (text == "negotiating") out = CpOfferState::Negotiating;
  else if (text == "active") out = CpOfferState::Active;
  else if (text == "failed") out = CpOfferState::Failed;
  else return false;
  return true;
}
inline bool parse_feature(JsonReader& r, std::string_view key, CpOfferState& value, unsigned& seen) {
  if (++seen != 1) return false; std::string_view raw; return r.string(raw) && offer(raw, value);
}
inline bool parse_features(JsonReader& r, CpNativeTelemetryStatus& s) {
  if (!r.ch('{')) return false;
  unsigned bluetooth=0,wifi=0,iap2=0,mfi=0,hap=0,rtsp=0,wired=0,main=0,second=0,instrument=0,media=0,call=0,siri=0,prompt=0,mic=0,hid=0,now=0,lyrics=0,gps=0,vehicle=0,navigation=0,phone=0;
  CpOfferState ignored_call{}, ignored_siri{}, ignored_prompt{}, ignored_now{}, ignored_lyrics{}, ignored_gps{}, ignored_navigation{}, ignored_phone{};
  if (r.ch('}')) return false;
  while (true) {
    std::string_view key; if (!r.string(key) || !r.ch(':')) return false;
    const bool ok = key == "bluetooth" ? parse_feature(r,key,s.bluetooth,bluetooth) : key == "wifi_bonjour" ? parse_feature(r,key,s.wifi_bonjour,wifi) : key == "iap2" ? parse_feature(r,key,s.iap2,iap2) : key == "mfi" ? parse_feature(r,key,s.mfi,mfi) : key == "hap" ? parse_feature(r,key,s.hap,hap) : key == "rtsp" ? parse_feature(r,key,s.rtsp,rtsp) : key == "wired_usb" ? parse_feature(r,key,s.wired_usb,wired) : key == "main_screen" ? parse_feature(r,key,s.main_screen,main) : key == "second_screen" ? parse_feature(r,key,s.second_screen,second) : key == "instrument_screen" ? parse_feature(r,key,s.instrument_screen,instrument) : key == "media_audio" ? parse_feature(r,key,s.media_audio,media) : key == "microphone" ? parse_feature(r,key,s.microphone,mic) : key == "hid" ? parse_feature(r,key,s.hid,hid) : key == "vehicle" ? parse_feature(r,key,s.vehicle,vehicle) : key == "call_audio" ? parse_feature(r,key,ignored_call,call) : key == "siri" ? parse_feature(r,key,ignored_siri,siri) : key == "prompt_audio" ? parse_feature(r,key,ignored_prompt,prompt) : key == "now_playing" ? parse_feature(r,key,ignored_now,now) : key == "lyrics" ? parse_feature(r,key,ignored_lyrics,lyrics) : key == "gps" ? parse_feature(r,key,ignored_gps,gps) : key == "navigation" ? parse_feature(r,key,ignored_navigation,navigation) : key == "phone" ? parse_feature(r,key,ignored_phone,phone) : r.skip();
    if (!ok) return false;
    if (r.ch(',')) continue;
    if (!r.ch('}')) return false;
    break;
  }
  return bluetooth && wifi && iap2 && mfi && hap && rtsp && wired && main && second && instrument && media && call && siri && prompt && mic && hid && now && lyrics && gps && vehicle && navigation && phone;
}
}  // namespace detail

inline bool parse_cp_native_telemetry_line(std::string_view line, CpNativeTelemetryStatus& out) {
  if (line.size() > kCpNativeTelemetryReplyCap) return false;
  detail::JsonReader r(line); if (!r.ch('{')) return false;
  unsigned id=0, ok=0, payload=0; uint64_t reply_id=0; bool reply_ok=false; CpNativeTelemetryStatus next{};
  if (r.ch('}')) return false;
  while (true) {
    std::string_view key; if (!r.string(key) || !r.ch(':')) return false;
    bool valid = true;
    if (key == "id") valid = ++id == 1 && r.number(reply_id);
    else if (key == "ok") valid = ++ok == 1 && r.boolean(reply_ok);
    else if (key == "payload") {
      valid = ++payload == 1 && r.ch('{');
      unsigned schema=0, session=0, lifecycle=0, wireless=0, wired=0, iphone=0, vehicle=0, bridge=0, connected=0, features=0, error=0;
      if (valid && r.ch('}')) valid = false;
      while (valid) {
        std::string_view field; valid = r.string(field) && r.ch(':'); if (!valid) break;
        if (field == "schema_version") { uint64_t v=0; valid = ++schema == 1 && r.number(v) && v == 1; next.schema_version = static_cast<uint16_t>(v); }
        else if (field == "session_id") valid = ++session == 1 && r.number(next.session_id);
        else if (field == "lifecycle") { std::string_view v; valid = ++lifecycle == 1 && r.string(v) && detail::copy_json_string(v,next.lifecycle); }
        else if (field == "wireless_listener_ready") valid = ++wireless == 1 && r.boolean(next.wireless_listener_ready);
        else if (field == "wired_endpoint_ready") valid = ++wired == 1 && r.boolean(next.wired_endpoint_ready);
        else if (field == "iphone_connected") valid = ++iphone == 1 && r.boolean(next.iphone_connected);
        else if (field == "vehicle_connected") valid = ++vehicle == 1 && r.boolean(next.vehicle_connected);
        else if (field == "bridge_active") valid = ++bridge == 1 && r.boolean(next.bridge_active);
        else if (field == "connected") valid = ++connected == 1 && r.boolean(next.connected);
        else if (field == "features") valid = ++features == 1 && detail::parse_features(r,next);
        else if (field == "last_error") { std::string_view v; valid = ++error == 1 && r.string(v) && detail::copy_json_string(v,next.last_error); }
        else valid = r.skip();
        if (!valid) break;
        if (r.ch(',')) continue;
        valid = r.ch('}');
        break;
      }
      valid = valid && schema && session && lifecycle && wireless && wired && iphone && vehicle && bridge && connected && features && error;
    } else valid = r.skip();
    if (!valid) return false;
    if (r.ch(',')) continue;
    if (!r.ch('}')) return false;
    break;
  }
  if (!r.done() || id != 1 || ok != 1 || payload != 1 || reply_id != 1 || !reply_ok) return false;
  const bool evidence = next.main_screen == CpOfferState::Active || next.main_screen == CpOfferState::Negotiating || next.media_audio == CpOfferState::Active || next.media_audio == CpOfferState::Negotiating || next.hap == CpOfferState::Active || next.hap == CpOfferState::Negotiating || next.rtsp == CpOfferState::Active || next.rtsp == CpOfferState::Negotiating;
  next.negotiated = next.connected && evidence;
  next.ready = next.connected && next.main_screen == CpOfferState::Active && next.media_audio == CpOfferState::Active;
  next.available = true; next.last_updated_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); out = next; return true;
}

inline CpNativeTelemetryReply read_cp_native_telemetry() {
#if defined(__unix__)
  const char* configured = std::getenv("CP_CORE_SOCKET");
  const std::string path = configured && *configured ? configured : "/run/zero2w/cp-native.sock";
  sockaddr_un address{}; if (path.empty() || path.size() >= sizeof(address.sun_path)) return {};
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return {};
  // cp-native may be servicing the engine supervisor on the same small core;
  // 200 ms caused false "unavailable" samples on the 1 GB Zero2W even though
  // the same reply parsed correctly. Keep the request bounded but allow one second.
  timeval timeout{1, 0}; (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)); (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  address.sun_family = AF_UNIX; std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) { ::close(fd); return {}; }
  constexpr std::string_view request = "{\"op\":\"telemetry\",\"id\":1}\n";
  if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) { ::close(fd); return {}; }
  std::string text; text.reserve(1024); std::array<char,512> buffer{};
  while (text.size() <= kCpNativeTelemetryReplyCap) { const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0); if (count <= 0) break; text.append(buffer.data(), static_cast<std::size_t>(count)); if (text.find('\n') != std::string::npos) break; }
  ::close(fd); const auto newline = text.find('\n'); if (newline == std::string::npos) return {}; text.resize(newline);
  CpNativeTelemetryStatus status; if (!parse_cp_native_telemetry_line(text,status)) return {}; return {status, std::move(text)};
#else
  return {};
#endif
}
}  // namespace mvp
