#include "carlife/render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace carlife {
namespace {

bool writeBmpRgba(const std::string& path, const uint8_t* rgba, int w, int h) {
  const int rowBytes = w * 3;
  const int pad = (4 - (rowBytes % 4)) % 4;
  const int imgSize = (rowBytes + pad) * h;
  const int fileSize = 54 + imgSize;
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  auto put32 = [&](int off, uint32_t v) {
    hdr[off] = v & 0xFF; hdr[off+1] = (v>>8)&0xFF; hdr[off+2] = (v>>16)&0xFF; hdr[off+3] = (v>>24)&0xFF;
  };
  auto put16 = [&](int off, uint16_t v) { hdr[off] = v & 0xFF; hdr[off+1] = (v>>8)&0xFF; };
  put32(2, static_cast<uint32_t>(fileSize));
  put32(10, 54);
  put32(14, 40);
  put32(18, static_cast<uint32_t>(w));
  put32(22, static_cast<uint32_t>(h));
  put16(26, 1);
  put16(28, 24);
  put32(34, static_cast<uint32_t>(imgSize));
  f.write(reinterpret_cast<char*>(hdr), 54);
  std::vector<uint8_t> row(static_cast<size_t>(rowBytes + pad), 0);
  for (int y = h - 1; y >= 0; --y) {
    const uint8_t* src = rgba + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      row[static_cast<size_t>(x) * 3 + 0] = src[x * 4 + 2];  // B
      row[static_cast<size_t>(x) * 3 + 1] = src[x * 4 + 1];  // G
      row[static_cast<size_t>(x) * 3 + 2] = src[x * 4 + 0];  // R
    }
    f.write(reinterpret_cast<char*>(row.data()), rowBytes + pad);
  }
  return f.good();
}

}  // namespace

// ------------------------------------------------------------------ decoder
H264Decoder::H264Decoder() {
  frame_ = av_frame_alloc();
  pkt_ = av_packet_alloc();
}

H264Decoder::~H264Decoder() {
  if (sws_) sws_freeContext(sws_);
  av_frame_free(&frame_);
  av_packet_free(&pkt_);
  if (ctx_) avcodec_free_context(&ctx_);
}

bool H264Decoder::open(std::string* err) {
  codec_ = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec_) {
    if (err) *err = "H.264 decoder not found in libavcodec";
    return false;
  }
  ctx_ = avcodec_alloc_context3(codec_);
  if (!ctx_) {
    if (err) *err = "avcodec_alloc_context3 failed";
    return false;
  }
  ctx_->thread_count = 4;
  if (avcodec_open2(ctx_, codec_, nullptr) < 0) {
    if (err) *err = "avcodec_open2 failed";
    return false;
  }
  return true;
}

bool H264Decoder::decode(const uint8_t* data, size_t len, bool* gotFrame, std::string* err) {
  if (gotFrame) *gotFrame = false;
  if (!ctx_ || !data || !len) return false;
  bytesIn_ += len;
  pkt_->data = const_cast<uint8_t*>(data);
  pkt_->size = static_cast<int>(len);
  int rc = avcodec_send_packet(ctx_, pkt_);
  pkt_->data = nullptr;
  pkt_->size = 0;
  if (rc < 0) {
    if (err) *err = std::string("avcodec_send_packet: ") + av_err2str(rc);
    return false;
  }
  while (true) {
    rc = avcodec_receive_frame(ctx_, frame_);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
    if (rc < 0) {
      if (err) *err = std::string("avcodec_receive_frame: ") + av_err2str(rc);
      return false;
    }
    const int w = frame_->width;
    const int h = frame_->height;
    if (w <= 0 || h <= 0) continue;
    if (w != width_ || h != height_ || !sws_) {
      if (sws_) sws_freeContext(sws_);
      sws_ = sws_getContext(w, h, static_cast<AVPixelFormat>(frame_->format), w, h,
                            AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sws_) {
        if (err) *err = "sws_getContext failed";
        return false;
      }
      width_ = w;
      height_ = h;
      rgba_.assign(static_cast<size_t>(w) * h * 4, 0);
    }
    uint8_t* dst[4] = {rgba_.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {width_ * 4, 0, 0, 0};
    sws_scale(sws_, frame_->data, frame_->linesize, 0, height_, dst, dstStride);
    ++framesDecoded_;
    if (gotFrame) *gotFrame = true;
    av_frame_unref(frame_);
  }
  return true;
}

// ------------------------------------------------------------------ sdl sink
SdlSink::SdlSink() = default;
SdlSink::~SdlSink() {
  if (texture_) SDL_DestroyTexture(texture_);
  if (renderer_) SDL_DestroyRenderer(renderer_);
  if (window_) SDL_DestroyWindow(window_);
}

bool SdlSink::init(int winW, int winH, const std::string& title, bool headless, std::string* err) {
  if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      if (err) *err = std::string("SDL_Init(VIDEO): ") + SDL_GetError();
      return false;
    }
  }
  if (headless) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
  const Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | (headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
  window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW,
                             winH, flags);
  if (!window_) {
    if (err) *err = std::string("SDL_CreateWindow: ") + SDL_GetError();
    return false;
  }
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer_) {
    if (err) *err = std::string("SDL_CreateRenderer: ") + SDL_GetError();
    return false;
  }
  SDL_SetRenderDrawColor(renderer_, 8, 8, 12, 255);
  SDL_RenderClear(renderer_);
  SDL_RenderPresent(renderer_);
  return true;
}

bool SdlSink::present(const uint8_t* rgba, int w, int h, int stride) {
  if (!renderer_ || w <= 0 || h <= 0) return false;
  if (!texture_ || texW_ != w || texH_ != h) {
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!texture_) return false;
    texW_ = w;
    texH_ = h;
  }
  SDL_UpdateTexture(texture_, nullptr, rgba, stride);
  lastRgba_.assign(rgba, rgba + static_cast<size_t>(stride) * static_cast<size_t>(h));
  lastW_ = w;
  lastH_ = h;
  int rw = 0, rh = 0;
  SDL_GetRendererOutputSize(renderer_, &rw, &rh);
  // 等比居中
  double scale = std::min(static_cast<double>(rw) / w, static_cast<double>(rh) / h);
  int dw = static_cast<int>(std::lround(w * scale));
  int dh = static_cast<int>(std::lround(h * scale));
  SDL_Rect dst{(rw - dw) / 2, (rh - dh) / 2, dw, dh};
  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
  SDL_RenderPresent(renderer_);
  viewW_ = dw;
  viewH_ = dh;
  return true;
}

bool SdlSink::saveSnapshot(const std::string& path) {
  if (lastRgba_.empty()) return false;
  return writeBmpRgba(path, lastRgba_.data(), lastW_, lastH_);
}

void SdlSink::pumpEvents() {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
      case SDL_QUIT:
        quit_.store(true);
        break;
      case SDL_KEYDOWN: {
        std::lock_guard<std::mutex> lk(clickMu_);
        if (ev.key.keysym.sym == SDLK_q || ev.key.keysym.sym == SDLK_ESCAPE) quit_.store(true);
        keyQueue_.push_back(ev.key.keysym.sym);
        break;
      }
      case SDL_MOUSEBUTTONDOWN: {
        if (ev.button.button != SDL_BUTTON_LEFT) break;
        int rw = 0, rh = 0;
        SDL_GetRendererOutputSize(renderer_, &rw, &rh);
        const int vx = (rw - viewW_) / 2;
        const int vy = (rh - viewH_) / 2;
        const int dw = viewW_ > 0 ? viewW_ : 1;
        const int dh = viewH_ > 0 ? viewH_ : 1;
        int px = (ev.button.x - vx) * texW_ / dw;
        int py = (ev.button.y - vy) * texH_ / dh;
        px = std::min(std::max(px, 0), texW_ > 0 ? texW_ - 1 : 0);
        py = std::min(std::max(py, 0), texH_ > 0 ? texH_ - 1 : 0);
        std::lock_guard<std::mutex> lk(clickMu_);
        clicks_.emplace_back(px, py, texW_, texH_);
        break;
      }
      default:
        break;
    }
  }
}

bool SdlSink::takeClick(int* x, int* y, int* viewW, int* viewH) {
  std::lock_guard<std::mutex> lk(clickMu_);
  if (clicks_.empty()) return false;
  auto t = clicks_.front();
  clicks_.erase(clicks_.begin());
  *x = std::get<0>(t);
  *y = std::get<1>(t);
  *viewW = std::get<2>(t);
  *viewH = std::get<3>(t);
  return true;
}

bool SdlSink::takeKey(int* keysym) {
  std::lock_guard<std::mutex> lk(clickMu_);
  if (keyQueue_.empty()) return false;
  *keysym = keyQueue_.front();
  keyQueue_.erase(keyQueue_.begin());
  return true;
}

// ------------------------------------------------------------------ audio
AudioSink::AudioSink() = default;
AudioSink::~AudioSink() { close(); }

void AudioSink::audioCallback(void* userdata, uint8_t* stream, int len) {
  auto* self = static_cast<AudioSink*>(userdata);
  std::lock_guard<std::mutex> lk(self->mu_);
  size_t avail = self->ring_.size() - self->readPos_;
  size_t n = std::min(avail, static_cast<size_t>(len));
  if (n) {
    std::memcpy(stream, self->ring_.data() + self->readPos_, n);
    self->readPos_ += n;
  }
  if (n < static_cast<size_t>(len)) {
    std::memset(stream + n, 0, static_cast<size_t>(len) - n);
  }
  if (self->readPos_ == self->ring_.size()) {  // 回收
    self->ring_.clear();
    self->readPos_ = 0;
  }
}

bool AudioSink::open(int sampleRate, int channels, std::string* err) {
  close();
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
      if (err) *err = std::string("SDL_Init(AUDIO): ") + SDL_GetError();
      return false;
    }
  }
  SDL_AudioSpec want{};
  want.freq = sampleRate > 0 ? sampleRate : 44100;
  want.format = AUDIO_S16SYS;
  want.channels = static_cast<uint8_t>(channels > 0 ? channels : 2);
  want.samples = 2048;
  want.callback = &AudioSink::audioCallback;
  want.userdata = this;
  SDL_AudioSpec have{};
  dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!dev_) {
    if (err) *err = std::string("SDL_OpenAudioDevice: ") + SDL_GetError();
    return false;
  }
  SDL_PauseAudioDevice(dev_, 0);
  opened_ = true;
  return true;
}

void AudioSink::writePcm(const uint8_t* data, size_t len) {
  if (!opened_) return;
  std::lock_guard<std::mutex> lk(mu_);
  if (ring_.size() - readPos_ > 40 * 1024 * 1024) {  // 背压：丢最旧
    size_t drop = (ring_.size() - readPos_) / 2;
    ring_.erase(ring_.begin(), ring_.begin() + static_cast<long>(drop));
    readPos_ = 0;
  }
  ring_.insert(ring_.end(), data, data + len);
  bytesWritten_ += len;
}

void AudioSink::close() {
  if (dev_) {
    SDL_CloseAudioDevice(dev_);
    dev_ = 0;
  }
  opened_ = false;
}

}  // namespace carlife