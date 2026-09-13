#include "carlife/wifi_ap.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace carlife {
namespace wifi {
namespace {

bool toolExists(const char* bin) {
  std::string cmd = "command -v ";
  cmd += bin;
  cmd += " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

bool ifaceExists(const std::string& iface) {
  return access(("/sys/class/net/" + iface).c_str(), F_OK) == 0;
}

void writeFile(const std::string& path, const std::string& body) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << body;
}

}  // namespace

ApPlan buildPlan(const ApConfig& cfg) {
  ApPlan plan;
  plan.hostapdPath = cfg.confDir + "/carlife-hostapd.conf";
  plan.dnsmasqPath = cfg.confDir + "/carlife-dnsmasq.conf";

  std::ostringstream hp;
  hp << "interface=" << cfg.iface << "\n"
     << "driver=nl80211\n"
     << "ssid=" << cfg.ssid << "\n"
     << "country_code=" << cfg.country << "\n"
     << "hw_mode=" << (cfg.band5 ? "a" : "g") << "\n"
     << "channel=" << cfg.channel << "\n"
     << "ieee80211n=1\n"
     << "wmm_enabled=1\n"
     << "auth_algs=1\n"
     << "wpa=2\n"
     << "wpa_key_mgmt=WPA-PSK\n"
     << "rsn_pairwise=CCMP\n"
     << "wpa_passphrase=" << cfg.psk << "\n";
  plan.hostapdConf = hp.str();

  std::ostringstream dm;
  dm << "interface=" << cfg.iface << "\n"
     << "bind-interfaces\n"
     << "no-resolv\n"
     << "dhcp-range=" << cfg.gateway << "," << cfg.dhcpFrom << "," << cfg.dhcpTo << ",12h\n"
     << "dhcp-option=3," << cfg.gateway << "\n"
     << "dhcp-option=6," << cfg.gateway << "\n";
  plan.dnsmasqConf = dm.str();

  plan.commands = {
      "ip link set " + cfg.iface + " up",
      "ip addr add " + cfg.gateway + "/24 dev " + cfg.iface,
      "hostapd -B " + plan.hostapdPath,
      "dnsmasq -C " + plan.dnsmasqPath,
  };
  return plan;
}

bool startAp(const ApConfig& cfg, bool verbose, std::string* note) {
  if (!ifaceExists(cfg.iface)) {
    if (note) *note = "没有无线网卡 " + cfg.iface + "（可先不带 --wifi-ap，手机已有可用网络时直接 --bt-local/--phone-ip）";
    return false;
  }
  if (!toolExists("hostapd") || !toolExists("dnsmasq")) {
    if (note) *note = "缺少 hostapd / dnsmasq，无法把本机做成热点";
    return false;
  }
  const ApPlan plan = buildPlan(cfg);
  writeFile(plan.hostapdPath, plan.hostapdConf);
  writeFile(plan.dnsmasqPath, plan.dnsmasqConf);
  for (const auto& cmd : plan.commands) {
    if (verbose) std::cout << "[wifi] # " << cmd << std::endl;
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
      if (note) *note = "命令失败(" + std::to_string(rc) + "): " + cmd + " — " + strerror(errno);
      return false;
    }
  }
  if (note) {
    std::ostringstream os;
    os << "热点已就绪: SSID=" << cfg.ssid << " 频段=" << (cfg.band5 ? "5GHz" : "2.4GHz")
       << " 网关=" << cfg.gateway;
    *note = os.str();
  }
  return true;
}

void stopAp(bool verbose) {
  const char* cmds[] = {"pkill -f carlife-dnsmasq.conf >/dev/null 2>&1",
                        "pkill -f carlife-hostapd.conf >/dev/null 2>&1"};
  for (const char* c : cmds) {
    if (verbose) std::cout << "[wifi] # " << c << std::endl;
    std::system(c);
  }
}

}  // namespace wifi
}  // namespace carlife