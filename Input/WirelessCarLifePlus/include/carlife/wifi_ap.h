// 车机侧 WiFi 热点（无线 CarLife+ 的第一段：手机要先连进车机的网络）
// 参考 carlife-sdk 的 transport 家族：WirlessAPProtocolTransport / WifiDirectManager
// + InstantConnectionSetup 的 TYPE_WIFI / FREQUENCY_2_4G / FREQUENCY_5G。
#pragma once
#include <string>
#include <vector>

namespace carlife {
namespace wifi {

struct ApConfig {
  std::string iface = "wlan0";
  std::string ssid = "zero2w-CarLife";
  std::string psk = "carlife12345";
  int band5 = 1;              // 1 = 5GHz(FREQUENCY_5G) / 0 = 2.4GHz
  int channel = 0;            // 0 = 自动
  std::string country = "CN";
  std::string gateway = "192.168.43.1";
  std::string dhcpFrom = "192.168.43.10";
  std::string dhcpTo = "192.168.43.200";
  std::string confDir = "/tmp";
};

struct ApPlan {
  std::string hostapdConf;      // 文件内容
  std::string hostapdPath;
  std::string dnsmasqConf;      // 文件内容
  std::string dnsmasqPath;
  std::vector<std::string> commands;  // 拉起顺序（含 ip addr / hostapd / dnsmasq）
};

// 纯函数：生成配置，可单测、可 dry-run
ApPlan buildPlan(const ApConfig& cfg);

// 真机：写配置 + 拉起 hostapd/dnsmasq；缺工具或没有无线网卡时返回 false 并给出原因
bool startAp(const ApConfig& cfg, bool verbose, std::string* note);
void stopAp(bool verbose);

}  // namespace wifi
}  // namespace carlife