#pragma once
#include "core/session_core.hpp"
#include "wirelesscarplay/catplay_media_ext.hpp"
#include "wirelesscarplay/catplay_media_protocol.hpp"
#include "wirelesscarplay/catplay_record_observer.hpp"
#include "wirelesscarplay/real_media_store.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>

namespace mvp {

// Single bounded AF_UNIX consumer for the independent CatPlay CPMF producer.
class CatPlayMediaClient {
 public:
  CatPlayMediaClient(SessionCore&, RealMediaStore&, std::string path={});
  ~CatPlayMediaClient();
  void start();
  void stop();
  void tick();
  void close();
  bool connected() const { return connected_.load(); }
  // 被媒体面拒掉、但已计数的记录数（例如音乐播放时的编码音频）。
  uint64_t record_dropped() const { return record_dropped_; }
  // 扩展类型里不认识的记录数（跳过但不拆会话，与 CarLife 侧同策略）。
  // 也包括"属于陈旧/空会话而被跳过"的扩展记录：同样只丢不拆会话。
  uint64_t record_skipped() const { return record_skipped_; }
  // 安装/更换/清除【可选】的原始记录观察者（契约见 catplay_record_observer.hpp）。
  // 不重放历史记录、不调用回调、也不影响当前观察者已收到的记录；传 nullptr 即关闭。
  // 观察者只收到解析器校验过的记录，包括媒体面（web 预览存储）拒收的那些。
  void set_record_observer(std::shared_ptr<CatPlayRecordObserver> observer);
  // 把符号名/数字串解析成 CarPlay 硬键标识：高 8 位 0=MediaButton、1=TelephonyButton，
  // 低 16 位是标识号（出处见 Reference/CatPlaySource/carplay/catplay_hid/src/{media_buttons,telephony}.rs）。
  static uint32_t hard_key_code(std::string_view name);
 private:
  friend struct CatPlayApiTestAccess;
  static bool on_record(void*, const catplay_media::Header&, std::span<const uint8_t>);
  bool handle(const catplay_media::Header&, std::span<const uint8_t>);
  // 处理扩展（新）类型；返回 false 表示"不认识、已跳过并计数"。
  bool handle_ext(uint16_t type, const catplay_media::Header&, std::span<const uint8_t>);
  void fail(bool protocol_error);
  void connect_if_due();
  void flush_request();
  void queue_control(const ControlEvent&);
  bool pop_control(ControlEvent&);
  void run();
  // ── 会话作用域 ────────────────────────────────────────────────────────────
  // 只认 RealMediaStore 真正接受过的会话 id（0 = 当前无会话）。扩展记录必须带这个
  // 非零 id 才允许落 Core；陈旧会话/无会话的记录一律跳过，否则上一会话的封面/歌词
  // 分片会被拼进新会话（跨会话混片）。
  bool session_live(uint64_t id) const { return id && id == session_id_; }
  // store 侧"同一个活跃会话"的判据（用于识别幂等的重复 SessionBegin）。
  bool session_live_in_store(uint64_t id) const;
  void accept_session(uint64_t id);
  void clear_session();
  // 清空全部本地扩展镜像与半份封面/歌词分片（新会话/会话结束/链路失败都走这里）。
  void reset_mirrors();
  // 活跃音频流 id 表（有界）：重复的 AudioStart 快照不重复计数，AudioEnd 只摘匹配的那条。
  static constexpr std::size_t kMaxActiveAudioStreams = 8;
  bool track_audio_stream(uint32_t id);
  bool untrack_audio_stream(uint32_t id);
  // 被媒体面/会话面跳过一条记录（计数 + 前若干条打日志，绝不拆会话）。
  void skip_record(uint16_t type, std::size_t bytes, const char* why);
  // ── 活跃源发布闸门 ────────────────────────────────────────────────────────
  // 本客户端只有在 Core 里是**当前活跃源**时才可以写状态面：否则一个没被选中的
  // catplay-real（后台仍在收记录）会把别的输入（CarLife/桌面）已发布的状态覆盖掉。
  // 所有 Core 状态写入都必须经过下面这些发布助手，不要在别处直调 set_*。
  bool publishable() const;
  void publish_media();
  void publish_display();
  void publish_input();
  void publish_audio();
  void publish_vehicle();
  void publish_navigation();
  void publish_telephony();
  void publish_link();
  void publish_artwork(uint32_t revision);
  void publish_lyrics(uint32_t revision);
  SessionCore& core_; RealMediaStore& store_; std::string path_; int fd_{-1}; bool connecting_{}, hello_{}, source_active_{};
  std::atomic<bool> running_{false}, connected_{false}; std::thread thread_;
  // 可选原始记录观察者。用 atomic<shared_ptr> 让回调期间对象始终有主（回调内即使
  // 被 set_record_observer 换掉也不会 use-after-free），且不引入额外锁。
  std::atomic<std::shared_ptr<CatPlayRecordObserver>> record_observer_{};
  catplay_media::ext::Parser parser_{};
  std::array<uint8_t,8192> read_{};
  // 上行记录最大 kHeaderBytes + kMaxCtrlText（键名/手势名/车控名/文件名），无需堆分配。
  std::array<uint8_t,catplay_media::kHeaderBytes+catplay_media::ext::kMaxCtrlText> request_{};
  std::size_t request_offset_{}, request_size_{};
  static constexpr std::size_t kControlCapacity=32;
  std::mutex control_mutex_;
  std::array<ControlEvent,kControlCapacity> controls_{};
  std::size_t control_begin_{},control_count_{};
  uint64_t control_sequence_{};
  uint64_t record_dropped_{}, record_skipped_{};
  // 已被媒体面接受的活跃会话 id（0 = 无会话）。
  uint64_t session_id_{};
  // 活跃音频流 id 表（有界，无堆）：simultaneous_streams 由它的长度推出。
  std::array<uint32_t,kMaxActiveAudioStreams> audio_streams_{};
  std::size_t audio_stream_count_{};
  // ── 会话内累计的状态镜像：增量更新后整块交给 Core（Core 侧做等值判断避免无谓重绘）──
  MediaInfo media_{}; DisplayConfig display_{}; InteractionState input_{}; AudioState audio_{};
  VehicleState vehicle_{}; TelephonyState tele_{}; LinkState link_{}; NavigationState nav_{};
  // 封面/歌词分片重组（定长，无堆）。rev 变化即视为新的一轮分片。
  std::array<char,mvp::kMaxArtwork> artwork_{}; std::size_t artwork_used_{};
  uint16_t artwork_seen_{}, artwork_total_{}; uint32_t artwork_rev_{}, artwork_bytes_{};
  std::array<char,mvp::kMaxLyrics> lyrics_{}; std::size_t lyrics_used_{};
  uint16_t lyrics_seen_{}, lyrics_total_{}; uint32_t lyrics_rev_{}, lyrics_bytes_{};
  std::chrono::steady_clock::time_point next_connect_{}, connected_at_{}, last_record_{};
};
} // namespace mvp
