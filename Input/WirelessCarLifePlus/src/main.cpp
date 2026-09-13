// CarLife+ 车机端 (Linux) 入口
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "carlife/bt_link.h"
#include "carlife/wifi_ap.h"
#include "carlife/sdl_host_sink.h"
#include "carlife/service_types.h"
#include "carlife/session.h"
#include "carlife/transport.h"

namespace {

void usage(const char* argv0) {
  std::cout
      << "CarLife+ head unit (Linux)\n"
      << "用法: " << argv0 << " [连接方式] [选项]\n"
      << "连接方式 (三选一):\n"
      << "  --phone-ip <A.B.C.D>   无线：车机主动连手机的 7240/8240/9240/9241/9242/9340\n"
      << "  --discover             无线：监听 UDP 7999，用手机发现报文的源地址作为手机 IP\n"
      << "  --listen               有线/adb 转发：车机在 7200/8200/9200/9201/9202/9300 监听\n"
      << "  --bt-rfcomm N          无线：在 RFCOMM 通道 N 上等手机蓝牙连入，换取它的 IP 后再开 7 条 TCP 通道\n"
      << "  --wifi-ap IFACE          无线：先把车机做成 WiFi 热点(hostapd+dnsmasq)让手机连进来\n"
      << "  --ssid S --psk S --wifi-band 2|5   热点参数；--print-ap-plan 只打印配置方案\n"
      << "  --bt-local PATH        蓝牙链路桥接模式（AF_UNIX，用于无蓝牙硬件的对拍/CI）\n"
      << "  --bt-timeout N         蓝牙引导超时（秒，默认 30）\n"
      << "选项:\n"
      << "  --width N --height N --fps N     协商分辨率，默认 1920x1080@30\n"
      << "  --auth sdk|trust|dev|deny        鉴权策略，默认 sdk（真实摘要校验）；trust 仅测试\n"
      << "  --auth-secret S                  dev 模式的共享口令（与模拟器一致）\n"
      << "  --bt-name S --bt-mac S           上报给手机的车机蓝牙名/MAC\n"
      << "  --cuid S --channel S             统计信息里的设备标识与渠道号\n"
      << "  --seconds N                      运行 N 秒后退出\n"
      << "  --min-frames N                   解码到 N 帧后退出\n"
      << "  --snapshot-dir DIR               周期保存 BMP 快照（无显示环境也能验证出图）\n"
      << "  --headless                       强制 SDL dummy 驱动（不开窗口）\n"
      << "  --inject-touch X,Y               自测用：进入推流后按千分比下发一次触控\n"
      << "  --verbose                        打印未识别消息\n";
}

void printSummary(const carlife::Session& session) {
  std::cout << "=== summary ===" << std::endl;
  std::cout << "state=" << carlife::toString(session.state()) << std::endl;
  std::cout << "framesReceived=" << session.framesReceived() << std::endl;
  std::cout << "framesDecoded=" << session.framesDecoded() << std::endl;
  std::cout << "videoBytes=" << session.videoBytes() << std::endl;
  std::cout << "audioBytes=" << session.audioBytes() << std::endl;
  std::cout << "heartbeatsSent=" << session.heartbeatsSent() << std::endl;
  std::cout << "displayResolution=" << session.negotiated().width << "x" << session.negotiated().height
            << std::endl;
  if (!session.lastError().empty()) std::cout << "error=\"" << session.lastError() << "\"" << std::endl;
  const bool displayed = session.framesDecoded() > 0;
  std::cout << "RESULT: " << (displayed ? "OK" : "NO_VIDEO") << std::endl;
}

bool needValue(int argc, char** argv, int* i, const char* name, std::string* out) {
  if (*i + 1 >= argc) {
    std::cerr << "缺少参数值: " << name << std::endl;
    return false;
  }
  *out = argv[++(*i)];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  carlife::SessionConfig cfg;
  std::string phoneIp;
  bool listenMode = false;
  bool discover = false;
  int discoverPort = 7999;
  int discoverTimeoutMs = 30000;
  int retrySeconds = 10;
  std::string btLocalPath;
  std::string btWifiName;
  int btRfcommChannel = -1;
  bool btAdvertise = false;
  std::string wifiApIface;
  std::string wifiSsid;
  std::string wifiPsk;
  int wifiBand = 5;
  bool printApPlan = false;
  int btTimeoutMs = 30000;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    std::string v;
    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    else if (a == "--phone-ip") { if (!needValue(argc, argv, &i, "--phone-ip", &phoneIp)) return 2; }
    else if (a == "--listen") { listenMode = true; }
    else if (a == "--discover") { discover = true; }
    else if (a == "--discover-port") { if (!needValue(argc, argv, &i, "--discover-port", &v)) return 2; discoverPort = std::atoi(v.c_str()); }
    else if (a == "--discover-timeout") { if (!needValue(argc, argv, &i, "--discover-timeout", &v)) return 2; discoverTimeoutMs = std::atoi(v.c_str()); }
    else if (a == "--width") { if (!needValue(argc, argv, &i, "--width", &v)) return 2; cfg.width = std::atoi(v.c_str()); }
    else if (a == "--height") { if (!needValue(argc, argv, &i, "--height", &v)) return 2; cfg.height = std::atoi(v.c_str()); }
    else if (a == "--fps") { if (!needValue(argc, argv, &i, "--fps", &v)) return 2; cfg.frameRate = std::atoi(v.c_str()); }
    else if (a == "--auth") { if (!needValue(argc, argv, &i, "--auth", &cfg.authMode)) return 2; }
    else if (a == "--auth-secret") { if (!needValue(argc, argv, &i, "--auth-secret", &cfg.authSecret)) return 2; }
    else if (a == "--bt-name") { if (!needValue(argc, argv, &i, "--bt-name", &cfg.btName)) return 2; }
    else if (a == "--bt-mac") { if (!needValue(argc, argv, &i, "--bt-mac", &cfg.btMac)) return 2; }
    else if (a == "--cuid") { if (!needValue(argc, argv, &i, "--cuid", &cfg.cuid)) return 2; }
    else if (a == "--channel") { if (!needValue(argc, argv, &i, "--channel", &cfg.channelId)) return 2; }
    else if (a == "--seconds") { if (!needValue(argc, argv, &i, "--seconds", &v)) return 2; cfg.seconds = std::atoi(v.c_str()); }
    else if (a == "--min-frames") { if (!needValue(argc, argv, &i, "--min-frames", &v)) return 2; cfg.minFrames = std::atoi(v.c_str()); }
    else if (a == "--snapshot-dir") { if (!needValue(argc, argv, &i, "--snapshot-dir", &cfg.snapshotDir)) return 2; }
    else if (a == "--snapshot-every") { if (!needValue(argc, argv, &i, "--snapshot-every", &v)) return 2; cfg.snapshotEverySec = std::atoi(v.c_str()); }
    else if (a == "--retry-seconds") { if (!needValue(argc, argv, &i, "--retry-seconds", &v)) return 2; retrySeconds = std::atoi(v.c_str()); }
    else if (a == "--bt-local") { if (!needValue(argc, argv, &i, "--bt-local", &btLocalPath)) return 2; }
    else if (a == "--bt-wifi-name") { if (!needValue(argc, argv, &i, "--bt-wifi-name", &btWifiName)) return 2; }
    else if (a == "--bt-rfcomm") { if (!needValue(argc, argv, &i, "--bt-rfcomm", &v)) return 2; btRfcommChannel = std::atoi(v.c_str()); }
    else if (a == "--bt-timeout") { if (!needValue(argc, argv, &i, "--bt-timeout", &v)) return 2; btTimeoutMs = std::atoi(v.c_str()) * 1000; }  // 参数单位是秒
    else if (a == "--bt-advertise") { btAdvertise = true; }
    else if (a == "--wifi-ap") { if (!needValue(argc, argv, &i, "--wifi-ap", &wifiApIface)) return 2; }
    else if (a == "--ssid") { if (!needValue(argc, argv, &i, "--ssid", &wifiSsid)) return 2; }
    else if (a == "--psk") { if (!needValue(argc, argv, &i, "--psk", &wifiPsk)) return 2; }
    else if (a == "--wifi-band") { if (!needValue(argc, argv, &i, "--wifi-band", &v)) return 2; wifiBand = std::atoi(v.c_str()); }
    else if (a == "--print-ap-plan") { printApPlan = true; }
    else if (a == "--inject-touch") { if (!needValue(argc, argv, &i, "--inject-touch", &cfg.injectTouch)) return 2; }
    else if (a == "--headless") { cfg.headless = true; }
    else if (a == "--verbose" || a == "-v") { cfg.verbose = true; }
    else { std::cerr << "未知参数: " << a << std::endl; usage(argv[0]); return 2; }
  }

  if (!printApPlan && !listenMode && !discover && phoneIp.empty() && btLocalPath.empty() && btRfcommChannel < 0) {
    std::cerr << "必须指定 --phone-ip / --discover / --listen 之一\n" << std::endl;
    usage(argv[0]);
    return 2;
  }

  std::cout << "=== CarLife+ 车机端 (Linux) ===" << std::endl;
  std::cout << "协商分辨率: " << cfg.width << "x" << cfg.height << "@" << cfg.frameRate << std::endl;
  std::cout << "鉴权模式: " << cfg.authMode << "   SDL 显示: " << (cfg.headless ? "dummy" : "自动") << std::endl;

  carlife::wifi::ApConfig apcfg;
  if (!wifiApIface.empty()) apcfg.iface = wifiApIface;
  apcfg.ssid = wifiSsid.empty() ? (cfg.btName + "-AP") : wifiSsid;
  if (!wifiPsk.empty()) apcfg.psk = wifiPsk;
  apcfg.band5 = (wifiBand == 2) ? 0 : 1;
  if (printApPlan) {
    carlife::wifi::ApPlan plan = carlife::wifi::buildPlan(apcfg);
    std::cout << "----- hostapd (" << plan.hostapdPath << ") -----\n" << plan.hostapdConf;
    std::cout << "----- dnsmasq (" << plan.dnsmasqPath << ") -----\n" << plan.dnsmasqConf;
    std::cout << "----- 拉起顺序 -----\n";
    for (const auto& cmd : plan.commands) std::cout << "# " << cmd << "\n";
    return 0;
  }

  if (btWifiName.empty()) btWifiName = apcfg.ssid;

  // ---- 无线第一步：车机开热点，手机连进来（真机上配合蓝牙引导告诉手机热点信息） ----
  if (!wifiApIface.empty()) {
    std::string note;
    if (carlife::wifi::startAp(apcfg, cfg.verbose, &note)) {
      std::cout << "[wifi] " << note << std::endl;
    } else {
      std::cout << "[wifi] 热点未启动（继续走已有的网络/桥接路径）: " << note << std::endl;
    }
  }

  // ---- 蓝牙引导：手机连上车机 SPP(RFCOMM)，4 步换到它的 IP，再开 7 条 TCP 通道 ----
  if (phoneIp.empty() && (!btLocalPath.empty() || btRfcommChannel >= 0)) {
    if (btAdvertise) {
      std::string note;
      if (carlife::BtLink::advertiseBlueZ(cfg.btName, cfg.verbose, &note)) {
        std::cout << "[bt] " << note << std::endl;
      } else {
        std::cout << "[bt] 蓝牙广播降级跳过: " << note << std::endl;
      }
    }
    carlife::BtLink bt;
    bool opened = false;
    std::string berr;
    if (btRfcommChannel >= 0) {
      opened = bt.listenRfcomm(btRfcommChannel, &berr);
      if (!opened) std::cout << "[bt] RFCOMM 监听失败(" << berr << ")，无蓝牙硬件时可改用 --bt-local 桥接" << std::endl;
    }
    if (!opened && !btLocalPath.empty()) {
      opened = bt.listenLocal(btLocalPath, &berr);
      if (!opened) {
        std::cerr << "[bt] AF_UNIX 监听失败: " << berr << std::endl;
        return 4;
      }
      std::cout << "[bt] 蓝牙链路(桥接模式)监听 " << btLocalPath << std::endl;
    }
    if (!opened) return 4;
    std::cout << "[bt] 等手机通过蓝牙连入 (SPP UUID 00001101-0000-1000-8000-00805F9B34FB)..." << std::endl;
    if (!bt.acceptLink(btTimeoutMs, &berr)) {
      std::cerr << "[bt] " << berr << std::endl;
      return 4;
    }
    carlife::BringupConfig bcfg;
    bcfg.wirelessType = carlife::btm::TYPE_WIFI;
    bcfg.wifiFrequency = carlife::btm::FREQ_5G;
    bcfg.wifiDeviceName = btWifiName;
    bcfg.timeoutMs = btTimeoutMs;
    bcfg.verbose = cfg.verbose;
    carlife::BringupResult bres;
    if (!bt.bringup(bcfg, &bres, &berr)) {
      std::cerr << "[bt] 引导失败: " << berr << std::endl;
      return 4;
    }
    phoneIp = bres.phoneIp;
    std::cout << "[bt] 引导完成，手机地址 " << phoneIp << " -> 开始 7 条 TCP 通道" << std::endl;
    bt.close();
  }

  if (phoneIp.empty() && discover) {
    std::cout << "[net] 等待手机在 UDP :" << discoverPort << " 上的 CarLife 发现报文..." << std::endl;
    carlife::DiscoveryResult dr;
    if (!carlife::Discovery::waitForPhone(discoverPort, discoverTimeoutMs, &dr)) {
      std::cerr << "[fail] 超时未发现手机（可用 --phone-ip 直连）" << std::endl;
      return 3;
    }
    phoneIp = dr.phoneIp;
    std::cout << "[net] 发现手机 " << phoneIp << ":" << dr.port << "  payload=" << dr.raw.substr(0, 48) << std::endl;
  }

  bool ok = false;
  bool displayed = false;
  bool authRejected = false;
  std::string err;
  const auto retryUntil = std::chrono::steady_clock::now() + std::chrono::seconds(retrySeconds);
  for (;;) {
    carlife::Session session(cfg);
    // 本机工具保留原有 SDL 呈现：与重构前行为一致，只是把窗口/解码器/音频设备
    // 从 Session 搬到了宿主机侧。
    carlife::SdlHostSink host;
    host.setLogger([&session](const char* level, const std::string& msg) {
      session.log_line(level, msg);
    });
    const int scale = std::max(1, static_cast<int>(std::ceil(static_cast<double>(cfg.width) / 1280.0)));
    std::string hostErr;
    if (!host.open(cfg.width / scale, cfg.height / scale,
                   "CarLife+ HU (Linux) " + std::to_string(cfg.width) + "x" +
                       std::to_string(cfg.height),
                   cfg.headless, &hostErr)) {
      std::cerr << "[error] host sink open failed: " << hostErr << std::endl;
      return 1;
    }
    session.setHost(&host);
    if (listenMode) {
      ok = session.runAsServer(&err);
    } else {
      ok = session.runWithPhone(phoneIp, &err);
    }
    // 只有"一个通道都没连上"才重试，一旦进入过会话就按正常流程收尾
    if (session.state() != carlife::State::Idle) {
      printSummary(session);
      displayed = session.framesDecoded() > 0;
      authRejected = (session.state() == carlife::State::Failed);
      break;
    }
    if (!err.empty()) std::cerr << "[warn] " << err << std::endl;
    if (std::chrono::steady_clock::now() >= retryUntil) {
      std::cerr << "[fail] 重试窗口内连不上手机，放弃" << std::endl;
      printSummary(session);
      break;
    }
    std::cout << "[net] 手机还没就绪，0.5s 后重试..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  if (cfg.authMode == "deny") return authRejected ? 0 : 1;  // 负向用例：期待鉴权被判失败
  return displayed ? 0 : 1;
}
