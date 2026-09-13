// 视频解码 + 显示 + 音频输出 + 快照
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <SDL2/SDL.h>

#include "carlife/session_state.h"

namespace carlife {

// H.264 Annex-B -> RGBA (libavcodec + swscale)
class H264Decoder {
 public:
  H264Decoder();
  ~H264Decoder();
  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

  bool open(std::string* err);
  bool decode(const uint8_t* data, size_t len, bool* gotFrame, std::string* err);
  int frameWidth() const { return width_; }
  int frameHeight() const { return height_; }
  uint64_t framesDecoded() const { return framesDecoded_; }
  uint64_t bytesIn() const { return bytesIn_; }
  const std::vector<uint8_t>& rgba() const { return rgba_; }

 private:
  AVCodecContext* ctx_ = nullptr;
  const AVCodec* codec_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  SwsContext* sws_ = nullptr;
  std::vector<uint8_t> rgba_;
  int width_ = 0;
  int height_ = 0;
  uint64_t framesDecoded_ = 0;
  uint64_t bytesIn_ = 0;
};

class SdlSink {
 public:
  SdlSink();
  ~SdlSink();
  bool init(int winW, int winH, const std::string& title, bool headless, std::string* err);
  bool present(const uint8_t* rgba, int w, int h, int stride);
  bool saveSnapshot(const std::string& path);
  void pumpEvents();
  bool quitRequested() const { return quit_.load(); }
  bool takeClick(int* x, int* y, int* viewW, int* viewH);
  bool takeKey(int* keysym);
  SDL_Window* window() const { return window_; }

 private:
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  int texW_ = 0;
  int texH_ = 0;
  std::vector<uint8_t> lastRgba_;
  int lastW_ = 0;
  int lastH_ = 0;
  int viewW_ = 0;
  int viewH_ = 0;
  std::atomic<bool> quit_{false};
  std::mutex clickMu_;
  std::vector<std::tuple<int, int, int, int>> clicks_;
  std::vector<int> keyQueue_;
};

// Media 通道：PCM(16bit) 直接播放；非 PCM 只计数
class AudioSink {
 public:
  AudioSink();
  ~AudioSink();
  bool open(int sampleRate, int channels, std::string* err);
  void writePcm(const uint8_t* data, size_t len);
  uint64_t bytesWritten() const { return bytesWritten_; }
  bool isOpen() const { return opened_; }
  void close();

 private:
  static void audioCallback(void* userdata, uint8_t* stream, int len);
  SDL_AudioDeviceID dev_ = 0;
  std::vector<uint8_t> ring_;
  std::mutex mu_;
  size_t readPos_ = 0;
  uint64_t bytesWritten_ = 0;
  bool opened_ = false;
};

}  // namespace carlife