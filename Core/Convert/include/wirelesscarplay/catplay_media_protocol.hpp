#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace mvp::catplay_media {
constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kMaxWirePayloadBytes = 1024U * 1024U;
constexpr std::size_t kMaxVideoConfigBytes = 64U * 1024U;
constexpr std::size_t kMaxVideoFrameBytes = 1024U * 1024U;
constexpr std::size_t kMaxAudioPayloadBytes = 3840;
// 音频扩展（多声道 / 多格式）——全部落在【既有 16 字节 AudioStart 描述符】的空闲字节里，
// 不新增消息类型、不动消息号，因此对旧生产者（只写前 4 字节）保持向后兼容。
constexpr std::size_t kMaxAudioChannels = 8;      // 参考引擎只到 1/2；这里按“如实转发”放宽（5.1=6、7.1=8）
// 采样率：从白名单（8000/16000/24000/32000/44100/48000）放宽为【范围检查】。
// 理由：多声道/杜比与高采样率靠“原样透传”支持，不能被我们自己的白名单挡在门外
//（用户硬要求：不要自己解码串改音频）。仍保留合理性边界，防垃圾数据把下游时钟算坏。
constexpr uint32_t kMinAudioRate = 8000, kMaxAudioRate = 192000;
// 压缩载荷标记（AAC-ELD / ALAC / OPUS 等）：带有此标记的 AudioChunk 不做 PCM 帧长等式校验，
// 字节原样转发 —— 本层不解码任何非 PCM 载荷，解码方只有浏览器一个。
constexpr uint16_t kFlagOpaquePayload = 0x0008;
// 压缩帧的每包帧数上界。PCM 用 p1/50（约 20ms）约束，但 AAC 一包典型是 1024 样本
// （48k 下 21.3ms > 960），拿 PCM 的约束去卡会把合法的 AAC-ELD 拒在门外。
// 这里只留一个防垃圾值的上界（8192 样本 ≈ 170ms@48k），不做能力裁剪。
constexpr uint32_t kMaxOpaqueFramesPerPacket = 8192;
constexpr std::size_t kAudioDescriptorBytes = 16; // CPMF AudioStart 的 payload 长度
// 描述符 [4..15] 全零 = 旧生产者，语义为“未声明”→ 按 PCM16 处理（见 audio_format_of）
constexpr uint8_t kAudioBitsUnspecified = 0;
constexpr std::size_t kMaxCodedDimension = 4096;
constexpr std::size_t kMaxCodedPixels = 8847360;
constexpr uint16_t kFlagKeyframe = 0x0001;
constexpr uint16_t kFlagDiscontinuity = 0x0002;
constexpr uint16_t kFlagSnapshot = 0x0004;

enum class Type : uint16_t { ServerHello=0x0001, Heartbeat=0x0002, SessionBegin=0x0003, SessionEnd=0x0004, VideoStart=0x0010, VideoConfig=0x0011, VideoFrame=0x0012, VideoEnd=0x0013, AudioStart=0x0020, AudioChunk=0x0021, AudioEnd=0x0022, AudioGain=0x0023, RequestKeyframe=0x8001, Touch=0x8002 };
struct Header { Type type{}; uint16_t flags{}; uint32_t stream_id{}; uint64_t session_id{}, sequence{}, pts{}; uint32_t payload_bytes{}, p0{}, p1{}, p2{}, p3{}; };

enum class ParseResult { Ok, NeedMore, Invalid, HandlerRejected };
using RecordHandler = bool (*)(void*, const Header&, std::span<const uint8_t>);

inline uint16_t be16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
inline void put_be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put_be32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline void put_be64(uint8_t* p, uint64_t v) { put_be32(p, uint32_t(v >> 32)); put_be32(p + 4, uint32_t(v)); }

// 音频流格式：照抄参考实现的 AudioStreamBasicDescription
// （Reference/CatPlaySource/carplay/catplay_audio/src/asbd.rs:6-15）中本管线需要的字段，
// 以及 fill_pcm(rate, valid_bits, total_bits, channels, float) 的语义（同文件 :76-90）。
// 本层【只如实转发】：不解码、不重采样、不改声道。
struct AudioFormat {
  uint8_t codec{};         // 0=未声明 1=Pcm 2=Pcm16 3=Pcm24 4=Alac 5=AacLc 6=AacEld 7=Opus
  uint8_t valid_bits{};    // 有效位 16/24/32；0=未声明
  uint8_t total_bits{};    // 容器位 16/24/32；0=未声明
  uint8_t role{};          // AudioRole：0 未知 1 媒体 2 导航 3 电话 4 提示 5 语音
  uint32_t format_flags{}; // 照抄 AudioFormatFlags：1=float 2=signed 4=packed 8=aligned_high 16=native_endian
  uint32_t bytes_per_frame{}; // 0=未声明 → channels*total_bits/8
  bool declared{false};    // 新增字节是否被生产者填过
};
// 描述符 → 格式。channels 来自 AudioStart 的 p2（字节里不重复存声道数）。
inline AudioFormat decode_audio_format(const uint8_t* d, std::size_t n, uint32_t channels) {
  AudioFormat f;
  if (n != kAudioDescriptorBytes) return f;
  for (std::size_t i = 4; i < kAudioDescriptorBytes; ++i) if (d[i]) { f.declared = true; break; }
  if (!f.declared) return f;   // 旧生产者：保持默认（PCM16），不改变既有行为
  f.codec = d[4]; f.valid_bits = d[5]; f.total_bits = d[6]; f.role = d[7];
  f.format_flags = be32(d + 8); f.bytes_per_frame = be32(d + 12);
  return f;
}
// 描述符自洽性校验。旧生产者（[4..15] 全零）直接放行。
inline bool valid_audio_format(const uint8_t* d, std::size_t n, uint32_t channels) {
  if (n != kAudioDescriptorBytes) return false;
  bool any = false;
  for (std::size_t i = 4; i < kAudioDescriptorBytes; ++i) if (d[i]) { any = true; break; }
  if (!any) return true;
  if (d[4] > 7 || d[7] > 5) return false;
  const auto ok_bits = [](uint8_t b) { return b == 0 || b == 8 || b == 16 || b == 24 || b == 32; };
  if (!ok_bits(d[5]) || !ok_bits(d[6])) return false;
  if (d[5] && d[6] && d[5] > d[6]) return false;         // 有效位不能超过容器位
  if (be32(d + 8) > 31) return false;                    // 只用参考实现的低 5 个标志位
  const uint32_t total = d[6] ? d[6] : (d[5] ? d[5] : 16);
  const uint32_t expect = channels * total / 8;
  const uint32_t declared = be32(d + 12);
  if (declared && declared != expect) return false;      // 声明了就必须与声道/位深自洽
  return true;
}
// 写描述符：前 4 字节保持既有约定（96/1/0/0），后 12 字节写本层新增字段。
inline void encode_audio_format(uint8_t* d, const AudioFormat& f) {
  d[0] = 96; d[1] = 1; d[2] = 0; d[3] = 0;
  d[4] = f.codec; d[5] = f.valid_bits; d[6] = f.total_bits; d[7] = f.role;
  put_be32(d + 8, f.format_flags); put_be32(d + 12, f.bytes_per_frame);
}
// 每样本容器字节数：未声明按 16 位（既有行为）。
inline uint32_t audio_container_bytes(const AudioFormat& f) {
  const uint32_t bits = f.total_bits ? f.total_bits : (f.valid_bits ? f.valid_bits : 16);
  return bits / 8;
}

inline bool known_type(Type t) { switch (t) { case Type::ServerHello: case Type::Heartbeat: case Type::SessionBegin: case Type::SessionEnd: case Type::VideoStart: case Type::VideoConfig: case Type::VideoFrame: case Type::VideoEnd: case Type::AudioStart: case Type::AudioChunk: case Type::AudioEnd: case Type::AudioGain: return true; default: return false; } }
inline bool valid_rate(uint32_t rate) { return rate >= kMinAudioRate && rate <= kMaxAudioRate; }
inline bool valid_dimensions(uint32_t width, uint32_t height) { return width && height && width <= kMaxCodedDimension && height <= kMaxCodedDimension && uint64_t(width)*height <= kMaxCodedPixels; }
inline bool has_annexb_nal(std::span<const uint8_t> payload, uint8_t nal_type) { for (std::size_t i=0; i+4<=payload.size(); ++i) { std::size_t prefix=0; if (payload[i]==0 && payload[i+1]==0 && payload[i+2]==1) prefix=3; else if (i+4<=payload.size() && payload[i]==0 && payload[i+1]==0 && payload[i+2]==0 && payload[i+3]==1) prefix=4; if (prefix && i+prefix<payload.size() && (payload[i+prefix]&0x1f)==nal_type) return true; } return false; }
inline bool valid_annexb(std::span<const uint8_t> payload) { for (std::size_t i=0; i+3<=payload.size(); ++i) if (payload[i]==0 && payload[i+1]==0 && ((payload[i+2]==1) || (i+3<payload.size() && payload[i+2]==0 && payload[i+3]==1))) return true; return false; }

inline bool decode_header(const uint8_t* raw, Header& h) {
  if (raw[0]!='C' || raw[1]!='P' || raw[2]!='M' || raw[3]!='F' || raw[4]!=1 || raw[5]!=0 || be16(raw+6)!=kHeaderBytes || be32(raw+60)!=0) return false;
  h.type=static_cast<Type>(be16(raw+8)); h.flags=be16(raw+10); h.stream_id=be32(raw+12); h.session_id=be64(raw+16); h.sequence=be64(raw+24); h.pts=be64(raw+32); h.payload_bytes=be32(raw+40); h.p0=be32(raw+44); h.p1=be32(raw+48); h.p2=be32(raw+52); h.p3=be32(raw+56);
  return known_type(h.type) && h.payload_bytes<=kMaxWirePayloadBytes;
}
inline bool valid_declared(const Header& h) { switch(h.type) { case Type::ServerHello: case Type::Heartbeat: case Type::SessionBegin: case Type::SessionEnd: case Type::VideoStart: case Type::VideoEnd: case Type::AudioEnd: case Type::AudioGain: return h.payload_bytes==0; case Type::VideoConfig: return h.payload_bytes <= kMaxVideoConfigBytes; case Type::VideoFrame: return h.payload_bytes>0 && h.payload_bytes<=kMaxVideoFrameBytes; case Type::AudioStart: return h.payload_bytes==16; case Type::AudioChunk:
  // Allocation guard only; valid_static and the active stream descriptor enforce
  // PCM frame alignment. Do not prematurely impose PCM16 on opaque/24-bit audio.
  return h.p3>0 && h.payload_bytes>0 && h.payload_bytes<=kMaxAudioPayloadBytes && ((h.flags&kFlagOpaquePayload)?h.p3<=kMaxOpaqueFramesPerPacket:h.p3<=h.p1/50);
default: return false; } }
inline bool valid_static(const Header& h, std::span<const uint8_t> payload) {
  if (payload.size()!=h.payload_bytes) return false;
  const uint16_t legal = h.type==Type::VideoFrame ? uint16_t(kFlagKeyframe|kFlagDiscontinuity) : h.type==Type::AudioChunk ? uint16_t(kFlagDiscontinuity|kFlagOpaquePayload) : (h.type==Type::SessionBegin || h.type==Type::VideoStart || h.type==Type::VideoConfig || h.type==Type::AudioStart || h.type==Type::AudioGain ? kFlagSnapshot : 0);
  if (h.flags & ~legal) return false;
  switch(h.type) {
    case Type::ServerHello: return h.stream_id==0 && h.session_id==0 && h.sequence==0 && h.pts==0 && h.payload_bytes==0 && h.p0==kMaxWirePayloadBytes && h.p1==1000 && h.p2==7 && h.p3==1;
    case Type::Heartbeat: return h.stream_id==0 && h.pts!=0 && h.payload_bytes==0 && h.p0==0 && h.p1==0 && h.p2==0 && h.p3==0;
    case Type::SessionBegin: return h.session_id && h.stream_id==0 && h.sequence==0 && h.pts==0 && h.payload_bytes==0 && h.p0==0 && h.p1==0 && h.p2==0 && h.p3==0;
    case Type::SessionEnd: return h.session_id && h.stream_id==0 && h.sequence==0 && h.pts==0 && h.payload_bytes==0 && h.p0<=4 && h.p1==0 && h.p2==0 && h.p3==0;
    case Type::VideoStart: return h.session_id && h.stream_id && h.sequence==0 && h.pts==0 && h.payload_bytes==0 && h.p0<=1 && h.p1==0 && h.p2==0 && h.p3==0;
    case Type::VideoConfig: if (!h.session_id || !h.stream_id || h.sequence || h.pts || !h.p3 || !valid_dimensions(h.p1,h.p2) || h.p0>2) return false; return h.p0==1 ? (h.payload_bytes>0 && h.payload_bytes<=kMaxVideoConfigBytes && valid_annexb(payload) && has_annexb_nal(payload,7) && has_annexb_nal(payload,8)) : h.payload_bytes==0;
    case Type::VideoFrame: return h.session_id && h.stream_id && h.sequence && h.pts && h.p0==1 && h.p3 && valid_dimensions(h.p1,h.p2) && h.payload_bytes>0 && h.payload_bytes<=kMaxVideoFrameBytes && valid_annexb(payload) && (!(h.flags&kFlagKeyframe) || has_annexb_nal(payload,5));
    case Type::VideoEnd: return h.session_id && h.stream_id && h.sequence==0 && h.pts==0 && h.payload_bytes==0 && h.p0<=4 && h.p1==0 && h.p2==0 && h.p3==0;
    // 多声道：p2 从 1..2 放宽到 1..kMaxAudioChannels（如实转发，不做任何声道变换）。
    case Type::AudioStart: return h.session_id && h.stream_id && h.sequence==0 && h.pts==0 && h.p0<=5 && valid_rate(h.p1) && h.p2>=1 && h.p2<=kMaxAudioChannels && h.p3==1 && h.payload_bytes==kAudioDescriptorBytes && payload[0]>=96 && payload[0]<=102 && (payload[0]==96 || payload[0]>=100) && payload[1]==1 && payload[2]<=1 && payload[3]==0 && valid_audio_format(payload.data(),payload.size(),h.p2);
    // 每帧字节数 = 声道 × 每样本字节；允许 8/16/24/32 位容器（1/2/3/4 字节）。
    // 压缩载荷（AAC-ELD/ALAC/OPUS）走 kFlagOpaquePayload 分支：不做帧长等式，只限上界、字节原样。
    case Type::AudioChunk:
      if(!(h.session_id && h.stream_id && h.sequence && h.pts && h.p0<=5 && valid_rate(h.p1) && h.p2>=1 && h.p2<=kMaxAudioChannels && h.p3>0)) return false;
      if(h.flags&kFlagOpaquePayload) return h.p3<=kMaxOpaqueFramesPerPacket && h.payload_bytes>0 && h.payload_bytes<=kMaxAudioPayloadBytes;
      return h.p3<=h.p1/50 && h.payload_bytes<=kMaxAudioPayloadBytes && (h.payload_bytes==h.p3*h.p2*1 || h.payload_bytes==h.p3*h.p2*2 || h.payload_bytes==h.p3*h.p2*3 || h.payload_bytes==h.p3*h.p2*4);
    case Type::AudioEnd: return h.session_id && h.stream_id && h.sequence==0 && h.pts==0 && h.payload_bytes==0 && h.p0<=4 && h.p1==0 && h.p2==0 && h.p3==0;
    case Type::AudioGain: return h.session_id && h.stream_id==0 && h.sequence==0 && h.pts==0 && h.payload_bytes==0 && h.p0<=60000 && h.p1<=1000000 && h.p2==0 && h.p3>0;
    default: return false;
  }
}

// Incremental state machine: a maximum-size payload and following coalesced header
// never have to coexist in one receive buffer.
class Parser {
 public:
  void reset() { header_used_=payload_used_=0; have_header_=false; }
  ParseResult push(const uint8_t* data, std::size_t size, RecordHandler handler, void* context) {
    while (size) {
      if (!have_header_) {
        const std::size_t take=std::min(size,kHeaderBytes-header_used_);
        std::memcpy(header_.data()+header_used_,data,take); header_used_+=take; data+=take; size-=take;
        if (header_used_ != kHeaderBytes) continue;
        if (!decode_header(header_.data(),current_) || !valid_declared(current_)) return ParseResult::Invalid;
        have_header_=true; payload_used_=0;
        if (!current_.payload_bytes) { if(!valid_static(current_,{}) || !handler(context,current_,{})) return ParseResult::HandlerRejected; reset(); }
        continue;
      }
      const std::size_t take=std::min(size,std::size_t(current_.payload_bytes)-payload_used_);
      std::memcpy(payload_.data()+payload_used_,data,take); payload_used_+=take; data+=take; size-=take;
      if(payload_used_ != current_.payload_bytes) continue;
      if(!valid_static(current_,{payload_.data(),payload_used_}) || !handler(context,current_,{payload_.data(),payload_used_})) return ParseResult::HandlerRejected;
      reset();
    }
    return ParseResult::NeedMore;
  }
 private:
  std::array<uint8_t,kHeaderBytes> header_{};
  std::array<uint8_t,kMaxWirePayloadBytes> payload_{};
  Header current_{};
  std::size_t header_used_{},payload_used_{};
  bool have_header_{};
};

inline std::array<uint8_t,kHeaderBytes> encode_header(const Header& h) { std::array<uint8_t,kHeaderBytes> out{};out[0]='C';out[1]='P';out[2]='M';out[3]='F';out[4]=1;put_be16(out.data()+6,kHeaderBytes);put_be16(out.data()+8,uint16_t(h.type));put_be16(out.data()+10,h.flags);put_be32(out.data()+12,h.stream_id);put_be64(out.data()+16,h.session_id);put_be64(out.data()+24,h.sequence);put_be64(out.data()+32,h.pts);put_be32(out.data()+40,h.payload_bytes);put_be32(out.data()+44,h.p0);put_be32(out.data()+48,h.p1);put_be32(out.data()+52,h.p2);put_be32(out.data()+56,h.p3);return out; }
inline std::array<uint8_t,kHeaderBytes> keyframe_request(uint64_t session, uint32_t stream) { std::array<uint8_t,kHeaderBytes> out{}; out[0]='C';out[1]='P';out[2]='M';out[3]='F';out[4]=1; out[5]=0;put_be16(out.data()+6,kHeaderBytes);put_be16(out.data()+8,uint16_t(Type::RequestKeyframe));put_be32(out.data()+12,stream);put_be64(out.data()+16,session);return out; }
inline std::array<uint8_t,kHeaderBytes> touch_request(uint64_t session,uint64_t sequence,uint32_t phase,uint32_t x,uint32_t y) { std::array<uint8_t,kHeaderBytes> out{};out[0]='C';out[1]='P';out[2]='M';out[3]='F';out[4]=1;put_be16(out.data()+6,kHeaderBytes);put_be16(out.data()+8,uint16_t(Type::Touch));put_be64(out.data()+16,session);put_be64(out.data()+24,sequence);put_be32(out.data()+44,phase);put_be32(out.data()+48,x);put_be32(out.data()+52,y);return out; }
} // namespace mvp::catplay_media
