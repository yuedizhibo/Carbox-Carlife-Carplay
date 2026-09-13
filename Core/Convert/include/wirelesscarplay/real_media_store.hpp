#pragma once
#include "wirelesscarplay/catplay_media_protocol.hpp"
#include "wirelesscarplay/media_relay.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
// 必须自包含：payload 用的是 std::vector<uint8_t>。
// 之前依赖其他 TU 间接引入 <vector>，本地全量构建能过、
// 而板上单 TU 编译（第一行就包含本头文件）会直接报
// “'vector' in namespace 'std' does not name a template type”，
// 进而连锁成“VideoSlot has no member named payload” —— 部署因此静默失败。
#include <vector>
#include <string>

namespace mvp {
struct RealMediaSnapshot {
  enum class Ipc { Unavailable, Connected, ProtocolError } ipc{Ipc::Unavailable};
  enum class Session { Inactive, Active } session{Session::Inactive};
  enum class Video { Inactive, WaitingConfig, WaitingKeyframe, Active, Unsupported, Failed };
  struct Screen {
    uint32_t role{}, stream_id{}, config_bytes{}, queued_frames{};
    // 已投递给客户端的帧数。轮询两次即可算出【服务端投递帧率】，
    // 用来区分掉帧发生在服务端/引擎侧还是浏览器侧。
    uint64_t frames_sent{};
    // ring 溢出而丢掉的帧数（画面切换码率爆发时的主要丢帧点）。
    uint64_t frames_dropped{};
    uint64_t config_generation{};
    bool selected{};
    Video state{Video::Inactive};
    // 帧率：如实上报（实测）与请求（目标）。这不是降载手段，只是把两边的数字对上。
    // 请求最终由适配器翻译成输入侧的原生消息（CarLife: MSG_CMD_VIDEO_ENCODER_FRAME_RATE_CHANGE 0x0001800C）。
    uint64_t frames_received{};
    uint32_t target_fps{}, actual_fps{};
  };
  struct Audio {
    uint32_t stream_id{}, audio_type{};
    uint64_t generation{};
    enum class State { StartedNoData, Active, Discontinuous } state{State::StartedNoData};
    // ↓ 多声道/多格式：只如实转发，不做重采样/声道变换（AUDIT 6.1）。
    // codec/role 用的就是 wire 上的 0..7 / 0..5，页面据此选播放器。
    uint32_t sample_rate{}, channels{}, bits{}, bytes_per_frame{};
    uint32_t codec{}, role{};
    // 1 = 本流载荷是压缩数据（AAC-ELD/ALAC/OPUS），本层一个字节都没动。
    uint8_t opaque{};
  };
  std::array<Screen,2> screens{{Screen{0},Screen{1}}};
  // 必须与 RealMediaStore::kAudioStreams 保持一致：写入端按打开流数递增 audio_count，
  // 数组比它小就是越界写（内存破坏 → 崩溃）。历史上这里是硬编码 2，后来是 4。
  // 现为 6：足以让 Media/Navigation/Telephony/Alert/Voice 五个 role 各占一条独立 ring。
  std::array<Audio,6> audio{};
  std::size_t audio_count{};
  // 音频仲裁/闪避状态。语义照抄参考实现 AudioTrackManagerDualNormal.java:376-395：
  // 导航播报时把【媒体轨】按比例压低（setVolume(maxVolume / mMusicAudioTrackVolumReduceRatio)），
  // 导航轨本身不动；ratio=3 → 增益 1/3 = 333333ppm。
  struct Ducking {
    bool active{};
    uint32_t ratio_ppm{333333};
    uint32_t transition_ms{300};
    int32_t media_ppm{1000000};
    int32_t nav_ppm{1000000};
    uint64_t generation{};
  } ducking{};
  // 元数据/封面/歌词的当前版本号与大小（正文在 MediaRelay 的定长缓冲里）。
  // 页面/输出侧按 revision 变化再取正文，避免每帧拷 128 KiB 封面。
  struct Media {
    bool valid{}, has_artwork{}, has_lyrics{};
    uint64_t info_generation{};
    uint32_t artwork_revision{}, lyrics_revision{};
    uint32_t artwork_bytes{}, lyrics_bytes{};
  } media{};
  uint64_t video_dropped{}, audio_dropped{}, protocol_dropped{};
  uint64_t session_id{};
  // Compatibility view of role 0 for existing API consumers.
  Video video{Video::Inactive};
  uint64_t video_config_generation{};
  uint32_t video_stream{};
};

// Shared ingest cache. All payload storage is fixed-capacity and one cache is shared by every consumer.
class RealMediaStore {
 public:
  static constexpr std::size_t kVideoRoles=2, kVideoFrames=48, kAudioStreams=6, kAudioChunks=128;
  // 帧率请求的发送节流（与 take_keyframe_request 的 500ms 节流同构：
  // 避免目标帧率变化被高频轮询时反复下发命令）。
  static constexpr uint32_t kRateRequestIntervalMs=500;
  void transport_connected();
  void transport_closed(bool protocol_error=false);
  bool ingest(const catplay_media::Header&, std::span<const uint8_t>);
  // Direct (non-CPMF) ingestion for inputs that are not the CatPlay producer.
  // These write the same bounded structures as ingest(), so one Web media path
  // serves every input. Currently used by the Wireless CarLife+ adapter.
  bool transport_ready();
  bool begin_session(uint64_t session_id);
  bool end_session(uint64_t session_id);
  bool set_video_stream(uint32_t role, uint32_t stream_id, uint32_t width, uint32_t height, std::span<const uint8_t> sps_pps);
  bool push_video_frame(uint32_t role, uint64_t sequence, uint64_t pts, bool keyframe, std::span<const uint8_t> annexb);
  bool end_video_stream(uint32_t role);
  bool start_audio(uint32_t stream_id, uint32_t audio_type, uint32_t sample_rate, uint32_t channels);
  // 带格式的直灌：多声道/多格式走这里。**不会**改变样本，只是把格式如实带下去。
  bool start_audio(uint32_t stream_id, uint32_t audio_type, uint32_t sample_rate, uint32_t channels,
                   const catplay_media::AudioFormat& format);
  // 流已开好后再声明/更新格式（适配器从原生消息里拿到格式时用）。
  bool set_audio_format(uint32_t stream_id, const catplay_media::AudioFormat& format);
  // 显式指定 role（AudioRole 0..5）；不指定时由 audio_type 推导。
  bool set_audio_role(uint32_t stream_id, uint32_t role);
  bool push_audio(uint32_t stream_id, uint64_t sequence, uint64_t pts, std::span<const uint8_t> pcm);
  // 压缩载荷（AAC-ELD/ALAC/OPUS）直灌：不做 PCM 帧长等式，字节原样入队与转发。
  // frames 由编码器声明（AAC 典型 1024）：本层不解码，所以不自行推算。
  bool push_audio_opaque(uint32_t stream_id, uint64_t sequence, uint64_t pts, uint32_t frames,
                         std::span<const uint8_t> payload);
  bool end_audio(uint32_t stream_id);
  bool set_audio_gain(uint32_t duration_ms, uint32_t gain_ppm);
  // 导航压媒体（ducking）：导航播报时把媒体轨压到 ratio_ppm，结束回 1000000ppm。
  // 与 set_audio_gain 共用同一个增益通道（CPMF AudioGain），因此客户端只有一套渐变逻辑。
  bool set_ducking(bool nav_active);
  bool set_duck_config(uint32_t ratio_ppm, uint32_t transition_ms);
  bool ducking_active() const;
  // 帧率：请求（目标）与实测。不用作降载/降分辨率手段。
  bool set_target_fps(uint32_t role, uint32_t fps);
  // 取出待下发的帧率变更请求，由适配器翻译成输入侧原生消息。≥kRateRequestIntervalMs 节流。
  bool take_rate_request(uint32_t& role, uint32_t& fps);
  // ── 元数据 / 封面 / 歌词（正文存定长缓冲，通过可注册回调转发）─────────
  // 本层不定义协议号：CPMF 消息号由输出侧（catplay_media_ext.hpp）决定。
  MediaRelay& media_relay() { return relay_; }
  const MediaRelay& media_relay() const { return relay_; }
  bool take_keyframe_request(uint64_t& session_id, uint32_t& stream_id);
  bool request_keyframe(uint32_t stream_id);
  void set_selected(bool selected);
  bool video_packet(uint32_t role, uint64_t after_sequence, std::string& out) const;
  bool video_packet(uint64_t after_sequence, std::string& out) const { return video_packet(0,after_sequence,out); }
  struct AudioCursor { std::array<uint32_t,kAudioStreams> stream{}; std::array<uint64_t,kAudioStreams> sequence{}; uint64_t gain_generation{}; };
  bool audio_packet(AudioCursor& cursor, std::string& out) const;
  bool audio_packet(std::string& out) const;
  RealMediaSnapshot snapshot() const;
 private:
  struct VideoSlot {
    uint64_t sequence{}, pts{};
    uint32_t config_id{}, width{}, height{}, size{};
    bool keyframe{};
    // Lazily allocated on the heap: fixed 1 MiB cap without placing the
    // two-screen 8 MiB frame pool on a thread stack.
    // 按实际帧大小分配（不再固定 1 MiB/槽）：ring 才能开大以吸收画面切换时的码率爆发，
    // 而内存只与真实内容成正比（典型 2–60 KB/帧，48 帧两路约 1–2 MB）。
    std::vector<uint8_t> payload;
  };
  struct VideoStream {
    bool started{}, have_config{}, waiting_keyframe{true}, have_sequence{}, keyframe_requested{};
    RealMediaSnapshot::Video state{RealMediaSnapshot::Video::Inactive};
    uint32_t role{}, id{}, config_id{}, width{}, height{};
    uint64_t config_generation{}, last_sequence{};
    std::array<uint8_t,catplay_media::kMaxVideoConfigBytes> config{};
    std::size_t config_size{};
    std::array<VideoSlot,kVideoFrames> frames{};
    std::size_t begin{}, count{};
    std::chrono::steady_clock::time_point last_keyframe_sent{};
    // 实测帧率：1 秒滑动窗内的收帧计数（只统计真正入 ring 的帧）。
    uint64_t frames_received{}, frames_in_window{};
    std::chrono::steady_clock::time_point fps_window_start{};
    uint32_t actual_fps{};
  };
  struct AudioSlot {
    uint64_t sequence{}, pts{};
    uint32_t frames{}, size{};
    bool discontinuity{};
    bool opaque{};
    std::array<uint8_t,catplay_media::kMaxAudioPayloadBytes> payload{};
  };
  struct AudioStream {
    bool open{}, has_sequence{};
    uint32_t id{}, type{}, rate{}, channels{};
    uint64_t generation{};
    std::array<uint8_t,16> descriptor{};
    uint64_t last_sequence{};
    RealMediaSnapshot::Audio::State state{RealMediaSnapshot::Audio::State::StartedNoData};
    // 每 role 一条独立 ring（AUDIT 6.1：多声道/多格式只如实转发，不重采样、不改声道）。
    uint32_t role{};
    catplay_media::AudioFormat format{};
    std::array<AudioSlot,kAudioChunks> chunks{};
    std::size_t begin{}, count{};
  };
  // 把 AudioStream 的格式/role 如实搬到快照里（页面按 role 选播放器）。
  uint32_t effective_bytes_per_frame(const AudioStream& a) const;
  // only_stream_id==0 （默认）表示重建全部打开流。
  void publish_audio_locked(uint32_t only_stream_id = 0);
  void apply_gain_locked(uint32_t duration_ms, int32_t ppm);
  void update_fps_locked(VideoStream& v);
  void clear_media_locked();
  bool push_video_frame_locked(VideoStream& v, uint64_t sequence, uint64_t pts, bool keyframe, bool discontinuity, std::span<const uint8_t> annexb);
  bool push_audio_locked(AudioStream& a, uint64_t sequence, uint64_t pts, bool discontinuity, std::span<const uint8_t> pcm);
  // 唯一入队实现：PCM 走帧长等式，opaque 走“字节原样 + 声明帧数”。
  bool push_audio_payload_locked(AudioStream& a, uint64_t sequence, uint64_t pts, bool discontinuity,
                                 bool opaque, uint32_t frames, std::span<const uint8_t> payload);
  void sync_screen_locked(uint32_t role);
  void request_keyframe_locked(VideoStream& video);
  VideoStream* video_id_locked(uint32_t id);
  AudioStream* audio_locked(uint32_t id);
  mutable std::mutex mutex_;
  RealMediaSnapshot snapshot_{};
  bool hello_{}, selected_{true};
  uint64_t last_heartbeat_pts{};
  std::array<VideoStream,kVideoRoles> video_{};
  std::array<AudioStream,kAudioStreams> audio_{};
  uint64_t next_audio_generation_{}, audio_gain_generation_{};
  uint32_t audio_gain_ppm_{1000000}, audio_gain_duration_ms_{};
  // 闪避配置与当前增益（媒体轨/导航轨分开记，导航轨不压低）。
  bool duck_active_{};
  uint32_t duck_ratio_ppm_{333333}, duck_transition_ms_{300};
  int32_t media_ppm_{1000000}, nav_ppm_{1000000};
  // 帧率：目标值与待下发的请求（每 role 一份）。
  std::array<uint32_t,kVideoRoles> target_fps_{};
  std::array<uint32_t,kVideoRoles> rate_request_fps_{};
  std::array<bool,kVideoRoles> rate_requested_{};
  std::array<std::chrono::steady_clock::time_point,kVideoRoles> last_rate_request_{};
  // 元数据/封面/歌词：独立锁 + 两块定长缓冲（kMaxArtwork + kMaxLyrics ≈ 132 KiB）。
  // 锁序恒为 mutex_ → relay_(内部锁)；回调不持锁调用，因此回调里可以安全地取 snapshot()。
  MediaRelay relay_{};
};
} // namespace mvp
