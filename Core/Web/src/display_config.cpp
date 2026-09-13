#include "wirelesscarplay/display_config.hpp"
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mvp { namespace {
constexpr std::size_t kMaxConfigBytes = 512;
void fail(std::string* error, std::string value) { if (error) *error = std::move(value); }
bool number(std::string_view text, uint32_t& out) {
  if (text.empty()) return false;
  uint32_t value{};
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) return false;
  out = value;
  return true;
}
bool valid_screen(const ScreenOffer& screen, const char* name, std::string* error) {
  if ((screen.width & 1U) || (screen.height & 1U) || screen.width < 320 || screen.width > 2560 ||
      screen.height < 240 || screen.height > 1440 || screen.fps < 1 || screen.fps > 60) {
    fail(error, std::string(name) + " must use even 320..2560 x 240..1440 dimensions and 1..60 fps");
    return false;
  }
  return true;
}
std::string serialize(const ScreenOutputConfig& c) {
  std::ostringstream out;
  out << "version=1\n"
      << "main_width=" << c.main.width << '\n'
      << "main_height=" << c.main.height << '\n'
      << "main_fps=" << c.main.fps << '\n'
      << "alt_enabled=" << (c.alt.enabled ? 1 : 0) << '\n'
      << "alt_width=" << c.alt.width << '\n'
      << "alt_height=" << c.alt.height << '\n'
      << "alt_fps=" << c.alt.fps << '\n';
  return out.str();
}
}

bool valid_display_config(const ScreenOutputConfig& config, std::string* error) {
  if (!config.main.enabled) { fail(error, "main display cannot be disabled"); return false; }
  if (!valid_screen(config.main, "main", error) || !valid_screen(config.alt, "alt", error)) return false;
  uint64_t rate = uint64_t(config.main.width) * config.main.height * config.main.fps;
  if (config.alt.enabled) rate += uint64_t(config.alt.width) * config.alt.height * config.alt.fps;
  if (rate > ScreenOutputConfig::kMaxTotalPixelRate) {
    fail(error, "combined display pixel rate exceeds 250000000 pixels/s");
    return false;
  }
  return true;
}

bool load_display_config(const std::string& path, ScreenOutputConfig& config, std::string* error) {
  config = ScreenOutputConfig{};
  if (path.empty()) return true;
  errno = 0;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (errno == ENOENT) return true;
    fail(error, "cannot open display config");
    return false;
  }
  std::string text(kMaxConfigBytes + 1, '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(input.gcount()));
  if (text.size() > kMaxConfigBytes) { fail(error, "display config is too large"); return false; }

  enum Key : unsigned { Version, MainWidth, MainHeight, MainFps, AltEnabled, AltWidth, AltHeight, AltFps, Count };
  std::array<bool, Count> seen{};
  ScreenOutputConfig parsed;
  std::size_t pos = 0;
  while (pos < text.size()) {
    auto end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    auto line = std::string_view(text).substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) { fail(error, "empty line in display config"); return false; }
    auto equal = line.find('=');
    if (equal == std::string_view::npos || equal == 0 || equal + 1 == line.size()) { fail(error, "invalid display config line"); return false; }
    auto key = line.substr(0, equal), value = line.substr(equal + 1);
    Key id;
    if (key == "version") id = Version;
    else if (key == "main_width") id = MainWidth;
    else if (key == "main_height") id = MainHeight;
    else if (key == "main_fps") id = MainFps;
    else if (key == "alt_enabled") id = AltEnabled;
    else if (key == "alt_width") id = AltWidth;
    else if (key == "alt_height") id = AltHeight;
    else if (key == "alt_fps") id = AltFps;
    else { fail(error, "unknown display config key"); return false; }
    if (seen[id]) { fail(error, "duplicate display config key"); return false; }
    seen[id] = true;
    uint32_t n{};
    if (!number(value, n)) { fail(error, "display config value is not an integer"); return false; }
    switch (id) {
      case Version: if (n != 1) { fail(error, "unsupported display config version"); return false; } break;
      case MainWidth: parsed.main.width = n; break;
      case MainHeight: parsed.main.height = n; break;
      case MainFps: parsed.main.fps = n; break;
      case AltEnabled: if (n > 1) { fail(error, "alt_enabled must be 0 or 1"); return false; } parsed.alt.enabled = n == 1; break;
      case AltWidth: parsed.alt.width = n; break;
      case AltHeight: parsed.alt.height = n; break;
      case AltFps: parsed.alt.fps = n; break;
      case Count: break;
    }
    pos = end + 1;
  }
  for (bool present : seen) if (!present) { fail(error, "display config is missing a required key"); return false; }
  if (!valid_display_config(parsed, error)) return false;
  config = parsed;
  return true;
}

bool save_display_config(const std::string& path, const ScreenOutputConfig& config, std::string* error) {
  if (path.empty()) { fail(error, "display config path is empty"); return false; }
  if (!valid_display_config(config, error)) return false;
#if defined(_WIN32)
  const std::string temporary = path + ".tmp";
#else
  const std::string temporary = path + ".tmp." + std::to_string(static_cast<unsigned long>(getpid()));
#endif
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const auto text = serialize(config);
    if (!output || !(output << text) || !(output.flush())) { fail(error, "cannot write display config"); std::remove(temporary.c_str()); return false; }
  }
#if !defined(_WIN32)
  (void)chmod(temporary.c_str(), S_IRUSR | S_IWUSR);
#endif
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
#if defined(_WIN32)
  if (ec) { std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(temporary, path, ec); }
#endif
  if (ec) { fail(error, "cannot replace display config"); std::remove(temporary.c_str()); return false; }
  return true;
}

std::string display_config_json(const ScreenOutputConfig& c, std::string_view applies) {
  std::ostringstream out;
  out << "{\"version\":1,\"applies\":\"" << applies << "\",\"main\":{\"enabled\":true,\"width\":"
      << c.main.width << ",\"height\":" << c.main.height << ",\"fps\":" << c.main.fps
      << "},\"alt\":{\"enabled\":" << (c.alt.enabled ? "true" : "false") << ",\"width\":"
      << c.alt.width << ",\"height\":" << c.alt.height << ",\"fps\":" << c.alt.fps << "}}";
  return out.str();
}
} // namespace mvp
