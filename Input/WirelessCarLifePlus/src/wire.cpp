#include "carlife/wire.h"

#include <cstring>

#include "carlife/service_types.h"

namespace carlife {

void be16_put(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}
void be32_put(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[3] = static_cast<uint8_t>(v & 0xFF);
}
uint16_t be16_get(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t be32_get(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool isShortHeaderChannel(int32_t channel) {
  return channel == ch::CMD || channel == ch::TOUCH;
}
int headerSize(int32_t channel) { return isShortHeaderChannel(channel) ? 8 : 12; }

std::vector<uint8_t> Frame::encode() const {
  const int hs = headerSize(channel);
  std::vector<uint8_t> out(static_cast<size_t>(hs) + payload.size());
  if (isShortHeaderChannel(channel)) {
    be16_put(out.data(), static_cast<uint16_t>(payload.size()));
    out[2] = 0;
    out[3] = 0;
    be32_put(out.data() + 4, serviceType);
  } else {
    be32_put(out.data(), static_cast<uint32_t>(payload.size()));
    be32_put(out.data() + 4, timestamp);
    be32_put(out.data() + 8, serviceType);
  }
  if (!payload.empty()) {
    std::memcpy(out.data() + hs, payload.data(), payload.size());
  }
  return out;
}

bool parseHeader(const uint8_t* p, int32_t channel, HeaderInfo* out) {
  if (isShortHeaderChannel(channel)) {
    out->payloadSize = be16_get(p);
    out->timestamp = 0;
    out->serviceType = be32_get(p + 4);
  } else {
    out->payloadSize = be32_get(p);
    out->timestamp = be32_get(p + 4);
    out->serviceType = be32_get(p + 8);
  }
  return true;
}

// ---------------------------------------------------------------- protobuf
void PbWriter::rawVarint(uint64_t v) {
  do {
    uint8_t b = static_cast<uint8_t>(v & 0x7F);
    v >>= 7;
    if (v) b |= 0x80;
    buf_.push_back(b);
  } while (v);
}
void PbWriter::fieldVarint(int field, uint64_t v) {
  rawVarint(static_cast<uint64_t>(field) << 3);
  rawVarint(v);
}
void PbWriter::fieldInt32(int field, int32_t v) {
  fieldVarint(field, static_cast<uint64_t>(static_cast<int64_t>(v)));
}
void PbWriter::fieldBool(int field, bool v) { fieldVarint(field, v ? 1 : 0); }
void PbWriter::fieldBytes(int field, const uint8_t* p, size_t n) {
  rawVarint((static_cast<uint64_t>(field) << 3) | 2);
  rawVarint(n);
  buf_.insert(buf_.end(), p, p + n);
}
void PbWriter::fieldString(int field, const std::string& s) {
  fieldBytes(field, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
void PbWriter::fieldMessage(int field, const PbWriter& sub) {
  fieldBytes(field, sub.data().data(), sub.size());
}
// zigzag：sint32/sint64 的编码方式（protobuf 规范）
void PbWriter::fieldSint32(int field, int32_t v) {
  fieldVarint(field, static_cast<uint64_t>(static_cast<uint32_t>((v << 1) ^ (v >> 31))));
}
void PbWriter::fieldSint64(int field, int64_t v) {
  fieldVarint(field, static_cast<uint64_t>((v << 1) ^ (v >> 63)));
}
void PbWriter::fieldFloat(int field, float v) {
  rawVarint((static_cast<uint64_t>(field) << 3) | 5);
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
}
void PbWriter::fieldDouble(int field, double v) {
  rawVarint((static_cast<uint64_t>(field) << 3) | 1);
  uint64_t bits = 0;
  std::memcpy(&bits, &v, 8);
  for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
}

bool PbReader::next(uint32_t* field, uint32_t* wireType) {
  uint64_t tag = 0;
  if (!readVarint(&tag)) return false;
  *field = static_cast<uint32_t>(tag >> 3);
  *wireType = static_cast<uint32_t>(tag & 7);
  return *field != 0;
}
bool PbReader::readVarint(uint64_t* v) {
  uint64_t result = 0;
  int shift = 0;
  for (int i = 0; i < 10; ++i) {
    if (p_ >= end_) return false;
    uint8_t b = *p_++;
    result |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      *v = result;
      return true;
    }
    shift += 7;
  }
  return false;
}
bool PbReader::readInt32(int32_t* v) {
  uint64_t raw = 0;
  if (!readVarint(&raw)) return false;
  *v = static_cast<int32_t>(static_cast<uint32_t>(raw));
  return true;
}
bool PbReader::readBool(bool* v) {
  uint64_t raw = 0;
  if (!readVarint(&raw)) return false;
  *v = raw != 0;
  return true;
}
bool PbReader::readLengthDelimited(const uint8_t** p, size_t* n) {
  uint64_t len = 0;
  if (!readVarint(&len)) return false;
  if (len > static_cast<uint64_t>(end_ - p_)) return false;
  *p = p_;
  *n = static_cast<size_t>(len);
  p_ += len;
  return true;
}
bool PbReader::readString(std::string* s) {
  const uint8_t* p = nullptr;
  size_t n = 0;
  if (!readLengthDelimited(&p, &n)) return false;
  s->assign(reinterpret_cast<const char*>(p), n);
  return true;
}
bool PbReader::skip(uint32_t wireType) {
  switch (wireType) {
    case 0: {
      uint64_t v = 0;
      return readVarint(&v);
    }
    case 1:
      if (end_ - p_ < 8) return false;
      p_ += 8;
      return true;
    case 2: {
      const uint8_t* p = nullptr;
      size_t n = 0;
      return readLengthDelimited(&p, &n);
    }
    case 5:
      if (end_ - p_ < 4) return false;
      p_ += 4;
      return true;
    default:
      return false;
  }
}
bool PbReader::readUint32(uint32_t* v) {
  uint64_t raw = 0;
  if (!readVarint(&raw)) return false;
  *v = static_cast<uint32_t>(raw);
  return true;
}
bool PbReader::readUint64(uint64_t* v) { return readVarint(v); }
bool PbReader::readSint32(int32_t* v) {
  uint64_t raw = 0;
  if (!readVarint(&raw)) return false;
  const uint32_t u = static_cast<uint32_t>(raw);
  *v = static_cast<int32_t>((u >> 1) ^ (~(u & 1) + 1));
  return true;
}
bool PbReader::readSint64(int64_t* v) {
  uint64_t raw = 0;
  if (!readVarint(&raw)) return false;
  *v = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
  return true;
}
bool PbReader::readFloat(float* v) {
  if (end_ - p_ < 4) return false;
  uint32_t bits = 0;
  for (int i = 0; i < 4; ++i) bits |= static_cast<uint32_t>(p_[i]) << (8 * i);
  p_ += 4;
  std::memcpy(v, &bits, 4);
  return true;
}
bool PbReader::readDouble(double* v) {
  if (end_ - p_ < 8) return false;
  uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) bits |= static_cast<uint64_t>(p_[i]) << (8 * i);
  p_ += 8;
  std::memcpy(v, &bits, 8);
  return true;
}

// ---------------------------------------------------------------- messages
namespace pb {

std::vector<uint8_t> ProtocolVersion::encode() const {
  PbWriter w;
  w.fieldInt32(1, majorVersion);
  w.fieldInt32(2, minorVersion);
  return w.data();
}

bool ProtocolVersionMatchStatus::decode(const uint8_t* p, size_t n,
                                        ProtocolVersionMatchStatus* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 0) {
      if (!r.readInt32(&out->matchStatus)) return false;
    } else if (field == 2 && wire == 0) {
      if (!r.readInt32(&out->carlifeProtocolVersion)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> AuthenRequest::encode() const {
  PbWriter w;
  w.fieldString(1, randomValue);
  return w.data();
}

std::vector<uint8_t> AuthenResponse::encode() const {
  PbWriter w;
  w.fieldString(1, encryptValue);
  return w.data();
}

bool AuthenResponse::decode(const uint8_t* p, size_t n, AuthenResponse* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 2) {
      if (!r.readString(&out->encryptValue)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> AuthenResult::encode() const {
  PbWriter w;
  w.fieldBool(1, authenResult);
  return w.data();
}

bool AuthenResult::decode(const uint8_t* p, size_t n, AuthenResult* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 0) {
      if (!r.readBool(&out->authenResult)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> VideoEncoderInfo::encode() const {
  PbWriter w;
  w.fieldInt32(1, width);
  w.fieldInt32(2, height);
  w.fieldInt32(3, frameRate);
  return w.data();
}

bool VideoEncoderInfo::decode(const uint8_t* p, size_t n, VideoEncoderInfo* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire != 0) {
      if (!r.skip(wire)) return false;
      continue;
    }
    int32_t v = 0;
    if (!r.readInt32(&v)) return false;
    if (field == 1) out->width = v;
    if (field == 2) out->height = v;
    if (field == 3) out->frameRate = v;
  }
  return true;
}

std::vector<uint8_t> VideoFrameRate::encode() const {
  PbWriter w;
  w.fieldInt32(1, frameRate);
  return w.data();
}

bool VideoFrameRate::decode(const uint8_t* p, size_t n, VideoFrameRate* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 0) {
      if (!r.readInt32(&out->frameRate)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> DeviceInfo::encode() const {
  PbWriter w;
  if (!os.empty()) w.fieldString(1, os);
  if (!board.empty()) w.fieldString(2, board);
  if (!bootloader.empty()) w.fieldString(3, bootloader);
  if (!brand.empty()) w.fieldString(4, brand);
  if (!cpuAbi.empty()) w.fieldString(5, cpuAbi);
  if (!cpuAbi2.empty()) w.fieldString(6, cpuAbi2);
  if (!device.empty()) w.fieldString(7, device);
  if (!display.empty()) w.fieldString(8, display);
  if (!fingerprint.empty()) w.fieldString(9, fingerprint);
  if (!hardware.empty()) w.fieldString(10, hardware);
  if (!host.empty()) w.fieldString(11, host);
  if (!cid.empty()) w.fieldString(12, cid);
  if (!manufacturer.empty()) w.fieldString(13, manufacturer);
  if (!model.empty()) w.fieldString(14, model);
  if (!product.empty()) w.fieldString(15, product);
  if (!serial.empty()) w.fieldString(16, serial);
  if (!codename.empty()) w.fieldString(17, codename);
  if (!incremental.empty()) w.fieldString(18, incremental);
  if (!release.empty()) w.fieldString(19, release);
  if (!sdk.empty()) w.fieldString(20, sdk);
  w.fieldInt32(21, sdkInt);
  if (!token.empty()) w.fieldString(22, token);
  if (!btAddress.empty()) w.fieldString(23, btAddress);
  if (!carlifeVersion.empty()) w.fieldString(24, carlifeVersion);
  return w.data();
}

bool DeviceInfo::decode(const uint8_t* p, size_t n, DeviceInfo* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire == 2) {
      std::string s;
      if (!r.readString(&s)) return false;
      switch (field) {
        case 1: out->os = s; break;
        case 2: out->board = s; break;
        case 3: out->bootloader = s; break;
        case 4: out->brand = s; break;
        case 5: out->cpuAbi = s; break;
        case 6: out->cpuAbi2 = s; break;
        case 7: out->device = s; break;
        case 8: out->display = s; break;
        case 9: out->fingerprint = s; break;
        case 10: out->hardware = s; break;
        case 11: out->host = s; break;
        case 12: out->cid = s; break;
        case 13: out->manufacturer = s; break;
        case 14: out->model = s; break;
        case 15: out->product = s; break;
        case 16: out->serial = s; break;
        case 17: out->codename = s; break;
        case 18: out->incremental = s; break;
        case 19: out->release = s; break;
        case 20: out->sdk = s; break;
        case 22: out->token = s; break;
        case 23: out->btAddress = s; break;
        case 24: out->carlifeVersion = s; break;
        default: break;
      }
    } else if (wire == 0) {
      if (field == 21) {
        if (!r.readInt32(&out->sdkInt)) return false;
      } else if (!r.skip(wire)) {
        return false;
      }
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> StatisticsInfo::encode() const {
  PbWriter w;
  w.fieldString(1, cuid);
  w.fieldString(2, versionName);
  w.fieldInt32(3, versionCode);
  w.fieldString(4, channel);
  w.fieldInt32(5, connectCount);
  w.fieldInt32(6, connectSuccessCount);
  w.fieldInt32(7, connectTime);
  return w.data();
}

std::vector<uint8_t> FeatureConfigList::encode() const {
  PbWriter w;
  w.fieldInt32(1, static_cast<int32_t>(configs.size()));
  for (const auto& c : configs) {
    PbWriter sub;
    sub.fieldString(1, c.key);
    sub.fieldInt32(2, c.value);
    w.fieldMessage(2, sub);
  }
  w.fieldBool(3, huBtAudioSupport);
  if (!huBtName.empty()) w.fieldString(4, huBtName);
  if (!huBtMac.empty()) w.fieldString(5, huBtMac);
  return w.data();
}

bool FeatureConfigList::decode(const uint8_t* p, size_t n, FeatureConfigList* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 2 && wire == 2) {
      const uint8_t* sp = nullptr;
      size_t sn = 0;
      if (!r.readLengthDelimited(&sp, &sn)) return false;
      FeatureConfig c;
      PbReader sr(sp, sn);
      uint32_t f2 = 0, w2 = 0;
      while (sr.next(&f2, &w2)) {
        if (f2 == 1 && w2 == 2) {
          if (!sr.readString(&c.key)) return false;
        } else if (f2 == 2 && w2 == 0) {
          if (!sr.readInt32(&c.value)) return false;
        } else if (!sr.skip(w2)) {
          return false;
        }
      }
      out->configs.push_back(c);
    } else if (field == 3 && wire == 0) {
      if (!r.readBool(&out->huBtAudioSupport)) return false;
    } else if (field == 4 && wire == 2) {
      if (!r.readString(&out->huBtName)) return false;
    } else if (field == 5 && wire == 2) {
      if (!r.readString(&out->huBtMac)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> TouchSinglePoint::encode() const {
  PbWriter w;
  w.fieldInt32(1, x);
  w.fieldInt32(2, y);
  w.fieldInt32(3, pointerX);
  w.fieldInt32(4, pointerY);
  return w.data();
}

std::vector<uint8_t> CarHardKeyCode::encode() const {
  PbWriter w;
  w.fieldInt32(1, keycode);
  return w.data();
}

// 依据 CarlifeMediaProgressBarProto.proto：required int32 progressBar = 1;
bool MediaProgressBar::decode(const uint8_t* p, size_t n, MediaProgressBar* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  bool seen = false;
  while (r.next(&field, &wire)) {
    if (wire != 0) {
      if (!r.skip(wire)) return false;
      continue;
    }
    int32_t v = 0;
    if (!r.readInt32(&v)) return false;
    if (field == 1) {
      out->progressBar = v;
      seen = true;
    }
  }
  // required 字段缺失视为解析失败，以便上层区分“空载荷”与“真值”。
  return seen;
}

bool ModuleStatusList::decode(const uint8_t* p, size_t n, ModuleStatusList* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 2 && wire == 2) {
      const uint8_t* sp = nullptr;
      size_t sn = 0;
      if (!r.readLengthDelimited(&sp, &sn)) return false;
      int32_t moduleID = 0, statusID = 0;
      PbReader sr(sp, sn);
      uint32_t f2 = 0, w2 = 0;
      while (sr.next(&f2, &w2)) {
        if (f2 == 1 && w2 == 0) {
          if (!sr.readInt32(&moduleID)) return false;
        } else if (f2 == 2 && w2 == 0) {
          if (!sr.readInt32(&statusID)) return false;
        } else if (!sr.skip(w2)) {
          return false;
        }
      }
      out->items.emplace_back(moduleID, statusID);
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool AudioInit::decode(const uint8_t* p, size_t n, AudioInit* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire != 0) {
      if (!r.skip(wire)) return false;
      continue;
    }
    int32_t v = 0;
    if (!r.readInt32(&v)) return false;
    if (field == 1) out->sampleRate = v;
    if (field == 2) out->channelConfig = v;
    if (field == 3) out->sampleFormat = v;
  }
  return true;
}

// ════════════════════════════════════════════════════════════════
// A..U：本轮补齐的全部 CarLife 消息。
// 【统一的健壮性约定】解析时遇到不认识的字段一律 skip，只对
// “required 字段缺失”或“wire 格式错”返回 false —— 不因为多了新字段就断会话。
// ════════════════════════════════════════════════════════════════
namespace {
// repeated 标量在 proto2 里默认是【未打包】的（每个元素一个 tag），
// 但很多实现（包括 protobuf 的 Java/C++）会在可打包时打包。
// 这里两种形式都接受，否则遇到打包的发送方会整条解析失败。
struct RepeatedReader {
  // 返回 true 表示读到了一个值；false 表示该字段已结束或格式错。
  template <typename F>
  static bool each(PbReader& r, uint32_t wire, F&& f) {
    if (wire == 2) {  // packed
      const uint8_t* p = nullptr;
      size_t n = 0;
      if (!r.readLengthDelimited(&p, &n)) return false;
      PbReader sub(p, n);
      while (sub.remaining() > 0) {
        uint64_t raw = 0;
        if (!sub.readVarint(&raw)) return false;
        f(raw);
      }
      return true;
    }
    if (wire == 0 || wire == 5 || wire == 1) {
      uint64_t raw = 0;
      if (wire == 0) {
        if (!r.readVarint(&raw)) return false;
      }
      f(raw);
      return true;
    }
    return false;
  }
};
}  // namespace

bool MediaInfo::decode(const uint8_t* p, size_t n, MediaInfo* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire == 2) {
      std::string s;
      if (!r.readString(&s)) return false;
      switch (field) {
        case 1: out->source = s; break;
        case 2: out->song = s; out->has_song = true; break;
        case 3: out->artist = s; break;
        case 4: out->album = s; break;
        case 5: out->album_art = s; break;
        case 8: out->song_id = s; break;
        default: break;
      }
    } else if (wire == 0) {
      int32_t v = 0;
      if (!r.readInt32(&v)) return false;
      if (field == 6) out->duration = v;
      if (field == 7) out->playlist_num = v;
      if (field == 9) out->mode = v;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  // song 是 required：缺了就算解析失败，避免上层把空壳当成一条元数据。
  return out->has_song;
}

bool NaviNextTurnInfo::decode(const uint8_t* p, size_t n, NaviNextTurnInfo* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire == 2) {
      std::string s;
      if (!r.readString(&s)) return false;
      if (field == 3) out->road_name = s;
      // field 6：Kotlin SDK 版是 bytes turnIconData
      if (field == 6) out->turn_icon = s;
    } else if (wire == 0) {
      int32_t v = 0;
      if (!r.readInt32(&v)) return false;
      if (field == 1) { out->action = v; out->has_action = true; }
      if (field == 2) out->next_turn = v;
      if (field == 4) out->total_distance = v;
      if (field == 5) out->remain_distance = v;
      // field 6：C++ 库版是 int32 time
      if (field == 6) out->time_s = v;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return out->has_action;
}

bool NaviAssistantGuideInfo::decode(const uint8_t* p, size_t n, NaviAssistantGuideInfo* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire != 0) {
      if (!r.skip(wire)) return false;
      continue;
    }
    int32_t v = 0;
    if (!r.readInt32(&v)) return false;
    switch (field) {
      case 1: out->action = v; break;
      case 2: out->assistant_type = v; break;
      case 3: out->traffic_sign_type = v; break;
      case 4: out->total_distance = v; break;
      case 5: out->remain_distance = v; break;
      case 6: out->camera_speed = v; break;
      default: break;
    }
  }
  return true;
}

bool ContactsList::decode(const uint8_t* p, size_t n, ContactsList* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 0) {
      if (!r.readInt32(&out->cnt)) return false;
    } else if (field == 2 && wire == 2) {
      const uint8_t* sp = nullptr;
      size_t sn = 0;
      if (!r.readLengthDelimited(&sp, &sn)) return false;
      Contact c;
      PbReader sr(sp, sn);
      uint32_t f2 = 0, w2 = 0;
      while (sr.next(&f2, &w2)) {
        if (w2 == 2) {
          std::string s;
          if (!sr.readString(&s)) return false;
          if (f2 == 2) c.name = s;
          if (f2 == 3) c.number = s;
        } else if (w2 == 0) {
          int32_t v = 0;
          if (!sr.readInt32(&v)) return false;
          if (f2 == 1) c.cid = v;
        } else if (!sr.skip(w2)) {
          return false;
        }
      }
      out->contacts.push_back(std::move(c));
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool CallRecordsList::decode(const uint8_t* p, size_t n, CallRecordsList* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 0) {
      if (!r.readInt32(&out->cnt)) return false;
    } else if (field == 2 && wire == 2) {
      const uint8_t* sp = nullptr;
      size_t sn = 0;
      if (!r.readLengthDelimited(&sp, &sn)) return false;
      CallRecord rec;
      PbReader sr(sp, sn);
      uint32_t f2 = 0, w2 = 0;
      while (sr.next(&f2, &w2)) {
        if (w2 == 2) {
          std::string s;
          if (!sr.readString(&s)) return false;
          switch (f2) {
            case 2: rec.name = s; break;
            case 3: rec.number = s; break;
            case 4: rec.duration = s; break;
            case 5: rec.time = s; break;
            default: break;
          }
        } else if (w2 == 0) {
          int32_t v = 0;
          if (!sr.readInt32(&v)) return false;
          if (f2 == 1) rec.cid = v;
          if (f2 == 6) rec.type = v;
        } else if (!sr.skip(w2)) {
          return false;
        }
      }
      out->records.push_back(std::move(rec));
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool BTHfpRequest::decode(const uint8_t* p, size_t n, BTHfpRequest* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire == 2) {
      if (field == 2) {
        if (!r.readString(&out->phone_num)) return false;
      } else if (!r.skip(wire)) {
        return false;
      }
    } else if (wire == 0) {
      int32_t v = 0;
      if (!r.readInt32(&v)) return false;
      if (field == 1) out->command = v;
      if (field == 3) out->dtmf_code = v;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> BTHfpResponse::encode() const {
  PbWriter w;
  w.fieldInt32(1, status);
  w.fieldInt32(2, cmd);
  if (dtmf_code) w.fieldInt32(3, dtmf_code);
  return w.data();
}

std::vector<uint8_t> BTHfpIndication::encode() const {
  PbWriter w;
  w.fieldInt32(1, state);
  if (!phone_num.empty()) w.fieldString(2, phone_num);
  if (!phone_name.empty()) w.fieldString(3, phone_name);
  if (!address.empty()) w.fieldString(4, address);
  return w.data();
}

std::vector<uint8_t> BTHfpConnection::encode() const {
  PbWriter w;
  w.fieldInt32(1, state);
  if (!address.empty()) w.fieldString(2, address);
  if (!name.empty()) w.fieldString(3, name);
  return w.data();
}

bool BTHfpStatusRequest::decode(const uint8_t* p, size_t n, BTHfpStatusRequest* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 0) {
      if (!r.readInt32(&out->type)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> BTHfpStatusResponse::encode() const {
  PbWriter w;
  w.fieldInt32(1, status);
  w.fieldInt32(2, type);
  return w.data();
}

bool BTHfpCallStatusCover::decode(const uint8_t* p, size_t n, BTHfpCallStatusCover* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire == 2) {
      std::string s;
      if (!r.readString(&s)) return false;
      if (field == 2) out->phone_num = s;
      if (field == 3) out->name = s;
    } else if (wire == 0) {
      if (field == 1) {
        if (!r.readInt32(&out->state)) return false;
      } else if (!r.skip(wire)) {
        return false;
      }
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool ActiveRequest::decode(const uint8_t* p, size_t n, ActiveRequest* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 2) {
      if (!r.readString(&out->mac)) return false;
    } else if (wire == 0) {
      int32_t v = 0;
      if (!r.readInt32(&v)) return false;
      if (field == 2) out->is_active = v;
      if (field == 3) out->random_value = v;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> ActiveResponse::encode() const {
  PbWriter w;
  w.fieldString(1, statue);
  if (!token.empty()) w.fieldString(2, token);
  return w.data();
}

std::vector<uint8_t> HuRsaPublicKeyResponse::encode() const {
  PbWriter w;
  w.fieldString(1, rsa_public_key);
  return w.data();
}

bool MdAesKeyRequest::decode(const uint8_t* p, size_t n, MdAesKeyRequest* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 2) {
      if (!r.readString(&out->aes_key)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool FileTransferBegin::decode(const uint8_t* p, size_t n, FileTransferBegin* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire != 0) {
      if (!r.skip(wire)) return false;
      continue;
    }
    if (field == 1) {
      uint64_t v = 0;
      if (!r.readUint64(&v)) return false;
      out->file_size = static_cast<int64_t>(v);
    } else if (field == 2) {
      if (!r.readInt32(&out->version)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool VehicleControl::decode(const uint8_t* p, size_t n, VehicleControl* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    switch (field) {
      case 4:  // token_string
      case 8:  // bytes_value
      case 12: // string_value
        if (wire != 2) {
          if (!r.skip(wire)) return false;
          break;
        }
        {
          std::string s;
          if (!r.readString(&s)) return false;
          if (field == 4) out->token_string = s;
          if (field == 8) out->bytes_value = s;
          if (field == 12) out->string_value = s;
        }
        break;
      case 6:  // repeated int32 area_value
      case 9:  // repeated sint32 int32_values
      case 10: // repeated sint64 int64_values
        if (!RepeatedReader::each(r, wire, [&](uint64_t raw) {
              if (field == 6) out->area_value.push_back(static_cast<int32_t>(raw));
              if (field == 9) {
                const uint32_t u = static_cast<uint32_t>(raw);
                out->int32_values.push_back(static_cast<int32_t>((u >> 1) ^ (~(u & 1) + 1)));
              }
              if (field == 10) {
                out->int64_values.push_back(static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1)));
              }
            })) {
          return false;
        }
        break;
      case 11: // repeated float float_values
        if (wire == 5) {
          float f = 0;
          if (!r.readFloat(&f)) return false;
          out->float_values.push_back(f);
        } else if (wire == 2) {
          const uint8_t* sp = nullptr;
          size_t sn = 0;
          if (!r.readLengthDelimited(&sp, &sn)) return false;
          for (size_t i = 0; i + 4 <= sn; i += 4) {
            uint32_t bits = 0;
            for (int b = 0; b < 4; ++b) bits |= static_cast<uint32_t>(sp[i + b]) << (8 * b);
            float f = 0;
            std::memcpy(&f, &bits, 4);
            out->float_values.push_back(f);
          }
        } else if (!r.skip(wire)) {
          return false;
        }
        break;
      case 3:  // bool support
        if (wire != 0) {
          if (!r.skip(wire)) return false;
          break;
        }
        if (!r.readBool(&out->support)) return false;
        break;
      case 1:
      case 2:
      case 5:
      case 7:
        if (wire != 0) {
          if (!r.skip(wire)) return false;
          break;
        }
        {
          int32_t v = 0;
          if (!r.readInt32(&v)) return false;
          if (field == 1) out->type = v;
          if (field == 2) out->id = v;
          if (field == 5) out->area_id = v;
          if (field == 7) out->value_type = v;
        }
        break;
      default:
        if (!r.skip(wire)) return false;
        break;
    }
  }
  return true;
}

bool TouchEvent::decode(const uint8_t* p, size_t n, TouchEvent* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire != 0) {
      if (!r.skip(wire)) return false;
      continue;
    }
    int32_t v = 0;
    if (!r.readInt32(&v)) return false;
    if (field == 1) out->type = v;
    if (field == 2) out->code = v;
    if (field == 3) out->value = v;
  }
  return true;
}

bool TouchEventDevice::decode(const uint8_t* p, size_t n, TouchEventDevice* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field >= 10 && field <= 12 && wire == 2) {
      const uint8_t* sp = nullptr;
      size_t sn = 0;
      if (!r.readLengthDelimited(&sp, &sn)) return false;
      TouchEvent ev;
      if (!TouchEvent::decode(sp, sn, &ev)) return false;
      if (field == 10) out->down_events.push_back(ev);
      if (field == 11) out->up_events.push_back(ev);
      if (field == 12) out->move_events.push_back(ev);
      continue;
    }
    if (wire == 2) {
      if (field == 9) {
        if (!r.readString(&out->device)) return false;
      } else if (!r.skip(wire)) {
        return false;
      }
    } else if (wire == 0) {
      int32_t v = 0;
      if (!r.readInt32(&v)) return false;
      switch (field) {
        case 1: out->cid = v; break;
        case 2: out->eventx = v; break;
        case 3: out->screen_width = v; break;
        case 4: out->screen_height = v; break;
        case 5: out->abs_x_min = v; break;
        case 6: out->abs_x_max = v; break;
        case 7: out->abs_y_min = v; break;
        case 8: out->abs_y_max = v; break;
        default: break;
      }
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool TouchEventAllDevice::decode(const uint8_t* p, size_t n, TouchEventAllDevice* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 3 && wire == 2) {
      const uint8_t* sp = nullptr;
      size_t sn = 0;
      if (!r.readLengthDelimited(&sp, &sn)) return false;
      TouchEventDevice dev;
      if (!TouchEventDevice::decode(sp, sn, &dev)) return false;
      out->devices.push_back(std::move(dev));
    } else if (wire == 0) {
      int32_t v = 0;
      if (!r.readInt32(&v)) return false;
      if (field == 1) out->version = v;
      if (field == 2) out->cnt = v;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

// MSG_TOUCH_ACTION_3 的定长（非 protobuf）布局。
// 依据 receiver/touch/RemoteControlManager.kt:76-104：
//   action.fillByteArray(buffer, offset, 4)            → u32 大端
//   for each pointer: id(1B) x(2B BE) y(2B BE)         → 共 5 字节
std::vector<uint8_t> TouchAction3::encode() const {
  std::vector<uint8_t> out;
  out.reserve(4 + pointers.size() * kPointerBytes);
  out.push_back(static_cast<uint8_t>((action >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((action >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((action >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(action & 0xFF));
  for (const auto& pt : pointers) {
    out.push_back(static_cast<uint8_t>(pt.id & 0xFF));
    out.push_back(static_cast<uint8_t>((pt.x >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(pt.x & 0xFF));
    out.push_back(static_cast<uint8_t>((pt.y >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(pt.y & 0xFF));
  }
  return out;
}

bool TouchAction3::decode(const uint8_t* p, size_t n, TouchAction3* out) {
  if (!p || n < 4) return false;
  out->pointers.clear();
  out->action = static_cast<int32_t>(be32_get(p));
  // 剩余长度必须刚好是 5 的整数倍：否则说明对端用了别的版本，宁可整条丢弃。
  const size_t rest = n - 4;
  if (rest % kPointerBytes != 0) return false;
  for (size_t i = 4; i + kPointerBytes <= n; i += kPointerBytes) {
    Pointer pt;
    pt.id = p[i];
    pt.x = static_cast<int32_t>((static_cast<uint32_t>(p[i + 1]) << 8) | p[i + 2]);
    pt.y = static_cast<int32_t>((static_cast<uint32_t>(p[i + 3]) << 8) | p[i + 4]);
    out->pointers.push_back(pt);
  }
  return true;
}

bool TouchPadMove::decode(const uint8_t* p, size_t n, TouchPadMove* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire != 0) {
      if (!r.skip(wire)) return false;
      continue;
    }
    int32_t v = 0;
    if (!r.readInt32(&v)) return false;
    if (field == 1) out->timestamp = v;
    if (field == 2) out->delta_x = v;
    if (field == 3) out->delta_y = v;
  }
  return true;
}

bool TouchPadPinch::decode(const uint8_t* p, size_t n, TouchPadPinch* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 5) {
      if (!r.readFloat(&out->scale)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool TouchPadSimple::decode(const uint8_t* p, size_t n, TouchPadSimple* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 0) {
      if (!r.readInt32(&out->timestamp)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool TouchScroll::decode(const uint8_t* p, size_t n, TouchScroll* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire == 5) {
      float f = 0;
      if (!r.readFloat(&f)) return false;
      if (field == 5) out->distance_x = f;
      if (field == 6) out->distance_y = f;
    } else if (wire == 0) {
      int32_t v = 0;
      if (!r.readInt32(&v)) return false;
      if (field == 1) out->x1 = v;
      if (field == 2) out->y1 = v;
      if (field == 3) out->x2 = v;
      if (field == 4) out->y2 = v;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool TouchFling::decode(const uint8_t* p, size_t n, TouchFling* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire == 5) {
      float f = 0;
      if (!r.readFloat(&f)) return false;
      if (field == 5) out->velocity_x = f;
      if (field == 6) out->velocity_y = f;
    } else if (wire == 0) {
      int32_t v = 0;
      if (!r.readInt32(&v)) return false;
      if (field == 1) out->x1 = v;
      if (field == 2) out->y1 = v;
      if (field == 3) out->x2 = v;
      if (field == 4) out->y2 = v;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool VoiceControlRequest::decode(const uint8_t* p, size_t n, VoiceControlRequest* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire != 0) {
      if (!r.skip(wire)) return false;
      continue;
    }
    int32_t v = 0;
    if (!r.readInt32(&v)) return false;
    if (field == 1) out->command = v;
    if (field == 2) out->opt = v;
  }
  return true;
}

std::vector<uint8_t> BTStartPairReq::encode() const {
  PbWriter w;
  w.fieldInt32(1, ostype);
  if (!address.empty()) w.fieldString(2, address);
  return w.data();
}

bool BTIdentifyResultInd::decode(const uint8_t* p, size_t n, BTIdentifyResultInd* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 2 && wire == 2) {
      if (!r.readString(&out->address)) return false;
    } else if (wire == 0) {
      if (field == 1) {
        if (!r.readInt32(&out->status)) return false;
      } else if (!r.skip(wire)) {
        return false;
      }
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> BTPairInfo::encode() const {
  PbWriter w;
  w.fieldString(1, address);
  if (!pass_key.empty()) w.fieldString(2, pass_key);
  if (!hash.empty()) w.fieldString(3, hash);
  if (!randomizer.empty()) w.fieldString(4, randomizer);
  w.fieldString(5, uuid);
  w.fieldString(6, name);
  w.fieldInt32(7, status);
  return w.data();
}

bool BTPairInfo::decode(const uint8_t* p, size_t n, BTPairInfo* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (wire == 2) {
      std::string s;
      if (!r.readString(&s)) return false;
      switch (field) {
        case 1: out->address = s; break;
        case 2: out->pass_key = s; break;
        case 3: out->hash = s; break;
        case 4: out->randomizer = s; break;
        case 5: out->uuid = s; break;
        case 6: out->name = s; break;
        default: break;
      }
    } else if (wire == 0) {
      if (field == 7) {
        if (!r.readInt32(&out->status)) return false;
      } else if (!r.skip(wire)) {
        return false;
      }
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

bool ConnectTimeSync::decode(const uint8_t* p, size_t n, ConnectTimeSync* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 0) {
      if (!r.readInt32(&out->timestamp)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> BTAudioInfo::encode() const {
  PbWriter w;
  w.fieldInt32(1, support_bt_audio);
  return w.data();
}

// 车辆数据（HU->MD）的编码器。
// 【方向提醒】这些是【车机上送给手机】的数据（
// 参考实现 CommonParams 里 CAR_GPS/CAR_VELOCITY/… 都是 0x00018000 段 = HU 发出）。
// 所以我们实现 encode 而不是 decode。
std::vector<uint8_t> CarSpeed::encode() const {
  PbWriter w;
  w.fieldInt32(1, speed);
  if (timestamp) w.fieldUint64(2, timestamp);
  return w.data();
}

std::vector<uint8_t> GearInfo::encode() const {
  PbWriter w;
  w.fieldInt32(1, gear);
  return w.data();
}

bool GearInfo::decode(const uint8_t* p, size_t n, GearInfo* out) {
  PbReader r(p, n);
  uint32_t field = 0, wire = 0;
  while (r.next(&field, &wire)) {
    if (field == 1 && wire == 0) {
      if (!r.readInt32(&out->gear)) return false;
    } else if (!r.skip(wire)) {
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> Oil::encode() const {
  PbWriter w;
  w.fieldInt32(1, level);
  w.fieldInt32(2, range);
  w.fieldBool(3, low_fuel_warning);
  return w.data();
}

std::vector<uint8_t> Gyroscope::encode() const {
  PbWriter w;
  w.fieldInt32(1, gyro_type);
  w.fieldDouble(2, x);
  w.fieldDouble(3, y);
  w.fieldDouble(4, z);
  if (timestamp) w.fieldUint64(5, timestamp);
  return w.data();
}

std::vector<uint8_t> Acceleration::encode() const {
  PbWriter w;
  w.fieldDouble(1, x);
  w.fieldDouble(2, y);
  w.fieldDouble(3, z);
  if (timestamp) w.fieldUint64(4, timestamp);
  return w.data();
}

std::vector<uint8_t> CarGps::encode() const {
  PbWriter w;
  // 全部 25 个字段都要写：参考 proto 里它们都是 required，
  // 解析方（手机）可能严格校验缺字段。
  w.fieldUint32(1, antenna_state);
  w.fieldUint32(2, signal_quality);
  w.fieldInt32(3, latitude);
  w.fieldInt32(4, longitude);
  w.fieldInt32(5, height);
  w.fieldUint32(6, speed);
  w.fieldUint32(7, heading);
  w.fieldUint32(8, 0);   // year
  w.fieldUint32(9, 0);   // month
  w.fieldUint32(10, 0);  // day
  w.fieldUint32(11, 0);  // hrs
  w.fieldUint32(12, 0);  // min
  w.fieldUint32(13, 0);  // sec
  w.fieldUint32(14, fix);
  w.fieldUint32(15, 0);  // hdop
  w.fieldUint32(16, 0);  // pdop
  w.fieldUint32(17, 0);  // vdop
  w.fieldUint32(18, sats_used);
  w.fieldUint32(19, sats_visible);
  w.fieldUint32(20, 0);  // horPosError
  w.fieldUint32(21, 0);  // vertPosError
  w.fieldInt32(22, north_speed);
  w.fieldInt32(23, east_speed);
  w.fieldInt32(24, vert_speed);
  if (timestamp) w.fieldUint64(25, timestamp);
  return w.data();
}

std::vector<uint8_t> Contact::encode() const {
  PbWriter w;
  w.fieldInt32(1, cid);
  w.fieldString(2, name);
  w.fieldString(3, number);
  return w.data();
}

}  // namespace pb
}  // namespace carlife