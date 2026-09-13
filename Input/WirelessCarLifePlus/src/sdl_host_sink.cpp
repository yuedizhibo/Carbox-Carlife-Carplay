#include "carlife/sdl_host_sink.h"

#include <utility>

namespace carlife {

SdlHostSink::SdlHostSink() = default;
SdlHostSink::~SdlHostSink() = default;

bool SdlHostSink::open(int width, int height, const std::string& title, bool headless, std::string* err) {
  decoder_ = std::make_unique<H264Decoder>();
  if (!decoder_->open(err)) {
    decoder_.reset();
    return false;
  }
  std::string displayErr;
  display_ = std::make_unique<SdlSink>();
  if (!display_->init(width, height, title, headless, &displayErr)) {
    // 与重构前的 Session 行为一致：显示不可用时退化为隐藏小窗，而不是让会话失败。
    log("warn", "display unavailable (" + displayErr + "), continuing headless");
    display_ = std::make_unique<SdlSink>();
    if (!display_->init(640, 360, "CarLife+ HU", true, &displayErr)) {
      if (err) *err = displayErr;
      display_.reset();
      decoder_.reset();
      return false;
    }
    degraded_ = true;
  }
  audio_ = std::make_unique<AudioSink>();
  width_ = width;
  height_ = height;
  return true;
}

void SdlHostSink::onVideoConfig(const VideoInfo& info) {
  if (info.width > 0 && info.height > 0) {
    width_ = info.width;
    height_ = info.height;
    have_size_ = true;
  }
}

void SdlHostSink::onVideoAnnexB(const uint8_t* data, std::size_t len) {
  if (!decoder_ || !data || !len) return;
  bool got = false;
  std::string err;
  if (!decoder_->decode(data, len, &got, &err)) {
    return;  // 与重构前一致：解码错误只在帧计数很小时记录，这里由会话侧统计。
  }
  if (!got) return;
  const int w = decoder_->frameWidth();
  const int h = decoder_->frameHeight();
  if (w > 0 && h > 0 && (w != width_ || h != height_)) {
    log("video", "decoded stream is " + std::to_string(w) + "x" + std::to_string(h) +
                     " (phone-encoded size)");
    width_ = w;
    height_ = h;
    have_size_ = true;
  }
  if (!first_frame_logged_) {
    first_frame_logged_ = true;
    log("video", "first frame decoded: " + std::to_string(w) + "x" + std::to_string(h) +
                     " -> displaying");
  }
  if (display_ && !decoder_->rgba().empty()) {
    display_->present(decoder_->rgba().data(), w, h, w * 4);
  }
}

void SdlHostSink::onVideoEnded() {
  if (display_) {
    // 停流后保持最后一帧，等下一次会话语义由会话状态机决定。
  }
}

void SdlHostSink::onAudioInit(int32_t channel, int sample_rate, int channels, int sample_format) {
  if (sample_format != 2) {  // ENCODING_PCM_16BIT
    log("warn", "channel " + std::to_string(channel) + " sampleFormat=" + std::to_string(sample_format) +
                    " (非 PCM16，暂不解码，仅计数)");
    return;
  }
  std::string err;
  if (audio_ && audio_->open(sample_rate, channels, &err)) {
    log("audio", "channel " + std::to_string(channel) + " PCM " + std::to_string(sample_rate) +
                     "Hz " + std::to_string(channels) + "ch -> SDL 播放中");
  } else {
    log("warn", "audio open failed: " + err);
  }
}

void SdlHostSink::onAudioPcm(int32_t channel, const uint8_t* data, std::size_t len) {
  (void)channel;
  if (audio_ && audio_->isOpen() && data && len) audio_->writePcm(data, len);
}

void SdlHostSink::onAudioEnd(int32_t channel) {
  (void)channel;
  if (audio_) audio_->close();
}

void SdlHostSink::pump() {
  if (display_) display_->pumpEvents();
}

bool SdlHostSink::quitRequested() const { return display_ && display_->quitRequested(); }

bool SdlHostSink::takeControl(HostControl& out) {
  if (!pending_controls_.empty()) {
    out = pending_controls_.front();
    pending_controls_.pop_front();
    return true;
  }
  if (!display_) return false;
  int x = 0;
  int y = 0;
  int vw = 0;
  int vh = 0;
  if (display_->takeClick(&x, &y, &vw, &vh)) {
    if (vw > 0 && vh > 0) {
      view_w_ = vw;
      view_h_ = vh;
    }
    HostControl down;
    down.type = HostControl::Type::Touch;
    down.phase = HostControl::TouchPhase::Down;
    down.x = x;
    down.y = y;
    HostControl up = down;
    up.phase = HostControl::TouchPhase::Up;
    // 本地点击没有独立阶段，翻译成 CarLife 的 DOWN + UP（与重构前一致）。
    pending_controls_.push_back(up);
    out = down;
    return true;
  }
  int keysym = 0;
  if (display_->takeKey(&keysym)) {
    int32_t keycode = 0;
    switch (keysym) {
      case SDLK_UP: keycode = 19; break;       // KEYCODE_DPAD_UP
      case SDLK_DOWN: keycode = 20; break;     // KEYCODE_DPAD_DOWN
      case SDLK_LEFT: keycode = 21; break;
      case SDLK_RIGHT: keycode = 22; break;
      case SDLK_RETURN: keycode = 23; break;   // KEYCODE_DPAD_CENTER
      case SDLK_BACKSPACE: keycode = 4; break; // KEYCODE_BACK
      case SDLK_h: keycode = 3; break;         // KEYCODE_HOME
      default: return false;
    }
    out.type = HostControl::Type::Key;
    out.keycode = keycode;
    return true;
  }
  return false;
}

bool SdlHostSink::saveSnapshot(const std::string& path) {
  return display_ && display_->saveSnapshot(path);
}

uint64_t SdlHostSink::decodedFrames() const { return decoder_ ? decoder_->framesDecoded() : 0; }
uint64_t SdlHostSink::decodedAudioBytes() const { return audio_ ? audio_->bytesWritten() : 0; }

}  // namespace carlife
