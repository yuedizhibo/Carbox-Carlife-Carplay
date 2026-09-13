// 蓝牙引导层（CarLife+ 无线的第二条腿）
//
// 依据参考实现：
//   carlife-sdk/.../receiver/transport/instant/InstantConnectionSetup.kt   （4 步引导流程）
//   carlife-sdk/.../internal/transport/communicator/BluetoothCommunicator.kt（在 BT 链路上传 CMD 通道 8 字节帧）
//   carlife-sdk/.../sdk/Constants.kt: BLUETOOTH_COMMUNICATE_UUID = 00001101-0000-1000-8000-00805F9B34FB (SPP)
//
// 消息序列（全部走 CMD 通道短头：len(u16) + 保留(2) + serviceType(u32)）：
//   手机 -> 车机  MSG_WIRELESS_INFO_REQUEST   0x00100001
//   车机 -> 手机  MSG_WIRELESS_INFO_RESPONSE  0x00108002  CarlifeWirlessInfo{wirlessType,wifiFrequency}
//   手机 -> 车机  MSG_WIRELESS_TARGET_INFO_REQUEST  0x00100004
//   车机 -> 手机  MSG_WIRELESS_TARGET_INFO_RESPONSE 0x00108005 CarlifeWirlessTarget{wifiDeviceName,targetInfo}
//   车机 -> 手机  MSG_WIRELESS_REQUEST_IP     0x00108006  (空载荷)
//   手机 -> 车机  MSG_WIRELESS_RESPONSE_IP    0x00100007  CarlifeWirlessIp{wirlessip}
//   手机 -> 车机  MSG_WIRELESS_MD_STATUS      0x00100008  CarlifeWirlessStatus{status}
//   车机 -> 手机  MSG_WIRELESS_HU_STATUS      0x00108009
#pragma once
#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <vector>

namespace carlife {

namespace btm {
constexpr int32_t TYPE_NONE = 0;
constexpr int32_t TYPE_WIFI = 1;
constexpr int32_t TYPE_WIFI_DIRECT = 2;
constexpr int32_t TYPE_ALL = 3;
constexpr int32_t FREQ_2_4G = 0;
constexpr int32_t FREQ_5G = 1;
}  // namespace btm

struct BringupConfig {
  int32_t wirelessType = btm::TYPE_WIFI;      // 我们提供的是车机侧 WiFi AP/热点
  int32_t wifiFrequency = btm::FREQ_5G;
  std::string wifiDeviceName;                 // 上报给手机的 WiFi 接口/热点名
  std::string targetInfo;
  int timeoutMs = 30000;
  bool verbose = false;
};

struct BringupResult {
  std::string phoneIp;      // 手机在 CarLife 网络里的地址
  int32_t peerType = 0;
  int32_t peerFrequency = 0;
  std::string peerDeviceName;
};

// 一条蓝牙链路（真机 = RFCOMM/SPP；测试 = AF_UNIX，走同一套帧）
class BtLink {
 public:
  BtLink();
  ~BtLink();
  BtLink(const BtLink&) = delete;
  BtLink& operator=(const BtLink&) = delete;

  // 真机：绑定 SPP RFCOMM 通道监听（需要 BlueZ 已 sdptool add SP）
  bool listenRfcomm(int channel, std::string* err);
  // 测试/桥接：AF_UNIX 监听
  bool listenLocal(const std::string& path, std::string* err);
  // 作为主动方去连（一般车机是被动方，这个给对拍工具用）
  bool connectLocal(const std::string& path, int timeoutMs, std::string* err);
  bool connectRfcomm(const std::string& mac, int channel, int timeoutMs, std::string* err);

  // 接管一条已建立的链路（例如 BlueZ Profile1.NewConnection 递过来的 fd）。
  // 成功后 fd 归本对象所有，close()/析构会关闭它。
  bool adoptFd(int fd, std::string* err);

  // 等手机连上来（listen* 之后调用）
  bool acceptLink(int timeoutMs, std::string* err);

  // ---- 依据参考实现（receiver/transport/instant/BluetoothDeviceDiscover.kt）----
  // 车机必须【主动连到手机的 SPP socket】，而不是等手机连我们：
  //   参考实现是 device.createRfcommSocketToServiceRecord(BLUETOOTH_COMMUNICATE_UUID) + connect()，
  //   即车机做客户端；手机侧 CarLife 进入无线模式后开始监听 SPP。
  //   双方都当服务端就会死锁，手机界面停在“连接中”。
  //
  // 找到已配对的手机（参考实现是在 bondedDevices 里按名字找）。
  // wantName 为空表示不按名字过滤，取第一个已配对设备。
  static bool findPairedPhone(const std::string& wantName, std::string* mac, std::string* name);
  // 严格查 SPP：只认 Service Class List 里带 Serial Port(0x1101) 的那条记录的通道。
  // 【不做任何兜底猜测】—— 实测教训：旧版“取第一个 Channel:”会抓到
  // Headset Gateway(1112) 的 channel 2，拿它去连 SPP 当然失败。
  // 失败返回 -1，并把手机实际提供的 RFCOMM 服务清单写进 note（便于诊断）。
  static int querySppChannel(const std::string& mac, std::string* note);
  // 列出远端全部 RFCOMM 服务（名称 + 通道号）。
  // 用途：手机若用厂商自定义 UUID 发布 CarLife 通道（而非标准 0101），
  // 就靠这份清单逐个探测 —— 判据是对方是否真的发 CarLife 报文，不是猜。
  static std::vector<std::pair<std::string, int>> listRfcommChannels(const std::string& mac);

  bool send(uint32_t serviceType, const std::vector<uint8_t>& payload = {});
  bool recv(uint32_t* serviceType, std::vector<uint8_t>* payload, int timeoutMs);
  void close();
  bool isOpen() const { return linkFd_ >= 0; }

  // 跑完 4 步引导，拿到手机 IP
  bool bringup(const BringupConfig& cfg, BringupResult* out, std::string* err);

  // 尽力而为地把本机蓝牙打开、可被发现、注册 SPP 服务（hciconfig/btmgmt/sdptool）
  static bool advertiseBlueZ(const std::string& name, bool verbose, std::string* note);

 private:
  bool bindListen(int domain, const sockaddr* addr, socklen_t len, const std::string& what,
                  std::string* err);
  bool sendRaw(uint32_t serviceType, const std::vector<uint8_t>& payload);

  int listenFd_ = -1;
  int linkFd_ = -1;
  std::vector<uint8_t> rxbuf_;
  std::string path_;
};

}  // namespace carlife