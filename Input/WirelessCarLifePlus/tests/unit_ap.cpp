// WiFi 热点计划生成测试（不依赖无线网卡，纯函数断言）
#include <iostream>
#include "carlife/wifi_ap.h"

using namespace carlife;

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::cout << "FAIL  " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; \
      ++g_fail;                                                                  \
    } else {                                                                     \
      ++g_pass;                                                                  \
    }                                                                            \
  } while (0)

int main() {
  wifi::ApConfig c;
  c.iface = "wlan0";
  c.ssid = "zero2w-CarLife";
  c.psk = "carlife12345";
  c.band5 = 1;
  wifi::ApPlan p5 = wifi::buildPlan(c);
  CHECK(p5.hostapdConf.find("interface=wlan0") != std::string::npos);
  CHECK(p5.hostapdConf.find("ssid=zero2w-CarLife") != std::string::npos);
  CHECK(p5.hostapdConf.find("hw_mode=a") != std::string::npos);   // 5GHz

  c.band5 = 0;
  wifi::ApPlan p2 = wifi::buildPlan(c);
  CHECK(p2.hostapdConf.find("hw_mode=g") != std::string::npos);   // 2.4GHz
  CHECK(p2.dnsmasqConf.find("dhcp-range=192.168.43.1,192.168.43.10,192.168.43.200,12h") !=
        std::string::npos);
  CHECK(p2.commands.size() == 4);
  CHECK(p2.commands[2].find("hostapd -B") != std::string::npos);
  CHECK(p2.commands[3].find("dnsmasq -C") != std::string::npos);
  std::cout << "unit-ap: " << g_pass << " passed, " << g_fail << " failed" << std::endl;
  return g_fail ? 1 : 0;
}