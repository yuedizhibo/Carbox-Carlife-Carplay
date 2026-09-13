#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace mvp {
struct ScreenOffer {
  bool enabled{true};
  uint32_t width{1920};
  uint32_t height{1080};
  uint32_t fps{60};
};

struct ScreenOutputConfig {
  static constexpr uint64_t kMaxTotalPixelRate = 250'000'000;
  ScreenOffer main{};
  ScreenOffer alt{false, 1280, 720, 30};
};

bool valid_display_config(const ScreenOutputConfig& config, std::string* error = nullptr);
bool load_display_config(const std::string& path, ScreenOutputConfig& config, std::string* error = nullptr);
bool save_display_config(const std::string& path, const ScreenOutputConfig& config, std::string* error = nullptr);
std::string display_config_json(const ScreenOutputConfig& config, std::string_view applies = "next-session");
} // namespace mvp
