// 本地宿主机实现：解码 + SDL 开窗 + SDL 音频 + 快照。
//
// 这是重构前 Session 自带的显示职责，整体搬到宿主机侧，行为保持不变，
// 只服务独立工具 carlife-hu。Core 接入不走这里（见 carlife_input.h）。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "carlife/host_sink.h"
#include "carlife/render.h"

namespace carlife {

class SdlHostSink final : public HostSink {
 public:
  // 日志钩子：让本类沿用与 Session 相同的输出格式（run_wsl_tests.sh 依赖这些行）。
  using Logger = std::function<void(const char*, const std::string&)>;

  SdlHostSink();
  ~SdlHostSink() override;

  void setLogger(Logger logger) { logger_ = std::move(logger); }

  // 打开解码器与窗口；headless 时窗口隐藏但仍可快照。
  bool open(int width, int height, const std::string& title, bool headless, std::string* err);
  bool ready() const { return decoder_ != nullptr; }
  // 首次开窗失败并已退化为隐藏窗口时为真。
  bool degraded() const { return degraded_; }

  void onVideoConfig(const VideoInfo& info) override;
  void onVideoAnnexB(const uint8_t* data, std::size_t len) override;
  void onVideoEnded() override;
  void onAudioInit(int32_t channel, int sample_rate, int channels, int sample_format) override;
  void onAudioPcm(int32_t channel, const uint8_t* data, std::size_t len) override;
  void onAudioEnd(int32_t channel) override;

  void pump() override;
  bool quitRequested() const override;
  bool takeControl(HostControl& out) override;
  bool saveSnapshot(const std::string& path) override;
  uint64_t decodedFrames() const override;
  uint64_t decodedAudioBytes() const override;

 private:
  void log(const char* level, const std::string& msg) const {
    if (logger_) logger_(level, msg);
  }

  std::unique_ptr<H264Decoder> decoder_;
  std::unique_ptr<SdlSink> display_;
  std::unique_ptr<AudioSink> audio_;
  std::deque<HostControl> pending_controls_;
  Logger logger_;
  int width_ = 0;
  int height_ = 0;
  int view_w_ = 0;
  int view_h_ = 0;
  bool have_size_ = false;
  bool first_frame_logged_ = false;
  bool degraded_ = false;
};

}  // namespace carlife
