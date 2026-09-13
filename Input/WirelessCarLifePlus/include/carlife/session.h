// CarLife+ 车机端会话状态机
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "carlife/encryption.h"
#include "carlife/host_sink.h"
#include "carlife/session_state.h"
#include "carlife/transport.h"
#include "carlife/wire.h"

namespace carlife {

struct SessionConfig {
  int width = 1920;           // 默认协商分辨率 1920x1080
  int height = 1080;
  int frameRate = 30;
  std::string cuid = "zero2w-000001";
  // CarLife 通道号（CarlifeStatisticsInfo.channel）：这是百度发放给 OEM 的厂商标识。
  // 【真机实测教训】原来写的是自编字符串 "zero2w"。手机（小米 CarLife 8.8.4）在
  // 收到 HU_INFO + HU_AUTH_REQUEST 后【直接回 MD_AUTH_RESULT=false】，
  // 连 MD_AUTH_RESPONSE（挑战值）都不发 —— 说明它在【我们的身份】上就没通过。
  // 通道号是最明显的假身份：参考树里自带的 bdcf 就写着一个真实通道号：
  //   #Channel Id: Baidu will provide the channel id to all OEM
  //   20022100
  // 这里先照它用（参考实现的 bdcf 就是那份配置）。
  std::string channelId = "20022100";
  std::string versionName = "1.0.0";
  std::string huName = "zero2w CarLife HU";
  std::string btName = "CarLife-HU";
  // 车机蓝牙 MAC：会通过 HU_INFO.btAddress 与 HU_FEATURE_CONFIG_RESPONSE.huBtMac
  // 上报给手机，供手机定位/记住这台车机。
  // 【不能给占位值】原来默认是 "00:11:22:33:44:55"，而 Core/Web/src/main.cpp 从未
  // 赋值 → 上报给手机的是一个不存在的地址，手机会据此找不到车机的蓝牙设备。
  // 现在默认空：空值不会写出该字段（wire.cpp 里 if(!empty) 才写）——宁可不报也不报假。
  // 实际值由 main.cpp 从 CARLIFE_BT_MAC 或自动探测适配器得到。
  std::string btMac;
  std::string brand = "zero2w";
  std::string model = "CarLifeHU-Linux";
  std::string authMode = "sdk";   // sdk (default) | trust (explicit test only) | dev | deny
  std::string authSecret = "zero2w-shared-secret";
  // 内容加密（AUDIT 3.1 / 5.4）：能力位 CONTENT_ENCRYPTION 报什么、是否走 RSA+AES 协商。
  // 参考实现里 bdcf 的默认就是 CONTENT_ENCRYPTION=true，EncryptConfig 是
  // DEBUG_ENABLE=true / AES_ENCRYPT_AS_BEGINE=false（即 RSA 交换密钥 → 之后 AES）。
  // 因此默认打开；真机若不接受，用 --no-content-encryption 一键退回明文（不改代码）。
  bool contentEncryption = true;
  // 激活状态：参考实现里激活是 OEM 商务流程（BOX_ACTIVE/HU_ACTIVE）。
  // 未提供外部激活状态与 token 时保持未知，不用固定成功代替激活结果。
  int activationState = 0;
  std::string activationToken;
  // 车况上报周期（毫秒）。0 = 不主动上报（只应手机的订阅请求）。
  int vehicleReportMs = 1000;
  int seconds = 0;                  // >0 到时退出（WSL 测试用）
  int minFrames = 0;                // >0 达到解码帧数后退出
  std::string snapshotDir;          // 非空则周期存 BMP 快照
  int snapshotEverySec = 2;
  bool headless = false;
  bool verbose = false;
  std::string injectTouch;   // "x,y"：进入推流后向手机下发一次触控（自测触控回传）
};

class Session {
 public:
  explicit Session(SessionConfig cfg);
  ~Session();

  // 呈现由宿主机接管，必须在 start()/runWithPhone()/runAsServer() 之前设置。
  // 不设置则只跑协议：视频与音频会被计数但不会呈现。
  void setHost(HostSink* sink) { host_ = sink; }
  HostSink* host() const { return host_; }
  // 与内部 log 相同的输出格式，供宿主机复用（run_wsl_tests.sh 依赖这些行）。
  void log_line(const char* level, const std::string& msg) const { log(level, msg); }
  // 请求结束会话：置停止标志并关闭 7 条通道，使 mainLoop 与 accept 尽快返回。
  // 宿主机停止时必须先调它再 join，否则会话线程会一直阻塞在收包上。
  void requestStop();
  // 鉴权校验缝：真机的 getVerifyResult 在闭源 libencryption.so 里，我们不冒充。
  // 传 nullptr 表示回到 config.authMode，默认 sdk；trust/dev/deny 仅用于显式测试。
  void setVerifySeam(VerifySeam* seam) { verify_seam_ = seam; }

  bool runWithPhone(const std::string& phoneIp, std::string* err);
  bool runAsServer(std::string* err);

  uint64_t framesReceived() const { return framesReceived_; }
  uint64_t framesDecoded() const;
  uint64_t audioBytes() const;  uint64_t videoBytes() const { return videoBytes_; }
  uint64_t heartbeatsSent() const { return heartbeatsSent_; }
  // A3：UPDATE 通道的接收计数（消息数 / 字节数）。
  uint64_t updateMessages() const { return updateMsgs_; }
  uint64_t updateBytes() const { return updateBytes_; }
  State state() const { return state_; }
  std::string lastError() const { return lastError_; }
  VideoInfo negotiated() const { return negotiated_; }
  // 内容加密状态（管理页面展示用）。
  EncryptionState encryptionState() const { return cipher_.state(); }
  std::size_t aesKeyBits() const { return cipher_.aes_key_bits(); }
  // 帧率动态调整（AUDIT 3.4 / 5.1 #11）：请求手机改编码帧率。
  // 只有会话已进 Established 之后才有意义，否则返回 false。
  bool requestFrameRate(int32_t fps);
  // 多点触控是否启用（对应能力位 MULTI_TOUCH，也是 RemoteControlManager 的分支条件）。
  // 启用时所有触控都走 MSG_TOUCH_ACTION_3 定长格式；关闭时回退到旧的
  // TOUCH_ACTION_DOWN/UP/MOVE + TouchSinglePoint（与官方 C++ 车机库一致）。
  void setMultiTouch(bool on) { multiTouch_ = on; }
  bool multiTouch() const { return multiTouch_; }

  // ── 本轮新增：各类接口的接收计数（供测试与管理页面判定“真的在解析”）──
  uint64_t mediaInfoCount() const { return mediaInfoCount_; }
  uint64_t naviCount() const { return naviCount_; }
  uint64_t phoneInputCount() const { return phoneInputCount_; }
  uint64_t hfpCount() const { return hfpCount_; }
  uint64_t vehicleControlCount() const { return vehicleControlCount_; }
  uint64_t voiceControlCount() const { return voiceControlCount_; }
  uint64_t fileTransferCount() const { return fileTransferCount_; }
  uint64_t activationCount() const { return activationCount_; }
  uint64_t carDataSubscribeCount() const { return carDataSubscribeCount_; }
  uint64_t vehicleReportSent() const { return vehicleReportSent_; }
  uint64_t multiTouchCount() const { return multiTouchCount_; }
  uint64_t encryptedMessages() const { return encryptedMessages_; }
  uint64_t decryptFailures() const { return decryptFailures_; }

 private:
  bool start(std::string* err);
  void mainLoop();
  void setState(State s);
  bool sendCmd(uint32_t serviceType, std::vector<uint8_t> payload = {});
  // 所有出站消息都必须走这里：它负责在加密启用后对【载荷】加密（帧头保持明文，
  // 因为协议要靠帧头的 payloadSize 才能定位密文——照抄 EncryptionTool.kt：只加密
  // commandSize 之后的 payload，并同步修正 payloadSize；Frame::encode 已经做了后者）。
  bool sendOn(int32_t channel, uint32_t serviceType, std::vector<uint8_t> payload = {});
  // 入站解密：只在加密就绪时生效；解密失败把 serviceType 加入 decryptExcludes 并
  // 按明文继续（照抄 EncryptionTool.decrypt 的容错设计，绝不断会话）。
  void decryptInbound(Frame& frame);
  void handleFrame(const Frame& f);
  void handleProtocolVersionMatch(const Frame& f);
  void handleMdInfo(const Frame& f);
  void handleAuthResponse(const Frame& f);
  void handleAuthResult(const Frame& f);
  void handleFeatureConfigRequest(const Frame& f);
  // 能力协商表（HU_FEATURE_CONFIG_RESPONSE 的 configs 段）的生成。
  // 与发送分离：CONTENT_ENCRYPTION 不能只照配置报位，还要看本会话是否真的持有
  // RSA 密钥对（否则就是向手机承诺一个我们不兑现的能力），所以这里必须是可单测的。
  pb::FeatureConfigList buildFeatureConfig() const;
  // VR 上行：向宿主机要一帧 PCM16，按 micBuffer_ 容量校验后拷进载荷。
  // 返回 false = 本帧无数据（未采集 / 空帧 / 宿主机回报的帧数越界）。
  bool takeMicFrame(std::vector<uint8_t>* payload);
  void handleVideoInitDone();
  void handleVideoData(const Frame& f);
  void handleMediaInit(const Frame& f);
  void handleModuleStatus(const Frame& f);
  // ── 本轮新增的解析入口（MD->HU）──
  void handleMediaInfo(const Frame& f);
  void handleNaviNextTurn(const Frame& f);
  void handleNaviAssistantGuide(const Frame& f);
  // 手机上报档位（C++ 库的 MSG_CMD_GEAR_INFO = 0x00010029）。
  void handlePhoneGear(const Frame& f);
  void handleTouchPad(uint32_t service_type, const Frame& f);
  // 多点触控（MSG_TOUCH_ACTION_3，定长格式）。
  void handleTouchAction3(const Frame& f);
  void handleTelephonyState(uint32_t service_type);
  void handleHfpRequest(const Frame& f);
  void handleHfpCallStatusCover(const Frame& f);
  void handleHfpStatusRequest(const Frame& f);
  void handleVoiceControl(const Frame& f);
  // MIC_RECORD_PREPARE_START：提示音完成/失败均回 DONE，不声称麦克风已就绪。
  // 非阻塞、无线程：宿主机回调必须同步给出结果（见 HostSink::prepareMicrophone）。
  void handleMicRecordPrepare();
  void handleVehicleControl(const Frame& f);
  void handleActivationRequest(const Frame& f);
  void handleFileTransfer(uint32_t service_type, const Frame& f);
  void handleCarDataSubscribe(uint32_t service_type, const Frame& f);
  void handleEncryption(uint32_t service_type, const Frame& f);
  void publishVehicleReport();

  void onEstablished();
  void pollTouchOutput();
  void pollMicrophone();
  void pollVehicleReport();
  void maybeInjectTouch();
  bool verifyAuth(const std::string& seed, const std::string& encryptValue);
  // 单测访问器（见 tests/input_api_tests.cpp）。
  // 【边界】只给测试一个具名的友元入口，不把协议状态改成 public —— 也不为测试
  // 加任何运行期可见的 API。
  friend struct SessionApiTestAccess;
  void heartbeatLoop();
  void snapshotLoop();
  void log(const char* level, const std::string& msg) const;

  SessionConfig cfg_;
  Transport tx_;
  // 解码、开窗、音频设备都属于宿主机；Session 只持有回调。
  HostSink* host_{};
  // 鉴权校验缝（闭源边界，见 encryption.h 的 VerifySeam）。
  VerifySeam* verify_seam_{};
  // 内容加密（RSA 交换 AES → 之后全载荷 AES-ECB/PKCS5）。
  ContentCipher cipher_;

  State state_ = State::Idle;
  std::string lastError_;
  VideoInfo negotiated_{};
  int32_t adoptedProtocolVersion_ = 0;
  // 我方声明的协议主版本。参考实现里它是可配置项（默认 PROTOCOL_VERSION_MAJOR_VERSION_4），
  // 而合法取值只有 1..4（没有 0）。实测真机：发 0 时手机直接回 NOT_MATCH(2) 并告知它的版本是 2，
  // 而 CarLife 2.0 就对应 major=2（本工程 kSdkVersionCode="2.0"）。
  // 不匹配时我们会按手机告知的版本重发一次（见 handleProtocolVersionMatch）。
  int32_t sentProtocolMajor_ = 2;
  pb::DeviceInfo mdInfo_{};
  std::string authSeed_;
  std::string authConnectTime_;
  uint64_t statsSentAt_ = 0;
  bool firstFrameLogged_ = false;
  bool touchInjected_ = false;
  // 车辆数据订阅：手机请求后我们才开始周期上报（AUDIT 3.6）。
  bool carDataSubscribed_ = false;
  uint64_t lastVehicleReportMs_ = 0;
  // 多点触控开关（见 setMultiTouch）。默认开：我们本来就把 MULTI_TOUCH 能力位报 1。
  bool multiTouch_ = true;

  std::atomic<uint64_t> framesReceived_{0};
  std::atomic<uint64_t> videoBytes_{0};
  std::atomic<uint64_t> heartbeatsSent_{0};
  // A3：UPDATE 通道消费计数（见 session.cpp 的 case ch::UPDATE）。
  std::atomic<uint64_t> updateMsgs_{0};
  std::atomic<uint64_t> updateBytes_{0};
  // TTS（导航音）与 VR（语音上行麦克风）的接入计数，供管理页面展示。
  std::atomic<uint64_t> ttsBytes_{0};
  std::atomic<uint64_t> micRequests_{0};
  std::atomic<uint64_t> micFrames_{0};
  // “决定回 MSG_CMD_MIC_RECORD_PREPARE_DONE(0x18072)”的次数。
  // 只有宿主机真的报了“准备完成”才会加一 —— 因此它同时是“没有谎报”的证据，
  // 也是无 socket 单测里唯一可观察的出站判据（传输是否成功由 sendCmd 返回值单独记日志）。
  std::atomic<uint64_t> micPrepareDoneAttempts_{0};
  std::array<int16_t, 1600> micBuffer_{};
  bool mic_recording_{};
  // 本轮新增：各类接口计数。
  std::atomic<uint64_t> mediaInfoCount_{0};
  std::atomic<uint64_t> naviCount_{0};
  std::atomic<uint64_t> phoneInputCount_{0};
  std::atomic<uint64_t> multiTouchCount_{0};
  std::atomic<uint64_t> hfpCount_{0};
  std::atomic<uint64_t> vehicleControlCount_{0};
  std::atomic<uint64_t> voiceControlCount_{0};
  std::atomic<uint64_t> fileTransferCount_{0};
  std::atomic<uint64_t> activationCount_{0};
  std::atomic<uint64_t> carDataSubscribeCount_{0};
  std::atomic<uint64_t> vehicleReportSent_{0};
  std::atomic<uint64_t> encryptedMessages_{0};
  std::atomic<uint64_t> decryptFailures_{0};
  std::atomic<bool> running_{false};
  std::thread hbThread_;
  std::thread snapThread_;
  std::chrono::steady_clock::time_point startedAt_;
  std::atomic<uint64_t> lastVideoByteMs_{0};
  std::atomic<uint64_t> lastHeartbeatAckMs_{0};
};

}  // namespace carlife
