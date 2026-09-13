// 把无线 CarLife+ 接入 Core。
//
// 依据（都是现成项目，不自行发明）：
//   * carlife-vehicle-lib（百度官方 V0.15 C++ 车机库）：控制/视频/音频/触控是独立
//     通道模块，库通过类型化结构体把呈现交给宿主机 —— 本类即那个宿主机；
//   * carlinkit-cxx：把解码源（ck::DecoderSource）与会话解耦，显示后端可替换；
//   * 本仓 Input/WirelessCarPlay/src/catplay_media_client.cpp：同类形态的既有先例 ——
//     一个 InputAdapter 把外部媒体灌进 SessionCore/RealMediaStore，并注册控制回环。
//
// 因此本类同时是：
//   * mvp::InputAdapter  —— 向 SessionCore 注册源、上报连接状态、接收控制；
//   * carlife::HostSink  —— 从 CarLife 会话取 H.264 Annex-B 与 PCM。
// 视频不做解码：Annex-B 原样进入 Core 的有界媒体面，由浏览器侧解码（与 CarPlay 同路）。
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "carlife/host_sink.h"
#include "carlife/pairing_agent.h"
#include "carlife/session.h"
#include "core/session_core.hpp"
#include "wirelesscarplay/real_media_store.hpp"

namespace carlife {

// 管理页面与诊断用的有界状态快照。
// 【设计选择】Core 的 SessionSnapshot 里没有“导航逐向 / 车控 / 激活 / 文件传输”这些分组
// （它们是 CarLife/CarPlay 各自的业务面，不是跨输入共用的状态），所以这些项放在这里，
// 由管理页面直接从 /api/state 的 carlife 段渲染 —— 与既有的 media_progress/bt_phase
// 完全同一种做法（已在真机验证过的那条路）。
struct CarLifeInputStatus {
  bool running{};
  bool connected{};
  State state{State::Idle};
  std::array<char, 32> state_name{};
  std::array<char, 96> detail{};
  std::array<char, 64> phone_ip{};
  uint32_t video_width{}, video_height{}, video_rate{};
  uint64_t frames_received{}, frames_forwarded{}, keyframes{}, video_configs{};
  uint64_t video_bytes{}, media_bytes{}, tts_bytes{}, audio_frames{};
  uint64_t controls_sent{}, controls_dropped{}, mic_requests{}, mic_frames{};
  // A2：手机上报的播放进度（0..100，CarLife MEDIA_PROGRESS_BAR）。
  int32_t media_progress{};
  // A1：当前是否处于“导航/ TTS 压低媒体音”状态（对应参考实现的 VolumReduceRatio）。
  bool media_ducked{};
  // 蓝牙引导阶段（无线拓扑的第一步），便于管理页面展示卡在哪一步。
  std::array<char, 32> bt_phase{};
  std::array<char, 96> bt_note{};

  // ── 本轮新增：逐项接口的落地证据（页面直接显示，也是验证脚本的判据）──
  uint64_t media_info_count{},
      navi_count{},              // 导航逐向 + 辅助引导
      phone_input_count{},       // 手机侧触摸板/手势
      multi_touch_count{},       // 多点触控（TOUCH_ACTION_3）
      hfp_count{}, vehicle_control_count{}, voice_control_count{}, file_transfer_count{},
      activation_count{}, car_data_subscribe_count{}, vehicle_report_sent{}, encrypted_messages{},
      decrypt_failures{};
  // 元数据（最近一条 MEDIA_INFO）
  std::array<char, 192> media_song{}, media_artist{}, media_album{};
  uint32_t media_duration_ms{}, media_position_ms{}, media_cover_bytes{};
  bool media_playing{};
  // 导航逐向
  int32_t navi_action{}, navi_next_turn{}, navi_remain_m{}, navi_total_m{};
  std::array<char, 96> navi_road{};
  // 电话 / HFP
  uint8_t call_state{};
  std::array<char, 192> call_caller{};
  std::array<char, 24> call_number{};
  int32_t last_hfp_command{-1};
  uint32_t last_hfp_dtmf{};
  // 车控 / 语音
  int32_t last_vehicle_ctrl_type{-1}, last_vehicle_ctrl_id{-1};
  int32_t last_voice_command{-1};
  // 激活 / 加密 / 文件传输
  uint8_t activation_state{};
  uint8_t encryption_state{};
  uint8_t file_transfer_active{};
  uint32_t file_transfer_bytes{}, file_transfer_total{};
  uint8_t ota_state{};
  // 手机侧档位与时间同步
  int32_t phone_gear{-1};
  int64_t last_time_sync{};
  // 多点触控最近一次的点数
  uint8_t last_touch_points{};
};

// 车况上报源（AUDIT 3.6 / 5.1 #8）。
// 这些数据在真车上来自 CAN/OBD，本机（Orange Pi Zero2W）没有这些传感器，
// 所以只能由宿主机注入。没注入就不上报（takeVehicleReport 返回 false）——
// 宁可不上报，也不编造一个假的 GPS 位置往手机上发。
using VehicleReportSource = std::function<bool(VehicleReport& out)>;

// 上行麦克风源（AUDIT 3.3 的第二条上行通道：CarLife VR）。
// 【契约（注入方必须遵守）】
//   * dst 至少有 capacity 个 int16_t 的空间；实现最多写 capacity 个样本，绝不越界；
//   * 实际写入的样本数写进 out_frames（单声道 PCM16 帧数）；
//   * 返回 false 表示这一次没有数据（此时 out_frames 会被忽略）；
//   * 实现必须【有界且非阻塞】：它在会话收包线程里被同步调用；
//   * 抛出的异常由适配器拦下并按“没数据”处理。
// 没注入源时适配器返回 false —— 不自己合成 PCM（宁可手机等不到，也不发假声音）。
using MicrophoneSource =
    std::function<bool(int16_t* dst, std::size_t capacity, std::size_t& out_frames)>;

class CarLifeInputAdapter final : public mvp::InputAdapter, public HostSink {
 public:
  CarLifeInputAdapter(mvp::SessionCore& core, mvp::RealMediaStore& media, SessionConfig config,
                      std::string source_id = "carlife-hu");
  ~CarLifeInputAdapter() override;

  // ---- mvp::InputAdapter ----
  std::string_view id() const override { return source_id_; }
  mvp::InputSourceKind kind() const override { return mvp::InputSourceKind::External; }
  void start() override;
  void stop() override;
  void on_control(const mvp::ControlEvent& event) override;

  // 连接方式，必须在 start() 之前设置；两者都不设置则默认 --discover 等价行为不可用，
  // 即直接报错退出（与 carlife-hu 的用法保持一致）。
  void connectToPhone(std::string phone_ip) { phone_ip_ = std::move(phone_ip); }
  void listenOnHuPorts() { listen_mode_ = true; }

  // 无线 CarLife 的第一步：蓝牙 SPP 引导。启用后 run() 会先尽力广播、监听 RFCOMM、
  // 等手机连上并跑完 4 步握手拿到手机 IP，再建 7 条 TCP 通道。
  // 与独立工具的 --bt-advertise/--bt-rfcomm 等价，只是接进了 Core 适配器。
  void enableBluetoothBootstrap(int rfcomm_channel, std::string wifi_device_name);

  // 注入车况上报源（见 VehicleReportSource 注释）。必须在 start() 之前设置。
  void setVehicleReportSource(VehicleReportSource source) {
    vehicle_source_ = std::move(source);
  }
  // 注入上行麦克风源（见 MicrophoneSource 注释）。必须在 start() 之前设置：
  // 运行中换源会让收包线程读到半个 std::function（与 setVehicleReportSource 同理）。
  void setMicrophoneSource(MicrophoneSource source) {
    microphone_source_ = std::move(source);
  }
  // Configure before start; provider must outlive the running session.
  void setVerifySeam(VerifySeam* seam) { verify_seam_ = seam; }
  bool microphoneAvailable() const override { return bool(microphone_source_); }
  // 麦克风“准备”缝（MSG_CMD_MIC_RECORD_PREPARE_START = 0x00010071 → DONE 0x18072）。
  // 参考实现里这一步是播一声提示音，播完/出错后才回 DONE（VRModule.kt），本身不校验硬件。
  // 【契约（注入方必须遵守）】
  //   * 回调必须【非阻塞、有界】：它在会话收包线程里被同步调用；
  //   * 提示音完成返回 true，不可用/失败返回 false；两者均由 Session 结束准备阶段；
  //   * 抛出的异常由适配器拦下并按 false 处理；
  //   * 没注入回调时 prepareMicrophone() 为 false，不冒充有提示音或麦克风；
  // 必须在 start() 之前设置（与 setMicrophoneSource 同理：运行中换会让收包线程读到半个
  // std::function）。
  void setMicPrepareCallback(std::function<bool()> prepare) {
    mic_prepare_ = std::move(prepare);
  }
  // 多点触控开关（对应能力位 MULTI_TOUCH，也是 RemoteControlManager 的分支条件）。
  // 开启时所有触控走 MSG_TOUCH_ACTION_3 定长格式；关闭时回退到旧的
  // TOUCH_ACTION_DOWN/UP/MOVE + TouchSinglePoint（与官方 C++ 车机库一致）。
  // 必须在 start() 之前设置：它在 run() 里被转交给 Session。
  void setMultiTouch(bool on) { multi_touch_ = on; }

  // ── 命令型回传（MSG_CMD_GO_TO_FOREGROUND 0x00018025 / MSG_CMD_MODULE_CONTROL 0x00018028）──
  // 把一条命令放进【有界控制队列】，由会话线程在下一次 poll 时用 sendCmd/sendOn
  // （也就是既有的加密出口）编码发出。本函数绝不在调用线程里碰 socket：
  // HTTP/Core 线程直接发消息会与会话线程并发写同一条 TCP 连接。
  //
  // 【返回 true 只表示“已入队”】不表示手机收到，更不表示设备真的做了动作；
  // 本机没有任何途径确认后者，代码不冒充成功。
  // 下列情形返回 false，并计入 controls_dropped：
  //   * 适配器未连接（没有会话在跑，队列没人消费）；
  //   * module_id 不在 HostControl::kModuleIdMin..kModuleIdMax；
  //   * status_id < 0（本轮只允许非负 int32，不编造“合法状态枚举”）；
  //   * 队列已满 —— 此时【既有控制一条都不丢】（整条拒绝，绝不挤掉在路上的控制）。
  bool requestForeground();
  bool requestModuleControl(int32_t module_id, int32_t status_id);
  // Configure before start. Bounded synchronous sink for the future car/convert
  // layer; receiving an event does not claim an output action.
  void setAppEventHandler(std::function<void(uint32_t)> handler) { app_event_handler_ = std::move(handler); }
  void onAppEvent(uint32_t service_type) override;

  CarLifeInputStatus status() const;

 private:
  void run();
  // 蓝牙 SPP 引导：成功时填入 phone_ip。
  bool run_bluetooth_bootstrap(std::string& phone_ip, std::string& error);
  bool take_control_locked(HostControl& out);
  void push_control_locked(const HostControl& control);
  // 批量入队：要么【整批】进队，要么一个都不进（全部计入 controls_dropped_）。
  // 【与 push_control_locked 的区别】后者在队列满时会清空队列腾位置（那适用于“单条触控
  // 宁可丢旧也要实时”的场景）；而旋钮脉冲串 / DTMF 数字串必须是原子的 —— 契约要求
  // 没地方就保持既有控制不动，否则手机会收到半串数字或停在错误位置。
  bool enqueue_batch_locked(const HostControl* items, std::size_t count);
  void clear_bounded_locked();
  void ensure_video_stream_locked();
  void forward_video_annexb(const uint8_t* data, std::size_t len);
  void ensure_session_locked();

  // ---- carlife::HostSink ----
  void onState(State state, const std::string& detail) override;
  void onVideoConfig(const VideoInfo& info) override;
  void onVideoAnnexB(const uint8_t* data, std::size_t len) override;
  void onVideoEnded() override;
  void onAudioInit(int32_t channel, int sample_rate, int channels, int sample_format) override;
  void onAudioPcm(int32_t channel, const uint8_t* data, std::size_t len) override;
  void onAudioEnd(int32_t channel) override;
  void onModuleStatus(const std::string& detail) override;
  // A2：播放进度条（CarLife MEDIA_PROGRESS_BAR = 0x00010036）。
  void onMediaProgress(int32_t progress) override;
  // ── A..U：本轮新增的全部回调 ──
  void onMediaInfo(const pb::MediaInfo& info) override;
  void onNaviNextTurn(const pb::NaviNextTurnInfo& info) override;
  void onNaviAssistantGuide(const pb::NaviAssistantGuideInfo& info) override;
  void onVideoControl(uint32_t service_type, int32_t frame_rate) override;
  void onAudioFocus(bool gained, int32_t channel) override;
  void onPhoneInput(PhoneInputKind kind, int32_t delta_x, int32_t delta_y, float value) override;
  void onPhoneTouchAction3(const pb::TouchAction3& event) override;
  void onGearFromPhone(int32_t gear) override;
  void onHfpRequest(const pb::BTHfpRequest& request) override;
  void onTelephony(uint32_t service_type, int32_t state, const std::string& number,
                   const std::string& name) override;
  void onHfpStatusRequest(const pb::BTHfpStatusRequest& request) override;
  void onVoiceControl(const pb::VoiceControlRequest& request) override;
  void onMicRecord(uint32_t service_type) override;
  void onVehicleControl(const pb::VehicleControl& control) override;
  void onActivationRequest(const pb::ActiveRequest& request) override;
  void onEncryption(uint8_t state, const std::string& detail, uint64_t encrypted,
                    uint64_t decrypt_failures) override;
  void onCarDataSubscribe(uint32_t service_type) override;
  void onFileTransfer(uint32_t service_type, int64_t total_bytes, std::size_t chunk_bytes) override;
  void onTimeSync(int32_t timestamp) override;
  // 上行麦克风（HostSink 缝）：把注入的 MicrophoneSource 接到 CarLife 会话的
  // VR 上行通道。没注入源、或源给出无效结果时返回 false 且 frames_out=0。
  bool takeMicrophone(int16_t* dst, std::size_t capacity_frames, std::size_t& frames_out) override;
  // 麦克风“准备”：把注入的回调同步转成 HostSink 的答复（见 setMicPrepareCallback）。
  bool prepareMicrophone() override;
  bool takeVehicleReport(VehicleReport& out) override;
  bool takeControl(HostControl& out) override;

  static constexpr std::size_t kControlCapacity = 32;
  // 一次旋钮事件最多产生的按键数（超过就整批拒绝，绝不发半个脉冲串）。
  static constexpr int32_t kMaxKnobRepeat = 32;
  // DTMF 数字串上限。core 的 ControlEvent::dtmf 是 char[16]，所以最多 15 个字符 + NUL；
  // 下面的 static_assert 把这个关系钉死，改了任何一边都会编译失败。
  static constexpr std::size_t kMaxDtmfDigits = 15;
  static_assert(sizeof(mvp::ControlEvent::dtmf) == kMaxDtmfDigits + 1,
                "ControlEvent::dtmf 的容量变了：DTMF 上限必须同步修改");
  static_assert(kMaxKnobRepeat <= static_cast<int32_t>(kControlCapacity),
                "一次旋钮脉冲串必须能整批放进有界队列");
  static constexpr std::size_t kMaxConfigBytes = 4096;
  static constexpr uint64_t kSessionEpochBase = 0xC0A11FE0000ULL;

  mvp::SessionCore& core_;
  mvp::RealMediaStore& media_;
  SessionConfig config_;
  std::string source_id_;
  std::string phone_ip_;
  bool listen_mode_{};
  // 蓝牙引导配置
  bool bt_enabled_{};
  int bt_channel_{1};
  // 等待手机经 SPP 连入的超时（毫秒）。
  // 原来写 30000（30 秒）：超时后 run_bluetooth_bootstrap 判定失败 → run() 结束
  // → running_ 置 false → 适配器停掉，表现为 /api/state 里
  //   detail="no phone connected over SPP within 30s"、running=false。
  // 但车机的语义是【无限期等待手机】：用户从打开蓝牙到手机上点连接经常远超 30 秒。
  // 因此改为 7 天量级，实际等同“一直等”，不再自行放弃。
  int bt_timeout_ms_{7 * 24 * 60 * 60 * 1000};
  std::string bt_wifi_name_;
  // 配对 agent：作为成员而不是局部变量 —— 否则函数返回即注销，
  // 手机在会话中再次配对就会失败。生命周期与适配器一致。
  PairingAgent pairing_agent_;
  std::string pair_pin_{"0000"};
  // HFP 通话通道的 fd：SLC 建立后保持打开（车机就应保持免提连接）。
  // 官方实现里有 MSG_CMD_BT_HFP_CONNECTION（向手机上报 HFP 连接状态），
  // 一配对完就断开 HFP 会被当成异常。
  int hfp_call_fd_{-1};
  std::array<char, 32> bt_phase_{};
  std::array<char, 96> bt_note_{};
  // 多点触控开关（见 setMultiTouch）。默认开：我们本来就把 MULTI_TOUCH 能力位报 1。
  bool multi_touch_{true};

  std::atomic<bool> running_{false};
  // 指向 run() 里当前活着的会话，供 stop() 叫停（否则 join 会永久阻塞）。
  std::atomic<Session*> active_session_{nullptr};
  std::thread thread_;
  mutable std::mutex mutex_;

  // 媒体面状态
  uint64_t session_id_{};
  uint32_t video_stream_id_{1};
  uint32_t video_width_{};
  uint32_t video_height_{};
  uint32_t video_rate_{};
  bool video_stream_open_{};
  std::array<uint8_t, kMaxConfigBytes> config_bytes_{};
  std::size_t config_size_{};
  uint64_t video_sequence_{};
  std::array<uint64_t, 4> audio_sequence_{};
  std::array<bool, 4> audio_open_{};

  // 控制回环（Core -> 手机），有界
  std::array<HostControl, kControlCapacity> controls_{};
  std::size_t control_begin_{}, control_count_{};

  // ── 向 Core 汇报的累加状态副本 ──
  // 为什么留副本：SessionCore 的新 setter 是“整块覆盖 + 变化检测”语义（返回是否变化），
  // 而 Core 没有单块 getter（快照太贵）。所以适配器自己持有最后一份，改一个字段再整块送。
  mvp::MediaInfo core_media_{};
  mvp::DisplayConfig core_display_{};
  mvp::InteractionState core_input_{};
  mvp::AudioState core_audio_{};
  mvp::VehicleState core_vehicle_{};
  mvp::TelephonyState core_telephony_{};
  mvp::LinkState core_link_{};
  // 导航逐向（AUDIT 5.1 #7）：契约里的 NavigationState。
  // maneuver_code 是【原始 action 码】：参考树里查不到码表（全树唯一的 maneuver 枚举
  // 属于 Android Auto 的 aa-proxy-rs），所以 deliberate 地不编映射表。
  mvp::NavigationState core_nav_{};

  // 状态快照计数
  std::atomic<uint64_t> frames_received_{0}, frames_forwarded_{0}, keyframes_{0}, video_configs_{0};
  std::atomic<uint64_t> video_bytes_{0}, media_bytes_{0}, tts_bytes_{0}, audio_frames_{0};
  std::atomic<uint64_t> controls_sent_{0}, controls_dropped_{0};
  std::atomic<uint64_t> mic_requests_{0}, mic_frames_{0};
  // A2/A1：播放进度与压低状态（由 Session 的 HostSink 回调写入）。
  std::atomic<int32_t> media_progress_{0};
  std::atomic<bool> media_ducked_{false};
  // ── 本轮新增：各类接口计数（全部 atomic，读取在同级的 status() 里）──
  std::atomic<uint64_t> media_info_count_{0}, navi_count_{0}, phone_input_count_{0};
  std::atomic<uint64_t> multi_touch_count_{0}, hfp_count_{0}, vehicle_control_count_{0};
  std::atomic<uint64_t> voice_control_count_{0}, file_transfer_count_{0}, activation_count_{0};
  std::atomic<uint64_t> car_data_subscribe_count_{0}, vehicle_report_sent_{0};
  std::atomic<uint64_t> encrypted_messages_{0}, decrypt_failures_{0};
  // ── 本轮新增：有界文本/数值状态（都由 mutex_ 保护）──
  std::array<char, 192> media_song_{}, media_artist_{}, media_album_{}, call_caller_{};
  std::array<char, 24> call_number_{};
  std::array<char, 96> navi_road_{};
  uint32_t media_duration_ms_{}, media_position_ms_{}, media_cover_bytes_{};
  bool media_playing_{};
  int32_t navi_action_{}, navi_next_turn_{}, navi_remain_m_{}, navi_total_m_{};
  uint8_t call_state_{};
  int32_t last_hfp_command_{-1}, last_vehicle_ctrl_type_{-1}, last_vehicle_ctrl_id_{-1};
  int32_t last_voice_command_{-1};
  uint32_t last_hfp_dtmf_{};
  uint8_t activation_state_{}, encryption_state_{}, file_transfer_active_{}, ota_state_{};
  uint32_t file_transfer_bytes_{}, file_transfer_total_{};
  int32_t phone_gear_{-1};
  int64_t last_time_sync_{};
  uint8_t last_touch_points_{};
  uint32_t artwork_revision_{};
  VehicleReportSource vehicle_source_;
  // 上行麦克风源（见 MicrophoneSource 注释）。start() 之前设置。
  MicrophoneSource microphone_source_;
  // 麦克风准备回调（见 setMicPrepareCallback）。空 = 没有这个接缝 → false。
  std::function<bool()> mic_prepare_;
  VerifySeam* verify_seam_{};
  std::function<void(uint32_t)> app_event_handler_;
  std::atomic<uint32_t> state_code_{0};
  std::atomic<bool> connected_{false};
  std::array<char, 32> state_name_{};
  std::array<char, 96> detail_{};
  std::array<char, 64> phone_ip_text_{};
};

// 由 Core 侧调用：构造并启动一个 CarLife 输入（源 id = "carlife-hu"）。
// 返回的对象由调用方持有；停止与销毁顺序必须是 stop() -> 析构。
const char* carlife_audio_type_name(uint32_t audio_type);

}  // namespace carlife
