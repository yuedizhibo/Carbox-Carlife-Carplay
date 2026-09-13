#include "wirelesscarplay/display_config.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
using namespace mvp;
namespace { int checks; }
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "check failed at " << __LINE__ << ": " #x "\n"; return 1; } } while (0)
int main() {
  const std::string path = "/tmp/zero2w-display-config-" + std::to_string(getpid());
  std::remove(path.c_str());
  ScreenOutputConfig c;
  std::string error;
  CHECK(load_display_config(path, c, &error));
  CHECK(c.main.width == 1920 && c.main.height == 1080 && c.main.fps == 60);
  CHECK(!c.alt.enabled && c.alt.width == 1280 && c.alt.height == 720 && c.alt.fps == 30);
  c.main = {true, 1280, 720, 50};
  c.alt = {true, 800, 480, 25};
  CHECK(save_display_config(path, c, &error));
  ScreenOutputConfig roundtrip;
  CHECK(load_display_config(path, roundtrip, &error));
  CHECK(roundtrip.main.width == 1280 && roundtrip.main.height == 720 && roundtrip.main.fps == 50);
  CHECK(roundtrip.alt.enabled && roundtrip.alt.width == 800 && roundtrip.alt.height == 480 && roundtrip.alt.fps == 25);
  CHECK(display_config_json(roundtrip).find("\"applies\":\"next-session\"") != std::string::npos);

  ScreenOutputConfig invalid = roundtrip;
  invalid.main.width = 1279;
  CHECK(!valid_display_config(invalid, &error));
  invalid = roundtrip;
  invalid.main.fps = 61;
  CHECK(!valid_display_config(invalid, &error));
  invalid = roundtrip;
  invalid.main = {true, 2560, 1440, 60};
  invalid.alt = {true, 2560, 1440, 60};
  CHECK(!valid_display_config(invalid, &error));

  { std::ofstream out(path, std::ios::trunc); out << "version=1\nmain_width=1920\n"; }
  CHECK(!load_display_config(path, roundtrip, &error));
  { std::ofstream out(path, std::ios::trunc); out << "version=1\nmain_width=1920\nmain_height=1080\nmain_fps=60\nalt_enabled=2\nalt_width=800\nalt_height=480\nalt_fps=30\n"; }
  CHECK(!load_display_config(path, roundtrip, &error));
  std::remove(path.c_str());
  std::cout << "display config checks executed: " << checks << '\n';
}
