#pragma once
// 元数据 / 专辑封面 / 歌词的转发缝（Core/Convert 侧）。
//
// 为什么要这一层：`MediaInfo` 正文很小，但封面（kMaxArtwork=128 KiB）与歌词（kMaxLyrics=4 KiB）
// 体积大且**只在变化时才需要过链路**。所以本层做两件事：
//   1) 定长缓冲保存当前正文（无堆分配，与 Core 其余部分同构）；
//   2) 通过【可注册回调】把"有变化"这件事连同正文交给下游。
//
// 本层**不自行发明协议号**：CPMF 侧的消息号由输出侧定义
// （Input/WirelessCarPlay/include/wirelesscarplay/catplay_media_ext.hpp，
//  对应 iAP2 NowPlayingUpdate 0x5000–0x5003 / MediaLibraryUpdate 0x4C00–0x4C09，
//  见 Temp/CarPlay-CarLife-CAPABILITY-AUDIT.md 2.6 与附录 A）。
// 这里只负责"什么时候该发、发什么字节"。
//
// 线程约定：内部自持锁；回调在**不持锁**时调用，因此回调里可以再调本类的方法，
// 但不要反过来去碰 RealMediaStore 的锁（避免锁序反转）。
#include "core/session_core.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string_view>

namespace mvp {

// 下游（输出侧编码器 / 页面推送）需要实现的三件事。context 由注册方自持。
struct MediaRelaySink {
  void* context{};
  // 返回 false = 下游此刻不能接收（链路断开等）。本层不回滚也不重发：
  // 正文一直留在定长缓冲里，订阅者恢复后用 revision 判断是否需要补齐。
  bool (*on_media_info)(void*, const MediaInfo&){};
  bool (*on_artwork)(void*, uint32_t revision, const char* data, std::size_t size){};
  bool (*on_lyrics)(void*, uint32_t revision, const char* data, std::size_t size){};
};

class MediaRelay {
 public:
  void set_sink(const MediaRelaySink& sink) {
    std::lock_guard lock(mutex_);
    sink_ = sink;
  }
  void clear_sink() {
    std::lock_guard lock(mutex_);
    sink_ = MediaRelaySink{};
  }
  bool has_sink() const {
    std::lock_guard lock(mutex_);
    return sink_.on_media_info || sink_.on_artwork || sink_.on_lyrics;
  }

  // 元数据：与上一次完全相同则返回 false（页面/输出侧无需重发）。
  bool publish_media_info(const MediaInfo& info) {
    MediaRelaySink sink;
    uint64_t generation = 0;
    {
      std::lock_guard lock(mutex_);
      if (same_info(info_, info)) return false;
      info_ = info;
      generation = ++info_generation_;
      sink = sink_;
    }
    if (sink.on_media_info) sink.on_media_info(sink.context, info_);
    (void)generation;
    return true;
  }

  // 封面正文。revision 由生产者自选（可以是序号或指纹），本层只做去重与转发，
  // 不按大小比较先后（避免生产者用 32 位回绕序号时被误判为过期）。
  // size==0 表示"清除封面"。
  bool publish_artwork(uint32_t revision, const char* data, std::size_t size) {
    if (size > kMaxArtwork) return false;
    if (size && !data) return false;
    MediaRelaySink sink;
    uint32_t rev = 0;
    {
      std::lock_guard lock(mutex_);
      if (!size) {
        if (!artwork_size_ && !artwork_rev_) return false;
        artwork_size_ = 0;
        artwork_rev_ = revision;
        rev = artwork_rev_;
        sink = sink_;
      } else {
        if (revision == artwork_rev_ && size == artwork_size_ &&
            std::memcmp(artwork_.data(), data, size) == 0)
          return false;
        std::memcpy(artwork_.data(), data, size);
        artwork_size_ = size;
        artwork_rev_ = revision;
        rev = artwork_rev_;
        sink = sink_;
      }
    }
    if (sink.on_artwork) sink.on_artwork(sink.context, rev, artwork_.data(), artwork_size_);
    return true;
  }

  // 歌词正文（LRC 原文）。超长直接拒绝而不是截断：截断成半个 LRC 行会显示成一堆残缺时间戳。
  bool publish_lyrics(uint32_t revision, std::string_view lrc) {
    if (lrc.size() > kMaxLyrics) return false;
    MediaRelaySink sink;
    uint32_t rev = 0;
    {
      std::lock_guard lock(mutex_);
      if (!lrc.empty() && revision == lyrics_rev_ && lrc.size() == lyrics_size_ &&
          std::memcmp(lyrics_.data(), lrc.data(), lrc.size()) == 0)
        return false;
      if (lrc.empty() && !lyrics_size_) return false;
      if (!lrc.empty()) std::memcpy(lyrics_.data(), lrc.data(), lrc.size());
      lyrics_size_ = lrc.size();
      lyrics_rev_ = revision;
      rev = lyrics_rev_;
      sink = sink_;
    }
    if (sink.on_lyrics) sink.on_lyrics(sink.context, rev, lyrics_.data(), lyrics_size_);
    return true;
  }

  // 迟到订阅者：新会话/新页面接入时按 revision 判断要不要补齐。
  MediaInfo media_info() const {
    std::lock_guard lock(mutex_);
    return info_;
  }
  bool artwork(const char** data, std::size_t* size, uint32_t* revision) const {
    std::lock_guard lock(mutex_);
    if (data) *data = artwork_.data();
    if (size) *size = artwork_size_;
    if (revision) *revision = artwork_rev_;
    return artwork_size_ != 0;
  }
  bool lyrics(std::string_view* out, uint32_t* revision) const {
    std::lock_guard lock(mutex_);
    if (out) *out = std::string_view(lyrics_.data(), lyrics_size_);
    if (revision) *revision = lyrics_rev_;
    return lyrics_size_ != 0;
  }
  uint64_t media_info_generation() const {
    std::lock_guard lock(mutex_);
    return info_generation_;
  }
  std::size_t artwork_bytes() const {
    std::lock_guard lock(mutex_);
    return artwork_size_;
  }
  std::size_t lyrics_bytes() const {
    std::lock_guard lock(mutex_);
    return lyrics_size_;
  }
  uint32_t artwork_revision() const {
    std::lock_guard lock(mutex_);
    return artwork_rev_;
  }
  uint32_t lyrics_revision() const {
    std::lock_guard lock(mutex_);
    return lyrics_rev_;
  }
  // 会话边界调用：正文与去重状态一起归零，避免上一会话的封面泄漏到新会话。
  void reset() {
    std::lock_guard lock(mutex_);
    info_ = MediaInfo{};
    info_generation_ = 0;
    artwork_size_ = 0;
    artwork_rev_ = 0;
    lyrics_size_ = 0;
    lyrics_rev_ = 0;
  }

 private:
  template <std::size_t N>
  static bool eq(const std::array<char, N>& a, const std::array<char, N>& b) {
    return std::memcmp(a.data(), b.data(), N) == 0;
  }
  // 逐字段比较。不用 memcmp 整块比：MediaInfo 有填充字节，整块比较判不相等是假阴性。
  static bool same_info(const MediaInfo& a, const MediaInfo& b) {
    return a.valid == b.valid && eq(a.title, b.title) && eq(a.artist, b.artist) &&
           eq(a.album, b.album) && eq(a.album_artist, b.album_artist) && eq(a.app, b.app) &&
           eq(a.genre, b.genre) && a.track_number == b.track_number &&
           a.track_count == b.track_count && a.duration_ms == b.duration_ms &&
           a.position_ms == b.position_ms && a.playing == b.playing &&
           a.repeat_mode == b.repeat_mode && a.shuffle == b.shuffle &&
           a.artwork_revision == b.artwork_revision && a.artwork_bytes == b.artwork_bytes &&
           a.has_artwork == b.has_artwork && a.lyrics_revision == b.lyrics_revision &&
           a.lyrics_bytes == b.lyrics_bytes && a.has_lyrics == b.has_lyrics &&
           a.media_library_revision == b.media_library_revision;
  }

  mutable std::mutex mutex_;
  MediaRelaySink sink_{};
  MediaInfo info_{};
  uint64_t info_generation_{};
  std::array<char, kMaxArtwork> artwork_{};
  std::size_t artwork_size_{};
  uint32_t artwork_rev_{};
  std::array<char, kMaxLyrics> lyrics_{};
  std::size_t lyrics_size_{};
  uint32_t lyrics_rev_{};
};

}  // namespace mvp
