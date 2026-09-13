// 宿主机缝：把 CarLife 协议与会话表现分开。
//
// 依据：
//   * carlife-vehicle-lib（百度官方 V0.15 C++ 车机库）把控制/视频/音频/触控拆成
//     独立通道模块，并通过类型化结构体把显示与音频交给宿主机，库本身不渲染；
//   * carlinkit-cxx 把解码源（ck::DecoderSource）与会话解耦，显示后端可替换；
//   * 本项目 PROJECT_ARCHITECTURE.md 要求 Input 只做输入，输出/呈现属于 Core。
//
// 因此 Session 只做协议：它不再解码、不再开窗、不再播放音频，而是把
// Annex-B、PCM 和状态交给 HostSink，并从 HostSink 取回要发给手机的控制。
// 独立工具 carlife-hu 使用 SdlHostSink 保留原有本地显示行为；
// Core 接入使用 CarLifeInputAdapter，把数据交给 SessionCore 与有界媒体面。
//
// 【本轮的扩展】审计里 CarLife 侧全部 ❌ 的能力都要有落点，因此 HostSink 从
// “视频+音频+进度”扩到全部业务：元数据/封面/歌词、导航逐向、车况、电话/HFP、
// 通讯录/通话记录、多点触控/触摸板/手势、激活、文件传输/OTA、车控、语音控制。
// 每条都用 pb:: 类型直传（这些结构体就是协议数据，不需要再包一层 DTO）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "carlife/session_state.h"
#include "carlife/service_types.h"
#include "carlife/wire.h"

namespace carlife {

// 宿主机 -> 会话：需要回传给手机的控制。
struct HostControl {
  enum class Type : uint8_t {
    Touch,        // 单点触控（现有）
    Key,          // 车机硬键（现有；keycode 用 CarLife 的 keycode::*，不是 Android 的）
    MultiTouch,   // 多点触控：TOUCH_ACTION_POINTER_DOWN / POINTER_UP
    TouchPad,     // 触摸板（MSG_TOUCH_PAD_* 回程）
    // ── 命令型控制（HU->MD，载荷由 encodeControlCommand 编码）──
    Foreground,   // 请求手机把 CarLife 转前台（MSG_CMD_GO_TO_FOREGROUND 0x00018025，空载荷）
    ModuleControl,  // 模块控制（MSG_CMD_MODULE_CONTROL 0x00018028，singular CarlifeModuleStatus）
  };
  enum class TouchPhase : uint8_t { Down = 0, Up = 1, Move = 2 };
  Type type = Type::Touch;
  TouchPhase phase = TouchPhase::Up;
  int32_t x = 0;        // 已按协商分辨率换算的像素坐标
  int32_t y = 0;
  int32_t keycode = 0;  // CarLife keycode::*（见 service_types.h 的 keycode 命名空间）
  // ModuleControl：模块号与状态号（CarlifeModuleStatus{moduleID=field1, statusID=field2}）。
  // 适配器在入队前就拒绝越界的 module_id 与负的 status_id，所以队列里的值一定合法。
  int32_t module_id = 0;
  int32_t status_id = 0;
  // 【模块号范围 1..kModuleIdMax】参考实现里模块号是从 1 起的小整数
  // （Reference/carlife-vehicle-lib/.../model/ModuleStatusModel.java:20-46：
  //   PHONE=1, NAVI=2, MUSIC=3, VR=4, CONNECT=5, MIC=6）；本轮契约给到 7。
  // 超范围一律拒绝：发一个不存在的模块号，手机只会静默丢弃或误动作，
  // 而协议里没有任何东西能告诉我们它到底做了什么。
  static constexpr int32_t kModuleIdMin = 1;
  static constexpr int32_t kModuleIdMax = 7;
  // 多点触控：最多 kMaxPoints 个触点。id 用触点序号（0..n-1）。
  static constexpr std::size_t kMaxPoints = 10;
  struct Point {
    int32_t x = 0, y = 0;
    uint8_t id = 0;
  };
  Point points[kMaxPoints]{};
  uint8_t point_count = 0;
};

// 命令型控制的线格式映射（纯函数：只读参数，不碰任何状态，可直接单测）。
//   Foreground    -> msg::GO_TO_FOREGROUND (0x00018025)，载荷【空】
//                    参考实现 cmdGoToForeground：setPackageHeadDataSize(0)，只发包头。
//   ModuleControl -> msg::MODULE_CONTROL   (0x00018028)，载荷是【singular】
//                    CarlifeModuleStatus{moduleID=1(int32), statusID=2(int32)}
//                    → 线上字节 [08 01 10 02]（module=1, status=2）。
//                    【不是】ModuleStatusList（那是 MODULE_STATUS 0x00010026 的格式，
//                    会把每条状态包在 field2 的 length-delimited 里）。
// 返回 false = 这不是命令型控制（触控/硬键/多点触控等仍走它们原有的分支，行为不变）。
struct ControlCommand {
  uint32_t service_type = 0;   // 走 CMD 通道（与 Session::sendCmd 一致）
  std::vector<uint8_t> payload;
};

inline bool encodeControlCommand(const HostControl& control, ControlCommand* out) {
  if (!out) return false;
  switch (control.type) {
    case HostControl::Type::Foreground:
      out->service_type = msg::GO_TO_FOREGROUND;
      out->payload.clear();
      return true;
    case HostControl::Type::ModuleControl: {
      if (control.module_id < HostControl::kModuleIdMin ||
          control.module_id > HostControl::kModuleIdMax || control.status_id < 0) return false;
      out->service_type = msg::MODULE_CONTROL;
      PbWriter w;
      w.fieldInt32(1, control.module_id);   // CarlifeModuleStatusProto.proto: moduleID = 1
      w.fieldInt32(2, control.status_id);   //                              statusID = 2
      out->payload = w.data();
      return true;
    }
    default:
      return false;
  }
}

// 宿主机 -> 会话：需要【车机上报给手机】的车况（AUDIT 3.6 / 5.1 #8）。
// 这些方向都是 HU->MD（CommonParams 里 CAR_GPS/CAR_VELOCITY/… 都是 0x00018000 段），
// 所以必须由宿主机提供数据，而不是我们解析。
struct VehicleReport {
  // 每一项独立置位：没有数据的项不发送（proto 里是 required，宁可整条不发也不发假值）。
  bool has_speed = false;
  int32_t speed_kph = 0;
  bool has_gear = false;
  int32_t gear = 0;  // 见 service_types.h 的 gear::*
  bool has_oil = false;
  int32_t oil_level = 0, oil_range = 0;
  bool oil_low_warning = false;
  bool has_gps = false;
  int32_t latitude_e6 = 0, longitude_e6 = 0, altitude_m = 0;
  uint32_t gps_speed = 0, gps_heading = 0;
  uint32_t antenna_state = 0, signal_quality = 0, gps_fix = 0;
  uint32_t sats_used = 0, sats_visible = 0;
  int32_t north_speed = 0, east_speed = 0, vert_speed = 0;
  bool has_gyroscope = false;
  double gyro_x = 0, gyro_y = 0, gyro_z = 0;
  bool has_acceleration = false;
  double acc_x = 0, acc_y = 0, acc_z = 0;
};

// 会话 -> 宿主机。全部回调都在会话线程内同步调用，实现必须非阻塞且自身有界。
class HostSink {
 public:
  virtual ~HostSink() = default;

  virtual void onState(State state, const std::string& detail) {
    (void)state;
    (void)detail;
  }
  // 已发出的显示协商（VIDEO_ENCODER_INIT）。后续 Annex-B 的尺寸按此解释。
  virtual void onVideoConfig(const VideoInfo& info) { (void)info; }
  // 一个 H.264 Annex-B 访问单元；由宿主机判断关键帧与参数集。
  virtual void onVideoAnnexB(const uint8_t* data, std::size_t len) {
    (void)data;
    (void)len;
  }
  virtual void onVideoEnded() {}
  // 下行音频 channel 用 carlife::ch::*（AUDIO=Media、TTS=导航音、VR=语音播放）。
  virtual void onAudioInit(int32_t channel, int sample_rate, int channels, int sample_format) {
    (void)channel;
    (void)sample_rate;
    (void)channels;
    (void)sample_format;
  }
  virtual void onAudioPcm(int32_t channel, const uint8_t* data, std::size_t len) {
    (void)channel;
    (void)data;
    (void)len;
  }
  virtual void onAudioEnd(int32_t channel) { (void)channel; }
  virtual void onModuleStatus(const std::string& detail) { (void)detail; }
  // Phone application lifecycle/request notifications; not connection state.
  // Host decides how to change its display. Never echo these as touch.
  virtual void onAppEvent(uint32_t service_type) { (void)service_type; }
  // 播放进度条（CarLife MEDIA_PROGRESS_BAR = 0x00010036，MD->HU）。
  // 依据 CarlifeMediaProgressBarProto.proto：progressBar 为 0..100 的 int32。
  virtual void onMediaProgress(int32_t progress) { (void)progress; }

  // ── 视频：帧率动态调整（AUDIT 3.4 / 5.1 #11）──
  // type 是 VIDEO_ENCODER_FRAME_RATE_CHANGE / _DONE / PAUSE / RESET 的 serviceType。
  virtual void onVideoControl(uint32_t service_type, int32_t frame_rate) {
    (void)service_type;
    (void)frame_rate;
  }

  // ── 音频仲裁（AUDIT 3.2 / 8.5）──
  // 参考实现里 TTS 起来时 AudioTrackManagerDualNormal.setVolume(maxVolume/3) 压低音乐轨，
  // TTS 结束恢复 maxVolume；同时走 Android AudioFocus（MSG_CMD_AUDIO_FOCUS_GAIN / _LOSS*）。
  // 我们把它显式化成事件：gained=true 表示 TTS/导航拿到焦点（压低媒体）。
  virtual void onAudioFocus(bool gained, int32_t channel) {
    (void)gained;
    (void)channel;
  }

  // ── 元数据：标题/歌手/专辑/时长/封面（AUDIT 3.7 / 5.1 #6）──
  virtual void onMediaInfo(const pb::MediaInfo& info) { (void)info; }
  // ── 导航逐向与辅助引导（AUDIT 3.6 / 5.1 #7）──
  virtual void onNaviNextTurn(const pb::NaviNextTurnInfo& info) { (void)info; }
  virtual void onNaviAssistantGuide(const pb::NaviAssistantGuideInfo& info) { (void)info; }

  // ── 手机侧来的触摸板 / 手势 / 多点触控设备事件（AUDIT 3.5 / 5.1 #4 #5）──
  // kind 用 HostPhoneInputKind。触摸板：delta_x/delta_y/scale 有意义。
  enum class PhoneInputKind : uint8_t { TouchPadDown, TouchPadMove, TouchPadUp, TouchPadPinch };
  virtual void onPhoneInput(PhoneInputKind kind, int32_t delta_x, int32_t delta_y, float value) {
    (void)kind;
    (void)delta_x;
    (void)delta_y;
    (void)value;
  }
  // 手机侧的“反控”多点触控事件（MSG_TOUCH_ACTION_3，定长格式，不走 protobuf）。
  virtual void onPhoneTouchAction3(const pb::TouchAction3& event) { (void)event; }
  // 手机上报的档位（C++ 库的 MSG_CMD_GEAR_INFO = 0x00010029，方向是 MD->HU，
  // 与 CAR_GEAR(0x00018029, HU->MD) 相反）。
  virtual void onGearFromPhone(int32_t gear) { (void)gear; }

  // ── 电话 / HFP（AUDIT 3.7 / 5.1 #13 #14）──
  // 手机让我们做的 HFP 动作（拨号/接听/挂断/拒接/DTMF/静音）。
  virtual void onHfpRequest(const pb::BTHfpRequest& request) { (void)request; }
  // 手机上报的通话状态（TEL_STATE_* 与 BT_HFP_CALL_STATUS_COVER）。
  virtual void onTelephony(uint32_t service_type, int32_t state, const std::string& number,
                           const std::string& name) {
    (void)service_type;
    (void)state;
    (void)number;
    (void)name;
  }
  virtual void onHfpStatusRequest(const pb::BTHfpStatusRequest& request) { (void)request; }
  virtual void onContacts(const pb::ContactsList& list) { (void)list; }
  virtual void onCallRecords(const pb::CallRecordsList& list) { (void)list; }

  // ── 语音（AUDIT 3.3）──
  virtual void onVoiceControl(const pb::VoiceControlRequest& request) { (void)request; }
  // 麦克风录音控制：WAKEUP_START / END / RECOG_START / PREPARE。
  virtual void onMicRecord(uint32_t service_type) { (void)service_type; }
  // Optional synchronous cue hook. Return true if the cue completed, false if
  // unavailable/failed. Both cases finish the cue phase with PREPARE_DONE, as in
  // VRModule.kt onFinish/onError; neither asserts microphone readiness.
  // Must be bounded and non-blocking. An asynchronous player needs its own
  // completion mechanism, not a callback that returns before playback finishes.
  virtual bool prepareMicrophone() { return false; }

  // ── 车控（AUDIT 3.6 / 5.1 #18）──
  virtual void onVehicleControl(const pb::VehicleControl& control) { (void)control; }

  // ── 激活（AUDIT 3.1 / 5.1 #10）──
  virtual void onActivationRequest(const pb::ActiveRequest& request) { (void)request; }

  // ── 内容加密（AUDIT 3.1 / 5.4）──
  // state 用 EncryptionState；detail 是人类可读的一行（进管理页面）。
  // encrypted/decrypt_failures 是【至今累计】的载荷加解密计数，由 Session 提供
  // （适配器自己看不到 Session 的内部计数器，所以必须由这里带过来）。
  virtual void onEncryption(uint8_t state, const std::string& detail, uint64_t encrypted,
                            uint64_t decrypt_failures) {
    (void)state;
    (void)detail;
    (void)encrypted;
    (void)decrypt_failures;
  }
  // 车辆数据订阅状态变化（D 项）：让宿主机能看到“手机要求了哪些车况”。
  virtual void onCarDataSubscribe(uint32_t service_type) { (void)service_type; }

  // ── 文件传输 / OTA（AUDIT 3.8 / 5.1 #17）──
  // service_type 是 DATA_*_TRANSFER_* / SEND_* 之一；total 只在 BEGIN 时有效。
  virtual void onFileTransfer(uint32_t service_type, int64_t total_bytes, std::size_t chunk_bytes) {
    (void)service_type;
    (void)total_bytes;
    (void)chunk_bytes;
  }

  // ── 时间同步 ──
  virtual void onTimeSync(int32_t timestamp) { (void)timestamp; }

  // 事件泵与退出请求（本地 UI 需要；Core 接入为空实现）。
  virtual void pump() {}
  virtual bool quitRequested() const { return false; }

  // 取一条待回传的控制；没有则返回 false。
  virtual bool takeControl(HostControl& out) {
    (void)out;
    return false;
  }
  // 上行麦克风：向 dst 填入最多 capacity_frames 个单声道 PCM16 帧。
  // 没有采集源时返回 false，会话侧只记录请求计数。
  virtual bool microphoneAvailable() const { return false; }
  virtual bool takeMicrophone(int16_t* dst, std::size_t capacity_frames, std::size_t& frames_out) {
    (void)dst;
    (void)capacity_frames;
    (void)frames_out;
    return false;
  }
  // 车况上报：有数据就填进 out 并返回 true，会话侧按置位项发送。
  // 没有数据返回 false（不发空值）。
  virtual bool takeVehicleReport(VehicleReport& out) {
    (void)out;
    return false;
  }

  virtual bool saveSnapshot(const std::string& path) {
    (void)path;
    return false;
  }
  virtual uint64_t decodedFrames() const { return 0; }
  virtual uint64_t decodedAudioBytes() const { return 0; }
};

}  // namespace carlife
