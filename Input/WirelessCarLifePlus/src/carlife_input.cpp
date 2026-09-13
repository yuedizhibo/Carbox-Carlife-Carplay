#include "carlife/carlife_input.h"

#include <unistd.h>

#include "carlife/bt_link.h"
#include "carlife/bt_client_profile.h"
#include "carlife/hfp_profile.h"
#include "carlife/hfp_slc.h"
#include "carlife/pairing_agent.h"
#include "carlife/spp_profile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
// std::exception：麦克风准备回调（prepareMicrophone）与麦克风源都要拦异常。
#include <exception>
// 必须显式包含：本文件用到 std::cout。之前误以为已有（mvp.log 里那些 [bt] 日志
// 实际来自 bt_link.cpp），本地校验与板上编译都因此报
// “'cout' is not a member of 'std'”。
#include <iostream>
#include <span>

#include "carlife/keycode_map.h"
#include "carlife/service_types.h"
#include "wirelesscarplay/catplay_media_protocol.hpp"

namespace carlife {
namespace {

uint64_t nowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// 从定长数组里【有界】取 C 字符串：只在 cap 个字节内找 NUL。
// 【为什么必须有这个函数】Core 的 ControlEvent::key / dtmf 是 std::array<char,N>，
// 不保证结尾有 NUL（调用方忘了置 \0 很正常）。直接 std::string(arr.data()) 或
// carlife_keycode_for(arr.data()) 会一路读到数组之外 —— 那是越界读，读到什么全凭运气。
// 返回 false = 这条串不完整/不可信，调用方必须整条拒绝（不能猜）。
bool bounded_cstr(const char* data, std::size_t cap, std::string* out) {
  if (!data) return false;
  for (std::size_t i = 0; i < cap; ++i) {
    if (data[i] == '\0') {
      out->assign(data, i);
      return true;
    }
  }
  return false;
}

// 通道 -> 媒体面音频流编号（视频与音频的 stream id 是独立命名空间）。
constexpr uint32_t kAudioStreamId(::int32_t channel) {
  return channel == ch::TTS ? 2U : channel == ch::VR ? 3U : 1U;
}

// 通道 -> 媒体面 audio_type。取值域来自 Core/Convert 的 CPMF 定义
// （0 default / 1 alert / 2 media / 3 telephony / 4 speech-recognition / 5 compatibility）：
// Media 通道是媒体音，TTS 通道是导航播报（按 alert 处理），VR 通道是语音识别上行。
constexpr uint32_t kAudioType(uint32_t stream_id) {
  return stream_id == 2U ? 1U : stream_id == 3U ? 4U : 2U;
}

// ---- A1：导航压媒体（CarLife 侧）----
// 依据参考实现 apollo-DuerOS/.../audiotrackmanager/AudioTrackManagerDualNormal.java：
//   第 68 行  private final float mMusicAudioTrackVolumReduceRatio = 3;
//   第 376 行 setVolume(maxVolume / mMusicAudioTrackVolumReduceRatio)   // TTS 起来 → 压低音乐轨
//   第 390 行 setVolume(maxVolume)                                      // TTS 结束 → 恢复
// 即：比例固定 1/3，【只作用于音乐轨】，TTS 轨自身不受影响。
// 我们不做音量运算本身：只把“压低/恢复”交给媒体面，由浏览器按类型施加到 media 流上
//（Core/Web/assets/app.js:377  if (p.type === 2) setPlayerVolume(p, duckGain, r.p0)），
// 而 Media 通道恰好映射为 audio_type=2(media)，TTS 映射为 1(alert) —— 语义一致。
constexpr uint32_t kDuckGainPpm = 1000000u / 3u;  // maxVolume / 3 → 333333 ppm
constexpr uint32_t kFullGainPpm = 1000000u;       // 原始音量
constexpr uint32_t kDuckRampMs = 300;            // 渐变时长，避免切音量时的爆音

// 遍历 Annex-B 访问单元里的 NAL 单元。f(data, begin, end, nal_type)，区间含起始码。
template <typename F>
void for_each_nal(const uint8_t* p, std::size_t n, F&& f) {
  std::size_t i = 0;
  while (i + 3 <= n) {
    std::size_t prefix = 0;
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      prefix = 3;
    } else if (i + 4 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
      prefix = 4;
    }
    if (!prefix) {
      ++i;
      continue;
    }
    const std::size_t begin = i;
    const std::size_t payload = i + prefix;
    std::size_t next = n;
    for (std::size_t k = payload; k + 3 <= n; ++k) {
      if (p[k] == 0 && p[k + 1] == 0 &&
          (p[k + 2] == 1 || (k + 3 < n && p[k + 2] == 0 && p[k + 3] == 1))) {
        next = k;
        break;
      }
    }
    if (payload < n) f(p, begin, next, uint8_t(p[payload] & 0x1f));
    i = next;
  }
}

template <std::size_t N>
void copy_text(std::array<char, N>& dst, const std::string& src) {
  dst.fill(0);
  std::size_t n = std::min(dst.size() - 1, src.size());
  // 【必须按 UTF-8 边界截断】
  // 这些字段最终会进 /api/state 的 JSON；若在字符中间按字节截断，
  // 会产出一个【半个 UTF-8 字符】，使整份 JSON 非法 —— 前端与诊断脚本全部读不到状态。
  // 实测已发生两次：一次是 substr(0,160) 截断 sdptool 输出，
  // 一次是这里 95 字节截断一段中文（约 120 字节）—— 两者都让 JSON 停在同一位置报
  // “invalid continuation byte”。
  // 做法：回退到不落在续字节(10xxxxxx)上的位置。
  while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
  std::memcpy(dst.data(), src.data(), n);
}

}  // namespace

const char* carlife_audio_type_name(uint32_t audio_type) {
  switch (audio_type) {
    case 0: return "default";
    case 1: return "alert";
    case 2: return "media";
    case 3: return "telephony";
    case 4: return "speech-recognition";
    case 5: return "compatibility";
    default: return "unknown";
  }
}

CarLifeInputAdapter::CarLifeInputAdapter(mvp::SessionCore& core, mvp::RealMediaStore& media,
                                         SessionConfig config, std::string source_id)
    : core_(core), media_(media), config_(std::move(config)), source_id_(std::move(source_id)) {
  video_stream_id_ = 1;
  audio_sequence_.fill(0);
  audio_open_.fill(false);
  copy_text(state_name_, "Idle");
}

CarLifeInputAdapter::~CarLifeInputAdapter() {
  stop();
  core_.unregister_control_sink(source_id_);
}

void CarLifeInputAdapter::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;
  if (!phone_ip_.empty()) copy_text(phone_ip_text_, phone_ip_);
  // 同类防护：给 joinable 的 std::thread 赋值同样会调 std::terminate()。
  // 若上一轮 run() 已自行结束而未被 join，重新 start() 就会在这里崩。
  if (thread_.joinable()) thread_.join();
  // 只有在真正启动时才注册源与控制回环，否则未启用的输入不应出现在 /api/sources 里。
  core_.upsert_source(source_id_, mvp::InputSourceKind::External, false);
  core_.register_control_sink(source_id_, [this](const mvp::ControlEvent& event) { on_control(event); });
  thread_ = std::thread([this] { run(); });
}

void CarLifeInputAdapter::stop() {
  // 【切不可提前 return】旧写法是：
  //   bool expected=true; if(!running_.compare_exchange_strong(expected,false)) return;
  // 一旦 run() 自行结束并把 running_ 置为 false（蓝牙引导失败、会话超时都会走这条），
  // 之后的 stop() 就立即返回、【永不 join thread_】。
  // 而 ~std::thread 对 joinable 的线程直接调 std::terminate() ——
  // 实测崩溃栈正是 CarLifeInputAdapter::~CarLifeInputAdapter()（由 main 调用），
  // 并伴随终止处理器的回溯输出。这是 C++ 标准行为，不是偶发。
  // 正确做法（与 carlinkit-cxx 等实现的拆除一致）：无条件停线程并 join；
  // 后续清理全部保持幂等，重复调用无副作用。
  running_.store(false);
  if (Session* session = active_session_.load()) session->requestStop();
  if (thread_.joinable()) thread_.join();
  // 释放 HFP 通话通道（若已建立）
  if (hfp_call_fd_ >= 0) {
    ::close(hfp_call_fd_);
    hfp_call_fd_ = -1;
  }
  std::lock_guard lock(mutex_);
  clear_bounded_locked();
  if (session_id_) {
    media_.end_session(session_id_);
    session_id_ = 0;
  }
  media_.transport_closed();
  video_stream_open_ = false;
  config_size_ = 0;
  video_sequence_ = 0;
  audio_sequence_.fill(0);
  audio_open_.fill(false);
  connected_.store(false);
  core_.upsert_source(source_id_, mvp::InputSourceKind::External, false);
  core_.unregister_control_sink(source_id_);
}

void CarLifeInputAdapter::enableBluetoothBootstrap(int rfcomm_channel, std::string wifi_device_name) {
  bt_enabled_ = true;
  bt_channel_ = rfcomm_channel > 0 ? rfcomm_channel : 1;
  bt_wifi_name_ = std::move(wifi_device_name);
}

bool CarLifeInputAdapter::run_bluetooth_bootstrap(std::string& phone_ip, std::string& error) {
  std::string note;
  // 【先注册配对 agent】手机的配对请求（PIN / 数字比对）必须有人「同意」。
  // bt-agent -c NoInputNoOutput 不做这件事：bluez >= 5.55 下该能力注册即失效，
  // 且它的 handler 会直接拒绝（见 bluez/bluer #190、RPi-Distro/repo #291）。
  // 本类实现 Agent1：RequestConfirmation / RequestAuthorization / AuthorizeService
  // 一律同意，RequestPinCode 返回固定 PIN。
  {
    std::string agent_error;
    std::string text;
    if (!pairing_agent_.start(pair_pin_, &agent_error)) {
      text = "配对 agent 注册失败: " + agent_error;
    } else {
      text = "配对 agent 就绪（KeyboardDisplay，PIN=" + pair_pin_ + "）";
    }
    {
      std::lock_guard lock(mutex_);
      copy_text(bt_note_, text);
    }
    std::cout << "[bt] " << text << std::endl;
    // 不致命：没 agent 仍可尝试引导，但手机上可能无法完成配对。
  }

  BtLink::advertiseBlueZ(config_.btName, config_.verbose, &note);
  {
    std::lock_guard lock(mutex_);
    copy_text(bt_note_, note.empty() ? std::string("advertise: no-op") : note);
    copy_text(bt_phase_, "advertising");
  }

  // 用 BlueZ 的 ProfileManager1 注册 SPP：由 bluetoothd 监听该 RFCOMM 通道并对外发布
  // SDP 记录，手机连入时通过 Profile1.NewConnection 把 fd 递过来。
  // （BlueZ 5 里旧的 `sdptool add SP` 已经不生效，实测适配器 UUID 里始终没有 1101。）
  SppProfileServer profile;
  {
    // 【失败不致命】无线链路是“车机主动连手机”，并不要求我们在本机发布 SPP 服务端。
    // 而这里失败的概率不低：BlueZ 对“同一 UUID 已注册”的判定包含竞态 ——
    // 上一个进程被部署脚本 SIGKILL 后，其 profile 注册是异步清理的；
    // 新进程紧接着注册同 UUID 就会拿到 “UUID already registered”。
    // 实测曾因此停在 phase=advertising、running=false，把后面真正的工作全挡掉。
    std::string spp_err;
    if (!profile.start(bt_channel_, "zero2w CarLife SPP", &spp_err)) {
      std::cout << "[bt] SPP 服务端注册失败（不致命，继续）: " << spp_err << std::endl;
    }
  }

  // 【关键】同时注册 HFP HS（UUID 111E）—— 这才是 CarLife 识别车机的入口。
  // 依据百度官方车机端（Reference/carlife-vehicle-lib/CarLife-Android-Vehicle）源码：
  //   蓝牙模块全部围绕 HFP 实现 —— BtHfpProtocolHelper.btOOBInfo() 发 OOB 信息，
  //   状态机用 getHfpConnectionState() 判连接，配套 CarlifeBTHfpConnectionProto /
  //   CarlifeBTHfpIndicationProto / CarlifeBTIdentifyResultIndProto。
  // 板子此前只发布 SPP(1101) + AVRCP(110C/110E)，没有 111E，所以手机端 CarLife 的
  // 无线列表里根本看不到本机。
  // 用 111E（HF，车机角色）而非 111F（AG，手机角色）。
  const int hfp_channel = (bt_channel_ == 7) ? 8 : 7;   // BlueZ 对 HFP HS 的默认通道是 7
  HfpProfileServer hfp;
  std::string hfp_error;
  if (!hfp.start(hfp_channel, "zero2w CarLife HF", 0x007f, 0x0107, &hfp_error)) {
    // 同样不致命：HFP 只服务于“车机身份/通话”，无线引导不依赖它。
    std::cout << "[bt] HFP HS 注册失败（不致命，继续）: " << hfp_error << std::endl;
  }

  // 【无线主路径：照抄参考实现 WirlessAPProtocolTransport + WirlessConnector】
  //
  // 参考实现里 receiver/transport/ 下有三套无线方式：
  //   transport/wirless/WirlessAPProtocolTransport  ← 主流「无线连接」
  //   transport/instant/WirlessP2PProtocolTransport ← Wi-Fi Direct 支路（才用蓝牙交换凭据）
  //   transport/aoa / ios                          ← USB / iOS
  // 主路径的机制（逐条对应）：
  //   ① 车机监听 UDP 7999（BOARDCAST_WIFI_PORT）
  //   ② 收到手机的发现广播，用【报文源地址】当手机 IP
  //   ③ 车机主动 TCP 连手机 7240/8240/9240/9241/9242/9340/9440（WirlessConnector）
  //   ④ 跑会话握手（ConnectionEstablishHandler）
  //
  // 蓝牙（HFP/SPP/配对 agent）只用于车机身份与配对，【不参与数据链路】。
  // 我们前面在蓝牙上绕了好几轮，是因为把 Wi-Fi Direct 的支路当成了主路径。
  //
  // 这套能力本项目里本来就有：Discovery::waitForPhone(7999)（transport.cpp）
  // 与 port::MD_*(7240…9440) + Transport::connectPhone。
  std::cout << "[bt] 监听 UDP 7999，等待手机的 CarLife 发现广播（无线主路径）..." << std::endl;
  {
    std::lock_guard lock(mutex_);
    copy_text(bt_phase_, "wifi-discover");
    copy_text(bt_note_, "监听 UDP 7999 等手机发现广播（车机将主动连手机的 7240/8240/… 通道）");
  }
  DiscoveryResult dr;
  for (;;) {
    if (!running_.load()) {
      error = "stopped while waiting for the UDP 7999 discovery broadcast";
      return false;
    }
    if (Discovery::waitForPhone(7999, 3000, &dr) && !dr.phoneIp.empty()) break;
  }
  std::cout << "[bt] 收到手机发现广播：ip=" << dr.phoneIp << " port=" << dr.port << std::endl;
  phone_ip = dr.phoneIp;
  if (phone_ip.empty()) {
    error = "bt bringup returned an empty phone IP";
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    copy_text(bt_phase_, "ip-ready");
    copy_text(phone_ip_text_, phone_ip);
  }
  return true;
}

void CarLifeInputAdapter::run() {
  // 【对照参考实现补上：断开后自动重连】
  // GroupedProtocolTransport.onConnectionDetached()：
  //     if (context.isVersionSupport) context.postDelayed({ connect() }, 1000)
  // 即【会话结束后 1 秒重新发起连接】。
  // 我原来写成“引导一次 → 会话结束 → 适配器退出”，后果实测可见：
  //   一次会话结束后 UDP 7999 监听随之消失，手机/假手机下一次广播没人收。
  // 所以把“引导 + 会话”包在一个循环里，直到 stop() 把 running_ 置 false。
  for (;;) {
    if (!running_.load()) break;
    std::string error;
    std::string phone = phone_ip_;
    // 无线拓扑的第一步：拿手机 IP（热点模式下就是监听 UDP 7999 收发现广播）。
    if (bt_enabled_ && !listen_mode_ && phone.empty()) {
      if (!run_bluetooth_bootstrap(phone, error)) {
        std::lock_guard lock(mutex_);
        detail_.fill(0);
        copy_text(detail_, error);
        // 引导失败不直接退出：按参考实现等 1 秒后重试（例如还没等到手机广播）。
        if (running_.load()) {
          ::usleep(1000 * 1000);
          continue;
        }
        break;
      }
    }
    Session session(config_);
    session.setHost(this);
    session.setVerifySeam(verify_seam_);
    // 多点触控的线格式选择（ACTION_3 定长 vs 旧的 per-action protobuf）由能力位决定，
    // 与 RemoteControlManager.kt:61-69 的分支一致。
    session.setMultiTouch(multi_touch_);
    active_session_.store(&session);
    if (listen_mode_) {
      session.runAsServer(&error);
    } else if (!phone.empty()) {
      session.runWithPhone(phone, &error);
    } else {
      error = "CarLifeInputAdapter needs connectToPhone(), listenOnHuPorts() or enableBluetoothBootstrap()";
    }
    active_session_.store(nullptr);
    // 会话结束：断开源、清媒体面，避免旧帧泄漏到下一次会话。
    {
      std::lock_guard lock(mutex_);
      if (session_id_) {
        media_.end_session(session_id_);
        session_id_ = 0;
      }
      media_.transport_closed();
      video_stream_open_ = false;
      config_size_ = 0;
      video_sequence_ = 0;
      audio_sequence_.fill(0);
      audio_open_.fill(false);
    }
    connected_.store(false);
    core_.upsert_source(source_id_, mvp::InputSourceKind::External, false);
    {
      std::lock_guard lock(mutex_);
      detail_.fill(0);
      const std::string text = error.empty() ? std::string("session ended") : error;
      copy_text(detail_, text);
    }
    if (!running_.load()) break;
    // 参考实现：断开后 1 秒重连。
    ::usleep(1000 * 1000);
  }
  running_.store(false);
}

// ---------------- HostSink ----------------

void CarLifeInputAdapter::onState(State state, const std::string& detail) {
  state_code_.store(static_cast<uint32_t>(state));
  {
    std::lock_guard lock(mutex_);
    copy_text(state_name_, toString(state));
    copy_text(detail_, detail);
  }
  if (state == State::Established) {
    {
      std::lock_guard lock(mutex_);
      ensure_session_locked();
    }
    connected_.store(true);
    core_.upsert_source(source_id_, mvp::InputSourceKind::External, true);
  } else if (state == State::Failed || state == State::Closed) {
    connected_.store(false);
    core_.upsert_source(source_id_, mvp::InputSourceKind::External, false);
  }
}

void CarLifeInputAdapter::onVideoConfig(const VideoInfo& info) {
  std::lock_guard lock(mutex_);
  if (info.width > 0 && info.height > 0) {
    video_width_ = static_cast<uint32_t>(info.width);
    video_height_ = static_cast<uint32_t>(info.height);
    video_rate_ = static_cast<uint32_t>(info.frameRate);
  }
  ensure_session_locked();
}

void CarLifeInputAdapter::onVideoAnnexB(const uint8_t* data, std::size_t len) {
  if (!data || !len) return;
  frames_received_.fetch_add(1);
  video_bytes_.fetch_add(len);
  forward_video_annexb(data, len);
}

void CarLifeInputAdapter::onVideoEnded() {
  std::lock_guard lock(mutex_);
  if (video_stream_open_) {
    media_.end_video_stream(0);
    video_stream_open_ = false;
  }
  video_sequence_ = 0;
}

void CarLifeInputAdapter::onAudioInit(int32_t channel, int sample_rate, int channels,
                                      int sample_format) {
  // A1：TTS（导航播报）开始 → 压低媒体轨。放在前置，确保即使后续因格式/采样率不符而
  // 不建流，压低/恢复仍然成对发生（不会把音乐永久压低）。
  if (channel == ch::TTS) {
    media_.set_audio_gain(kDuckRampMs, kDuckGainPpm);
    media_ducked_.store(true);
    // 【只有一套压缩逻辑】A1 的 media_.set_audio_gain 是唯一执行点；
    // 这里只是把同一事实同步到 Core 的状态面（audio.nav_active / media_volume_ppm），
    // 供页面与审计读取，不再另做一次音量运算。
    core_.set_ducking(true, static_cast<int32_t>(kDuckGainPpm), static_cast<int32_t>(kFullGainPpm));
    core_audio_.nav_active = 1;
    core_audio_.media_volume_ppm = static_cast<int32_t>(kDuckGainPpm);
    core_audio_.nav_volume_ppm = static_cast<int32_t>(kFullGainPpm);
    core_audio_.duck_ratio_ppm = static_cast<int32_t>(kDuckGainPpm);
    core_audio_.duck_transition_ms = kDuckRampMs;
    core_audio_.active_role = mvp::AudioRole::Navigation;
    core_.set_audio_state(core_audio_);
    // 可观测证据（验证 A1 用）：压低媒体轨，比例 1/3（参考实现 mMusicAudioTrackVolumReduceRatio=3）。
    std::cout << "[A1] TTS 开始 -> 压低媒体音 " << kDuckGainPpm << "ppm (1/3)，渐变 "
              << kDuckRampMs << "ms" << std::endl;
  }
  if (sample_format != 2) return;  // 只接 PCM16；其余格式由上层计数
  if (sample_rate <= 0 || (channels != 1 && channels != 2)) return;
  const uint32_t stream = kAudioStreamId(channel);
  // 多声道/编码形式如实登记（AUDIT 6.1 / 5.1 #2 #3）：
  // CarLife 的 AudioInit 只有 channelConfig/sampleFormat，能拿到的就是这些；
  // 我们不自行重采样也不自行解码——只把事实报出去。
  core_audio_.channels = static_cast<uint8_t>(channels);
  core_audio_.sample_rate = static_cast<uint32_t>(sample_rate);
  core_audio_.codec = (sample_format == 2) ? mvp::AudioCodec::Pcm16 : mvp::AudioCodec::Unknown;
  core_audio_.simultaneous_streams =
      static_cast<uint8_t>((audio_open_[1] ? 1 : 0) + (audio_open_[2] ? 1 : 0) +
                           (audio_open_[3] ? 1 : 0) + 1);
  core_.set_audio_state(core_audio_);
  std::lock_guard lock(mutex_);
  ensure_session_locked();
  if (audio_open_[stream]) return;
  if (media_.start_audio(stream, kAudioType(stream), static_cast<uint32_t>(sample_rate),
                         static_cast<uint32_t>(channels))) {
    audio_open_[stream] = true;
    audio_sequence_[stream] = 0;
  }
}

void CarLifeInputAdapter::onAudioPcm(int32_t channel, const uint8_t* data, std::size_t len) {
  if (!data || !len) return;
  const uint32_t stream = kAudioStreamId(channel);
  if (stream == 2U) tts_bytes_.fetch_add(len); else media_bytes_.fetch_add(len);
  std::lock_guard lock(mutex_);
  if (!audio_open_[stream]) return;
  if (media_.push_audio(stream, ++audio_sequence_[stream], nowMs(), {data, len})) {
    audio_frames_.fetch_add(1);
  }
}

void CarLifeInputAdapter::onAudioEnd(int32_t channel) {
  // A1：TTS 结束 → 恢复媒体轨音量（与 onAudioInit 成对，且不依赖 audio_open_）。
  if (channel == ch::TTS) {
    media_.set_audio_gain(kDuckRampMs, kFullGainPpm);
    media_ducked_.store(false);
    core_.set_ducking(false, static_cast<int32_t>(kFullGainPpm), static_cast<int32_t>(kFullGainPpm));
    core_audio_.nav_active = 0;
    core_audio_.media_volume_ppm = static_cast<int32_t>(kFullGainPpm);
    core_audio_.active_role = mvp::AudioRole::Media;
    core_.set_audio_state(core_audio_);
    std::cout << "[A1] TTS 结束 -> 恢复媒体音 " << kFullGainPpm << "ppm" << std::endl;
  }
  const uint32_t stream = kAudioStreamId(channel);
  std::lock_guard lock(mutex_);
  if (!audio_open_[stream]) return;
  media_.end_audio(stream);
  audio_open_[stream] = false;
  audio_sequence_[stream] = 0;
}

void CarLifeInputAdapter::onMediaProgress(int32_t progress) {
  // A2：只存值与打日志；不动 detail_（detail_ 专用于连接状态/错误展示）。
  media_progress_.store(progress);
  std::cout << "[media] progress=" << progress << "%" << std::endl;
}

void CarLifeInputAdapter::onModuleStatus(const std::string& detail) {
  std::lock_guard lock(mutex_);
  copy_text(detail_, detail);
}

bool CarLifeInputAdapter::takeControl(HostControl& out) {
  std::lock_guard lock(mutex_);
  return take_control_locked(out);
}

// ---------------- 命令型回传：转前台 / 模块控制 ----------------
void CarLifeInputAdapter::onAppEvent(uint32_t service_type) {
  if (!app_event_handler_) return;
  try { app_event_handler_(service_type); }
  catch (...) { std::cerr << "[app] lifecycle callback failed\n"; }
}

// 两条都是“宿主机要求手机做事”的 HU->MD 消息，线格式见 host_sink.h 的 encodeControlCommand。
// 【为什么走队列而不直接发】本类不是会话线程：直接调用会话的传输会与收包/心跳线程并发写
// 同一条连接。入队后由 Session 在会话线程里统一编码发出，也顺便走完既有的加密出口。
bool CarLifeInputAdapter::requestForeground() {
  // 没连接 = 没有会话在跑，队列没人消费：入队只会积压。
  if (!connected_.load()) {
    ++controls_dropped_;
    return false;
  }
  HostControl control;
  control.type = HostControl::Type::Foreground;
  std::lock_guard lock(mutex_);
  // 容量不足时 enqueue_batch_locked 返回 false 且【不动队列、不清空既有控制】。
  if (!enqueue_batch_locked(&control, 1)) return false;
  std::cout << "[cmd] 转前台 GO_TO_FOREGROUND(0x18025, 空载荷) 已入队（由会话线程发出）" << std::endl;
  return true;
}

bool CarLifeInputAdapter::requestModuleControl(int32_t module_id, int32_t status_id) {
  // 参数边界先判：越界的模块号 / 负状态一律拒发。
  // 【为什么不查“允许的状态枚举”】参考实现里状态号是按模块各自定义的小整数
  // （ModuleStatusModel.java:20-46），本轮契约只要求非负 int32，所以不在这里编枚举。
  if (module_id < HostControl::kModuleIdMin || module_id > HostControl::kModuleIdMax ||
      status_id < 0) {
    ++controls_dropped_;
    return false;
  }
  if (!connected_.load()) {
    ++controls_dropped_;
    return false;
  }
  HostControl control;
  control.type = HostControl::Type::ModuleControl;
  control.module_id = module_id;
  control.status_id = status_id;
  std::lock_guard lock(mutex_);
  if (!enqueue_batch_locked(&control, 1)) return false;
  std::cout << "[cmd] 模块控制 MODULE_CONTROL(0x18028) module=" << module_id
            << " status=" << status_id << " 已入队（由会话线程发出）" << std::endl;
  return true;
}

// ---------------- Core -> 手机 的控制回传 ----------------

void CarLifeInputAdapter::on_control(const mvp::ControlEvent& event) {
  using T = mvp::ControlEvent::Type;
  HostControl control;
  switch (event.type) {
    case T::Touch:
      control.type = HostControl::Type::Touch;
      switch (event.phase) {
        case mvp::ControlEvent::TouchPhase::Down: control.phase = HostControl::TouchPhase::Down; break;
        case mvp::ControlEvent::TouchPhase::Move: control.phase = HostControl::TouchPhase::Move; break;
        default: control.phase = HostControl::TouchPhase::Up; break;
      }
      if (event.x < 0 || event.y < 0) {
        ++controls_dropped_;
        return;
      }
      control.x = event.x;
      control.y = event.y;
      break;

    case T::Key: {
      // 车机硬键 → CarLife 的 KEYCODE_*。
      // 映射表在 carlife/keycode_map.cpp，逐行对着 ServiceTypes.kt:358-408，有自己的单测。
      // 名称无法识别时返回 0，由函数末尾统一丢弃并计数（不发 0）。
      //
      // 【回归修复】上一轮我把 on_control 重写成多事件分发时，漏掉了 Type::Key 分支，
      // 结果硬键全落进 default 被静默丢弃（A4 就死在这里）。
      // 是 mdsim 的 --expect-keys 独立自检把它抓出来的：期望 3 个实际收到 0 个。
      control.type = HostControl::Type::Key;
      // 【有界读】event.key 是 std::array<char,24>，不保证结尾有 NUL；
      // 直接 std::string(event.key.data()) 会一路读到数组之外。
      // 找不到 NUL 就当作“名字无法识别”（不发 0），见 bounded_cstr。
      std::string key_name;
      if (!bounded_cstr(event.key.data(), event.key.size(), &key_name)) {
        ++controls_dropped_;
        return;
      }
      control.keycode = carlife_keycode_for(key_name.c_str());
      break;
    }

    case T::MultiTouch: {
      // 多点触控（AUDIT 5.1 #4）：直接走 HostControl::MultiTouch，由 Session 编码成
      // MSG_TOUCH_ACTION_3 的定长格式（最多 10 点，与 ControlEvent 的上限一致）。
      control.type = HostControl::Type::MultiTouch;
      if (event.phase == mvp::ControlEvent::TouchPhase::Down) {
        control.phase = HostControl::TouchPhase::Down;
      } else if (event.phase == mvp::ControlEvent::TouchPhase::Up) {
        control.phase = HostControl::TouchPhase::Up;
      } else {
        control.phase = HostControl::TouchPhase::Move;
      }
      uint8_t n = event.point_count;
      if (n > HostControl::kMaxPoints) n = static_cast<uint8_t>(HostControl::kMaxPoints);
      for (uint8_t i = 0; i < n; ++i) {
        control.points[i].id = event.points[i].id;
        control.points[i].x = event.points[i].x;
        control.points[i].y = event.points[i].y;
      }
      control.point_count = n;
      if (!n) {
        ++controls_dropped_;
        return;
      }
      break;
    }

    case T::Knob: {
      // 旋钮（AUDIT 5.1 #5）：CarLife 没有专用的旋钮消息，但官方 KEYCODE_* 表里有
      // 方向键与选台/选曲键。映射规则写在这里，用的是官方取值（不是 Android keycode）。
      using KD = mvp::ControlEvent::KnobDir;
      int32_t code = 0;
      switch (event.knob_dir) {
        case KD::Left: code = keycode::MOVE_LEFT; break;
        case KD::Right: code = keycode::MOVE_RIGHT; break;
        case KD::Up: code = keycode::MOVE_UP; break;
        case KD::Down: code = keycode::MOVE_DOWN; break;
        case KD::Press: code = keycode::OK; break;
      }
      control.type = HostControl::Type::Key;
      control.keycode = code;
      if (!code) {
        ++controls_dropped_;
        return;
      }
      // 【转几格发几次】手机侧对每个 KEYCODE_MOVE_* 只移动一格，所以步数必须展开成
      // 重复的按键事件（改动前只发一次，转 3 格手机也当 1 格）。
      //   * Press 是“按下”本身，不含步长：无论 steps 多少都只发一次 OK；
      //   * 其余方向取 |steps|；在 int32 里取绝对值，所以 -32768 不会溢出
      //     （绝不写 abs(int16_t)）；
      //   * 0 格 = 没有方向位移 → 不发键；
      //   * 超过 kMaxKnobRepeat 一律【整批拒绝】：绝不发半个脉冲串
      //     （那会让手机停在错误的位置，比什么都不发更糟）。
      const int32_t repeats = (event.knob_dir == KD::Press)
                                  ? 1
                                  : (event.knob_steps < 0 ? -static_cast<int32_t>(event.knob_steps)
                                                          : static_cast<int32_t>(event.knob_steps));
      if (repeats == 0 || repeats > kMaxKnobRepeat) {
        ++controls_dropped_;
        return;
      }
      std::array<HostControl, static_cast<std::size_t>(kMaxKnobRepeat)> burst{};
      for (int32_t i = 0; i < repeats; ++i) burst[static_cast<std::size_t>(i)] = control;
      std::lock_guard lock(mutex_);
      enqueue_batch_locked(burst.data(), static_cast<std::size_t>(repeats));
      return;
    }

    case T::Voice:
      // 语音按键（Siri/语音助手）：CarLife 的 KEYCODE_VR_START/STOP。
      // 【按下 ≠ 抬起】ControlEvent::phase 就是语音键的按下/抬起（Move 视为按住，
      // 即长按）：改动前不分相一律发 VR_START，手机会一直停在“正在听”上。
      control.type = HostControl::Type::Key;
      control.keycode = (event.phase == mvp::ControlEvent::TouchPhase::Up) ? keycode::VR_STOP
                                                                          : keycode::VR_START;
      break;

    case T::Telephony: {
      // 电话键与 DTMF（AUDIT 5.1 #14）：KEYCODE_PHONE_CALL/END + NUMBER_* 数字键。
      control.type = HostControl::Type::Key;
      // 【有界读 + 必须完整 NUL 结尾】event.dtmf 是 char[16]（最多 15 个字符 + NUL）：
      // 找不到 NUL（= 被截断/残缺的串）或长度超上限，都【整条拒绝】。
      std::string dtmf;
      if (!bounded_cstr(event.dtmf.data(), event.dtmf.size(), &dtmf) ||
          dtmf.size() > kMaxDtmfDigits) {
        ++controls_dropped_;
        return;
      }
      if (dtmf.empty()) {
        // 没有数字串 → 电话硬键，按名字映射（同样有界读）。
        std::string tel_name;
        if (!bounded_cstr(event.key.data(), event.key.size(), &tel_name)) {
          ++controls_dropped_;
          return;
        }
        control.keycode = carlife_keycode_for(tel_name.c_str());
        break;
      }
      // DTMF：改动前只看第 1 个字符，拨 “123” 手机只收到 1。
      // 数字 0..9 → KEYCODE_NUMBER_0(0x23)+n（表里连续）；'*'/'#' 有真实键码
      // （ServiceTypes.kt:402-404 的 KEYCODE_NUMBER_STAR/POUND），不是自造映射。
      // 【整串原子】非法字符 → 整条拒绝；队列没空间 → 整条拒绝（不得部分下发，
      // 否则手机拨出一个错的号码）。
      std::array<HostControl, kMaxDtmfDigits + 1> burst{};
      for (std::size_t i = 0; i < dtmf.size(); ++i) {
        const char c = dtmf[i];
        int32_t digit = 0;
        if (c >= '0' && c <= '9') {
          digit = keycode::NUMBER_0 + (c - '0');
        } else if (c == '*') {
          digit = keycode::NUMBER_STAR;
        } else if (c == '#') {
          digit = keycode::NUMBER_POUND;
        } else {
          ++controls_dropped_;
          return;
        }
        burst[i] = control;
        burst[i].keycode = digit;
      }
      std::size_t count = dtmf.size();
      // Dial must enqueue the complete number and PHONE_CALL atomically.
      if (event.x == 2) {
        burst[count] = control;
        burst[count++].keycode = keycode::PHONE_CALL;
      }
      std::lock_guard lock(mutex_);
      enqueue_batch_locked(burst.data(), count);
      return;
    }

    case T::Gesture:
    case T::Proximity:
    case T::VehicleCtrl:
    default:
      // 【为什么这些不往外发】查遍两套官方代码：
      //   * 手势：TouchScroll/TouchFling 有 proto，但【没有任何 serviceType 绑定】；
      //   * 接近传感器：CarLife 侧协议里根本没有这个能力（CarPlay 才有 ProximityReport）；
      //   * 车控：VEHICLE_CONTROL(0x0001006F) 的方向是【手机->车机】，车机往手机发车控
      //     没有定义的消息。
      // 所以这些事件只能被上层的 Core 桌面自己消费，不能编造一条消息发出去。
      ++controls_dropped_;
      return;
  }
  if (control.type == HostControl::Type::Key && control.keycode == 0) {
    ++controls_dropped_;
    return;
  }
  std::lock_guard lock(mutex_);
  push_control_locked(control);
}

void CarLifeInputAdapter::push_control_locked(const HostControl& control) {
  // Move 事件合并成一条，避免拖动时把有界队列冲掉；队列满时丢弃 Move。
  if (control_count_ && control.phase == HostControl::TouchPhase::Move) {
    auto& last = controls_[(control_begin_ + control_count_ - 1) % kControlCapacity];
    if (last.type == HostControl::Type::Touch && last.phase == HostControl::TouchPhase::Move) {
      last = control;
      return;
    }
  }
  if (control_count_ == kControlCapacity) {
    if (control.phase == HostControl::TouchPhase::Move) {
      ++controls_dropped_;
      return;
    }
    control_begin_ = control_count_ = 0;
    ++controls_dropped_;
  }
  controls_[(control_begin_ + control_count_) % kControlCapacity] = control;
  ++control_count_;
  ++controls_sent_;
}

bool CarLifeInputAdapter::enqueue_batch_locked(const HostControl* items, std::size_t count) {
  if (!items || count == 0) return true;  // 空批 = 无事发生（不计数丢弃）
  // 【先看空间再写】要么整批进去，要么一个也不进。
  // 不能用 push_control_locked 逐条入队：队列满时它会先把整个队列清空腾位置
  // （那会丢掉已经在路上的触控/硬键，契约明确要求“没空间就保持既有控制不动”），
  // 而且逐条入队会发出半个脉冲串/半截号码。
  if (count > kControlCapacity || control_count_ + count > kControlCapacity) {
    controls_dropped_ += count;
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    controls_[(control_begin_ + control_count_) % kControlCapacity] = items[i];
    ++control_count_;
  }
  controls_sent_ += count;
  return true;
}

bool CarLifeInputAdapter::take_control_locked(HostControl& out) {
  if (!control_count_) return false;
  out = controls_[control_begin_];
  control_begin_ = (control_begin_ + 1) % kControlCapacity;
  --control_count_;
  return true;
}

void CarLifeInputAdapter::clear_bounded_locked() {
  control_begin_ = control_count_ = 0;
}

// ---------------- 媒体面 ----------------

void CarLifeInputAdapter::ensure_session_locked() {
  if (session_id_) return;
  static std::atomic<uint64_t> epoch{0};
  session_id_ = kSessionEpochBase + epoch.fetch_add(1) + 1;
  media_.begin_session(session_id_);
  media_.transport_ready();
}

void CarLifeInputAdapter::ensure_video_stream_locked() {
  if (video_stream_open_ || !config_size_ || !video_width_ || !video_height_) return;
  if (media_.set_video_stream(0, video_stream_id_, video_width_, video_height_,
                              {config_bytes_.data(), config_size_})) {
    video_stream_open_ = true;
    video_configs_.fetch_add(1);
  }
}

void CarLifeInputAdapter::forward_video_annexb(const uint8_t* data, std::size_t len) {
  bool has_sps = false;
  bool has_pps = false;
  bool keyframe = false;
  std::array<uint8_t, kMaxConfigBytes> sps{};
  std::array<uint8_t, kMaxConfigBytes> pps{};
  std::size_t sps_size = 0;
  std::size_t pps_size = 0;
  for_each_nal(data, len, [&](const uint8_t* p, std::size_t begin, std::size_t end, uint8_t type) {
    const std::size_t size = end - begin;
    if (type == 5) keyframe = true;
    if (type == 7 && size && size <= kMaxConfigBytes) {
      std::memcpy(sps.data(), p + begin, size);
      sps_size = size;
      has_sps = true;
    } else if (type == 8 && size && size <= kMaxConfigBytes) {
      std::memcpy(pps.data(), p + begin, size);
      pps_size = size;
      has_pps = true;
    }
  });
  if (keyframe) keyframes_.fetch_add(1);

  std::lock_guard lock(mutex_);
  ensure_session_locked();
  if (has_sps && has_pps && sps_size + pps_size <= kMaxConfigBytes) {
    const bool changed = config_size_ != sps_size + pps_size ||
                         std::memcmp(config_bytes_.data(), sps.data(), sps_size) != 0 ||
                         std::memcmp(config_bytes_.data() + sps_size, pps.data(), pps_size) != 0;
    if (changed) {
      std::memcpy(config_bytes_.data(), sps.data(), sps_size);
      std::memcpy(config_bytes_.data() + sps_size, pps.data(), pps_size);
      config_size_ = sps_size + pps_size;
      video_sequence_ = 0;
      // 参数集变化意味着解码器要重建：重新登记流并清空旧帧。
      if (video_stream_open_) {
        media_.end_video_stream(0);
        video_stream_open_ = false;
      }
    }
  }
  ensure_video_stream_locked();
  if (!video_stream_open_) return;
  if (media_.push_video_frame(0, ++video_sequence_, nowMs(), keyframe, {data, len})) {
    frames_forwarded_.fetch_add(1);
  }
}

bool CarLifeInputAdapter::takeMicrophone(int16_t* dst, std::size_t capacity_frames,
                                         std::size_t& frames_out) {
  // 无采集源：不编造 PCM，会话侧只会记录“手机在等麦克风”（见 HostSink::takeMicrophone）。
  // 【约定】任何失败路径都必须把 frames_out 置 0 —— 调用方按“非 0 就是有效帧”处理，
  // 留着上次的计数会让它去读一片没被写过的缓冲。
  frames_out = 0;
  if (dst == nullptr || capacity_frames == 0) return false;
  if (!microphone_source_) return false;
  // 源不可信（它是外部注入的 std::function）：
  //   * 抛异常 → 当成没数据，绝不让异常穿回收包线程（那会断整个会话）；
  //   * 返回 false → 没数据；
  //   * 报 0 帧 → 空帧，不发（发一条 0 字节的 VR_DATA 没有意义）；
  //   * 报超过 capacity_frames 的帧数 → 越界，丢弃（绝不按它去 memcpy）。
  std::size_t produced = 0;
  bool ok = false;
  try {
    ok = microphone_source_(dst, capacity_frames, produced);
  } catch (...) {
    frames_out = 0;
    return false;
  }
  if (!ok || produced == 0 || produced > capacity_frames) {
    frames_out = 0;
    return false;
  }
  frames_out = produced;
  mic_frames_.fetch_add(produced);
  return true;
}

bool CarLifeInputAdapter::prepareMicrophone() {
  // 麦克风“准备”缝（0x00010071 → 0x18072）。参考实现里这一步是播一声提示音并在
  // 播完/出错后回 DONE（VRModule.kt），它本身【不校验硬件麦克风】。
  // 【本适配器不做的事】不合成提示音、不打开麦克风、不置任何“正在录音”状态，
  // 也不向 Core 报“麦克风可用” —— 只把宿主机的准备回调转成同步答复。
  if (!mic_prepare_) {
    std::cout << "[M] MIC_RECORD_PREPARE_START：提示音不可用（Session 结束准备阶段）"
              << std::endl;
    return false;
  }
  // 回调是外部注入的 std::function：抛异常一律当失败，绝不让它穿回收包线程。
  bool ready = false;
  try {
    ready = mic_prepare_();
  } catch (const std::exception&) {
    std::cout << "[M] MIC_RECORD_PREPARE_START：准备回调抛异常 → false"
              << std::endl;
    return false;
  } catch (...) {
    std::cout << "[M] MIC_RECORD_PREPARE_START：准备回调抛非 std 异常 → false" << std::endl;
    return false;
  }
  std::cout << "[M] MIC_RECORD_PREPARE_START：准备回调返回 " << (ready ? "true" : "false")
            << std::endl;
  return ready;
}

bool CarLifeInputAdapter::takeVehicleReport(VehicleReport& out) {
  // 车况数据在真车上来自 CAN/OBD，本机没有这些传感器。
  // 没注入数据源就返回 false → Session 不会发任何车况消息（宁可不上报，也不编造）。
  if (!vehicle_source_) return false;
  const bool ok = vehicle_source_(out);
  if (ok) vehicle_report_sent_.fetch_add(1);
  return ok;
}

// ────────────────────────────────────────────────────────
// A..U：本轮新增的全部 HostSink 实现（CarLife -> Core）
// 统一约定：
//   * 能归到 Core 的冻结接口的，就调对应 setter（整块覆盖 + 变化检测）；
//   * 冻结接口里没有分组的（导航逐向/车控/激活/文件传输…），落在有界的 status 里，
//     由管理页面从 /api/state 的 carlife 段渲染（与 media_progress/bt_phase 同一做法）。
// ────────────────────────────────────────────────────────

void CarLifeInputAdapter::onMediaInfo(const pb::MediaInfo& info) {
  media_info_count_.fetch_add(1);
  mvp::MediaInfo mi;
  mi.valid = true;
  copy_text(mi.title, info.song);
  copy_text(mi.artist, info.artist);
  copy_text(mi.album, info.album);
  copy_text(mi.app, info.source);
  mi.duration_ms = static_cast<uint32_t>(info.duration > 0 ? info.duration : 0);
  mi.playing = 1;
  mi.track_number = static_cast<uint32_t>(info.playlist_num > 0 ? info.playlist_num : 0);
  mi.repeat_mode = static_cast<uint8_t>(info.mode);
  mi.shuffle = (info.mode == 2) ? 1 : 0;
  // 封面（AUDIT 6.5）：CarLife 把图直接放在 MEDIA_INFO.albumArt（bytes），
  // 我们原字节存进 Core 的有界缓冲（不做解码），页面按 revision 变化再取。
  if (!info.album_art.empty()) {
    const uint32_t rev = ++artwork_revision_;
    mi.artwork_revision = rev;
    if (core_.set_artwork(rev, info.album_art.data(), info.album_art.size())) {
      mi.has_artwork = true;
      mi.artwork_bytes = static_cast<uint32_t>(info.album_art.size());
      media_cover_bytes_ = mi.artwork_bytes;
    }
  }
  core_media_ = mi;
  core_.set_media_info(mi);
  {
    std::lock_guard lock(mutex_);
    copy_text(media_song_, info.song);
    copy_text(media_artist_, info.artist);
    copy_text(media_album_, info.album);
    media_duration_ms_ = mi.duration_ms;
    media_playing_ = true;
  }
  std::cout << "[A] MEDIA_INFO \"" << info.song << "\" / \"" << info.artist
            << "\" 封面=" << info.album_art.size() << "B 已入 SessionCore" << std::endl;
}

void CarLifeInputAdapter::onNaviNextTurn(const pb::NaviNextTurnInfo& info) {
  navi_count_.fetch_add(1);
  {
    std::lock_guard lock(mutex_);
    navi_action_ = info.action;
    navi_next_turn_ = info.next_turn;
    copy_text(navi_road_, info.road_name);
    navi_total_m_ = info.total_distance;
    navi_remain_m_ = info.remain_distance;
  }

  // ── 落到 Core 的 NavigationState（AUDIT 5.1 #7）──
  // 【maneuver 为什么不填】action / nextTurn 的取值域在参考树里【查不到】：
  // 全树唯一的 maneuver 枚举在 Reference/aa-proxy-rs（那是 **Android Auto** 的
  // NavigationType），CarLife 的码表由百度导航 App 产生，没有任何一份参考给了它。
  // 契约里的 maneuver_code 正是为这种情形准备的（“无法映射时保留原值”），
  // 所以：maneuver 保持 None（“能确定才填”），原值走 maneuver_code。
  // 一旦以后能从真机采样出码表，只改这一个函数即可。
  mvp::NavigationState nav = core_nav_;
  nav.valid = true;
  nav.active = 1;
  nav.maneuver = mvp::Maneuver::None;
  nav.maneuver_code = static_cast<uint32_t>(info.action);
  copy_text(nav.road_name, info.road_name);
  // turnIconData 原样透传（字节可能不是可打印文本，copy_text 会按 UTF-8 边界截断）。
  copy_text(nav.icon, info.turn_icon);
  // 两个距离的配对：【按字段名推定，不是编表】
  //   proto 只有 totalDistance / remainDistance，没有“到下一动作”的专名。
  //   remainDistance 字面就是“剩余距离” ⇒ 全程剩余；
  //   totalDistance 与之配对出现在“下一转向”消息里 ⇒ 到下一动作的距离。
  // 因此：distance_to_maneuver_m = totalDistance，distance_remaining_m = remainDistance。
  // 这是本文件里唯一一处“按名字推定”的地方，若真机证明相反，对调这两行即可。
  nav.distance_to_maneuver_m = static_cast<uint32_t>(info.total_distance > 0 ? info.total_distance : 0);
  nav.distance_remaining_m = static_cast<uint32_t>(info.remain_distance > 0 ? info.remain_distance : 0);
  // field 6：C++ 库版的 int32 time（Kotlin SDK 版这里是 bytes turnIconData，已在 wire 层分流）
  if (info.time_s > 0) nav.time_remaining_s = static_cast<uint32_t>(info.time_s);
  core_nav_ = nav;
  core_.set_navigation_state(nav);

  std::cout << "[C] NAV_NEXT_TURN action=" << info.action << " nextTurn=" << info.next_turn
            << " road=\"" << info.road_name << "\" remain=" << info.remain_distance << "m"
            << (info.time_s ? " time=" + std::to_string(info.time_s) + "s" : "")
            << " -> Core.nav.maneuver_code=" << nav.maneuver_code << " (maneuver=None, 无码表)"
            << std::endl;
}

void CarLifeInputAdapter::onNaviAssistantGuide(const pb::NaviAssistantGuideInfo& info) {
  navi_count_.fetch_add(1);
  // 辅助引导是【另一个码空间】（action/assistantType/trafficSignType/cameraSpeed），
  // 与转向消息的 action 不是同一套取值 ⇒ 只更新距离类字段，
  // 【不碰 maneuver/maneuver_code】，避免把两个码空间混到一个字段里。
  mvp::NavigationState nav = core_nav_;
  nav.valid = true;
  nav.active = 1;
  if (info.total_distance > 0) {
    nav.distance_to_maneuver_m = static_cast<uint32_t>(info.total_distance);
  }
  if (info.remain_distance > 0) {
    nav.distance_remaining_m = static_cast<uint32_t>(info.remain_distance);
  }
  core_nav_ = nav;
  core_.set_navigation_state(nav);
  std::cout << "[C] NAV_ASSISTANT_GUIDE action=" << info.action
            << " type=" << info.assistant_type << " sign=" << info.traffic_sign_type
            << " cameraSpeed=" << info.camera_speed
            << " -> 已更新 Core.nav 的距离（不动 maneuver_code）" << std::endl;
}

void CarLifeInputAdapter::onVideoControl(uint32_t service_type, int32_t frame_rate) {
  // 帧率动态调整（AUDIT 5.1 #11）：把手机确认的帧率写进 DisplayConfig。
  if (frame_rate > 0) {
    core_display_.actual_fps = static_cast<uint16_t>(frame_rate);
    core_display_.target_fps = static_cast<uint16_t>(frame_rate);
    core_.set_display_config(core_display_);
  }
  std::cout << "[T] VIDEO_CONTROL serviceType=0x" << std::hex << service_type << std::dec
            << " frameRate=" << frame_rate << std::endl;
}

void CarLifeInputAdapter::onAudioFocus(bool gained, int32_t channel) {
  (void)channel;
  // 与 A1 同一套语义的对外事件（音频仲裁，AUDIT 3.2 / 8.5）。
  std::cout << "[P] AUDIO_FOCUS " << (gained ? "gained" : "released") << std::endl;
}

void CarLifeInputAdapter::onPhoneInput(PhoneInputKind kind, int32_t delta_x, int32_t delta_y,
                                       float value) {
  phone_input_count_.fetch_add(1);
  // 手机侧的触摸板/手势（AUDIT 5.1 #4 #5）。
  // 【方向提醒】这些是【手机告诉车机】的输入，不是车机要发给手机的 —— 所以不能
  // 走 core_.route_control()（那会把事件送回 CarLife 自己，形成回环）。
  // 我们把它落到 InteractionState 里，供 Core 桌面/管理页面消费。
  core_input_.touchpad = 1;
  switch (kind) {
    case PhoneInputKind::TouchPadMove:
      core_input_.knob = 1;
      std::cout << "[I] 手机触摸板拖动 delta=(" << delta_x << "," << delta_y << ")" << std::endl;
      break;
    case PhoneInputKind::TouchPadPinch:
      std::cout << "[J] 手机手势 pinch scale=" << value << std::endl;
      break;
    case PhoneInputKind::TouchPadDown:
      std::cout << "[I] 手机触摸板 down" << std::endl;
      break;
    case PhoneInputKind::TouchPadUp:
      std::cout << "[I] 手机触摸板 up" << std::endl;
      break;
  }
  core_.set_input_state(core_input_);
}

void CarLifeInputAdapter::onPhoneTouchAction3(const pb::TouchAction3& event) {
  multi_touch_count_.fetch_add(1);
  const uint8_t n = static_cast<uint8_t>(
      event.pointers.size() > 255 ? 255 : event.pointers.size());
  core_input_.multi_touch_used = n;
  {
    std::lock_guard lock(mutex_);
    last_touch_points_ = n;
  }
  core_.set_input_state(core_input_);
  std::cout << "[H] 手机多点触控 action=" << event.action << " points=" << event.pointers.size()
            << std::endl;
}

void CarLifeInputAdapter::onGearFromPhone(int32_t gear) {
  std::lock_guard lock(mutex_);
  phone_gear_ = gear;
  std::cout << "[D] 手机上报档位 gear=" << gear << std::endl;
}

void CarLifeInputAdapter::onHfpRequest(const pb::BTHfpRequest& request) {
  hfp_count_.fetch_add(1);
  {
    std::lock_guard lock(mutex_);
    last_hfp_command_ = request.command;
    last_hfp_dtmf_ = static_cast<uint32_t>(request.dtmf_code);
  }
  // 电话控制（AUDIT 5.1 #14）：把手机的 HFP 动作落到 TelephonyState 上。
  // 【没做什么】真正去拨号需要车机侧的电话镜像（Android 上是 BluetoothHeadset）；
  // 本机没有 PBAP/HFP-AG 实现，所以这里只“接受并呈现”，不冒充已拨号。
  const char* what = "?";
  switch (request.command) {
    case hfp::REQ_START_CALL: what = "START_CALL"; break;
    case hfp::REQ_TERMINATE_CALL: what = "TERMINATE_CALL"; break;
    case hfp::REQ_ANSWER_CALL: what = "ANSWER_CALL"; break;
    case hfp::REQ_REJECT_CALL: what = "REJECT_CALL"; break;
    case hfp::REQ_DTMF_CODE: what = "DTMF"; break;
    case hfp::REQ_MUTE_MIC: what = "MUTE_MIC"; break;
    case hfp::REQ_UNMUTE_MIC: what = "UNMUTE_MIC"; break;
    default: break;
  }
  std::cout << "[F] HFP 请求 " << what
            << (request.phone_num.empty() ? "" : " num=" + request.phone_num)
            << (request.command == hfp::REQ_DTMF_CODE
                    ? " dtmf=" + std::to_string(request.dtmf_code)
                    : "")
            << std::endl;
}

void CarLifeInputAdapter::onTelephony(uint32_t service_type, int32_t state,
                                      const std::string& number, const std::string& name) {
  (void)service_type;
  hfp_count_.fetch_add(1);
  // 0=空闲 1=来电 2=拨出 3=通话中（与 TelephonyState.call_state 的取值域一致）。
  mvp::TelephonyState ts = core_telephony_;
  ts.call_state = static_cast<uint8_t>(state < 0 ? 0 : state);
  copy_text(ts.caller_number, number.empty() ? name : number);
  copy_text(ts.caller, name.empty() ? number : name);
  core_telephony_ = ts;
  core_.set_telephony_state(ts);
  {
    std::lock_guard lock(mutex_);
    call_state_ = ts.call_state;
    copy_text(call_caller_, std::string(ts.caller.data()));
    copy_text(call_number_, std::string(ts.caller_number.data()));
  }
  std::cout << "[F] 电话状态 state=" << state
            << (number.empty() ? "" : " num=" + number)
            << (name.empty() ? "" : " name=" + name) << std::endl;
}

void CarLifeInputAdapter::onHfpStatusRequest(const pb::BTHfpStatusRequest& request) {
  std::cout << "[F] HFP 状态查询 type=" << request.type << "（已由 Session 应答）" << std::endl;
}

void CarLifeInputAdapter::onVoiceControl(const pb::VoiceControlRequest& request) {
  voice_control_count_.fetch_add(1);
  {
    std::lock_guard lock(mutex_);
    last_voice_command_ = request.command;
  }
  std::cout << "[O] 语音控制 command=" << request.command << " opt=" << request.opt << std::endl;
}

void CarLifeInputAdapter::onMicRecord(uint32_t service_type) {
  ++mic_requests_;
  std::cout << "[M] 麦克风录音控制 serviceType=0x" << std::hex << service_type << std::dec
            << std::endl;
}

void CarLifeInputAdapter::onVehicleControl(const pb::VehicleControl& control) {
  vehicle_control_count_.fetch_add(1);
  {
    std::lock_guard lock(mutex_);
    last_vehicle_ctrl_type_ = control.type;
    last_vehicle_ctrl_id_ = control.id;
  }
  // 车控（AUDIT 5.1 #18）：手机让我们控车。本机没有车辆总线（CAN/空调/车窗），
  // 所以只“接受 + 呈现 + 计数”，绝不回复“已执行”——那会是假值。
  std::cout << "[N] 车控 type=" << control.type << " id=" << control.id
            << " area=" << control.area_id << std::endl;
}

void CarLifeInputAdapter::onActivationRequest(const pb::ActiveRequest& request) {
  activation_count_.fetch_add(1);
  mvp::LinkState ls = core_link_;
  ls.activation_state = (config_.activationState == 3 && !config_.activationToken.empty()) ? 3 : 0;
  core_link_ = ls;
  core_.set_link_state(ls);
  {
    std::lock_guard lock(mutex_);
    activation_state_ = ls.activation_state;
  }
  std::cout << "[R] 激活请求 mac=\"" << request.mac << "\" isActive=" << request.is_active
            << " -> activation_state=" << static_cast<int>(ls.activation_state) << std::endl;
}

void CarLifeInputAdapter::onEncryption(uint8_t state, const std::string& detail,
                                       uint64_t encrypted, uint64_t decrypt_failures) {
  mvp::LinkState ls = core_link_;
  // 0=关 1=要求 2=已启用（与 EncryptionState 的 Off/Advertised/KeyReceived/Ready 四态对齐）。
  // 【改动前是 bug】原来只分“Ready → 2，其余 → 1”，于是 Off（本机根本没启用，例如没装
  // OpenSSL 或没配 contentEncryption）也会被报成 1=要求，页面与控制端都会误以为在协商中。
  // 现在逐态映射：Off → 0；Ready → 2；其余（Advertised/KeyReceived/未知值）→ 1。
  uint8_t mapped = 1;
  if (state == static_cast<uint8_t>(EncryptionState::Off)) {
    mapped = 0;
  } else if (state == static_cast<uint8_t>(EncryptionState::Ready)) {
    mapped = 2;
  }
  ls.content_encryption = mapped;
  core_link_ = ls;
  core_.set_link_state(ls);
  {
    std::lock_guard lock(mutex_);
    encryption_state_ = state;
  }
  encrypted_messages_.store(encrypted);
  decrypt_failures_.store(decrypt_failures);
  std::cout << "[R] 内容加密状态=" << static_cast<int>(state) << " (" << detail
            << ") 已加密载荷=" << encrypted << "条 解密失败=" << decrypt_failures << "条"
            << std::endl;
}

void CarLifeInputAdapter::onCarDataSubscribe(uint32_t service_type) {
  car_data_subscribe_count_.fetch_add(1);
  std::cout << "[D] 车辆数据订阅请求 0x" << std::hex << service_type << std::dec << std::endl;
}

void CarLifeInputAdapter::onFileTransfer(uint32_t service_type, int64_t total_bytes,
                                         std::size_t chunk_bytes) {
  file_transfer_count_.fetch_add(1);
  std::lock_guard lock(mutex_);
  switch (service_type) {
    case msg::DATA_MD_TRANSFER_START:
    case msg::SEND_START:
      file_transfer_active_ = 1;
      file_transfer_bytes_ = 0;
      file_transfer_total_ = static_cast<uint32_t>(total_bytes > 0 ? total_bytes : 0);
      ota_state_ = transfer::OTA_DOWNLOADING;
      break;
    case msg::SENDING_DATA:
    case msg::DATA_MD_TRANSFER_SEND:
      file_transfer_bytes_ += static_cast<uint32_t>(chunk_bytes);
      break;
    case msg::SEND_FINISH:
    case msg::DATA_MD_TRANSFER_END:
      file_transfer_active_ = 0;
      ota_state_ = transfer::OTA_READY;
      break;
    case msg::SEND_STOP:
    case msg::STOP_RECEIVE:
      file_transfer_active_ = 0;
      ota_state_ = transfer::OTA_FAILED;
      break;
    case msg::DATA_HU_UPDATE_START:
      ota_state_ = transfer::OTA_DOWNLOADING;
      break;
    case msg::DATA_HU_UPDATE_END:
      ota_state_ = transfer::OTA_VERIFYING;
      break;
    default:
      break;
  }
  mvp::LinkState ls = core_link_;
  ls.file_transfer_active = file_transfer_active_;
  ls.file_transfer_bytes = file_transfer_bytes_;
  ls.file_transfer_total = file_transfer_total_;
  ls.ota_state = ota_state_;
  core_link_ = ls;
  core_.set_link_state(ls);
  std::cout << "[Q] 文件传输/OTA serviceType=0x" << std::hex << service_type << std::dec
            << " total=" << total_bytes << " chunk=" << chunk_bytes
            << " 已收=" << file_transfer_bytes_ << "B ota_state="
            << static_cast<int>(ota_state_) << std::endl;
}

void CarLifeInputAdapter::onTimeSync(int32_t timestamp) {
  std::lock_guard lock(mutex_);
  last_time_sync_ = timestamp;
  std::cout << "[S] 时间同步 timestamp=" << timestamp << std::endl;
}

CarLifeInputStatus CarLifeInputAdapter::status() const {
  CarLifeInputStatus out;
  out.running = running_.load();
  out.connected = connected_.load();
  out.state = static_cast<State>(state_code_.load());
  std::lock_guard lock(mutex_);
  copy_text(out.state_name, std::string(state_name_.data()));
  copy_text(out.detail, std::string(detail_.data()));
  out.phone_ip = phone_ip_text_;
  out.video_width = video_width_;
  out.video_height = video_height_;
  out.video_rate = video_rate_;
  out.frames_received = frames_received_.load();
  out.frames_forwarded = frames_forwarded_.load();
  out.keyframes = keyframes_.load();
  out.video_configs = video_configs_.load();
  out.video_bytes = video_bytes_.load();
  out.media_bytes = media_bytes_.load();
  out.tts_bytes = tts_bytes_.load();
  out.audio_frames = audio_frames_.load();
  out.controls_sent = controls_sent_.load();
  out.controls_dropped = controls_dropped_.load();
  out.mic_requests = mic_requests_.load();
  out.mic_frames = mic_frames_.load();
  // A2：播放进度；A1：媒体音是否被导航压低。
  out.media_progress = media_progress_.load();
  out.media_ducked = media_ducked_.load();
  out.bt_phase = bt_phase_;
  out.bt_note = bt_note_;
  // ── 本轮新增：逐项接口的计数与最近值（验证脚本按这些字段判定）──
  out.media_info_count = media_info_count_.load();
  out.navi_count = navi_count_.load();
  out.phone_input_count = phone_input_count_.load();
  out.multi_touch_count = multi_touch_count_.load();
  out.hfp_count = hfp_count_.load();
  out.vehicle_control_count = vehicle_control_count_.load();
  out.voice_control_count = voice_control_count_.load();
  out.file_transfer_count = file_transfer_count_.load();
  out.activation_count = activation_count_.load();
  out.car_data_subscribe_count = car_data_subscribe_count_.load();
  out.vehicle_report_sent = vehicle_report_sent_.load();
  out.encrypted_messages = encrypted_messages_.load();
  out.decrypt_failures = decrypt_failures_.load();
  out.media_song = media_song_;
  out.media_artist = media_artist_;
  out.media_album = media_album_;
  out.media_duration_ms = media_duration_ms_;
  out.media_position_ms = media_position_ms_;
  out.media_playing = media_playing_;
  out.navi_action = navi_action_;
  out.navi_next_turn = navi_next_turn_;
  out.navi_road = navi_road_;
  out.navi_total_m = navi_total_m_;
  out.navi_remain_m = navi_remain_m_;
  out.call_state = call_state_;
  out.call_caller = call_caller_;
  out.call_number = call_number_;
  out.last_hfp_command = last_hfp_command_;
  out.last_hfp_dtmf = last_hfp_dtmf_;
  out.last_vehicle_ctrl_type = last_vehicle_ctrl_type_;
  out.last_vehicle_ctrl_id = last_vehicle_ctrl_id_;
  out.last_voice_command = last_voice_command_;
  out.activation_state = activation_state_;
  out.encryption_state = encryption_state_;
  out.file_transfer_active = file_transfer_active_;
  out.file_transfer_bytes = file_transfer_bytes_;
  out.file_transfer_total = file_transfer_total_;
  out.ota_state = ota_state_;
  out.phone_gear = phone_gear_;
  out.last_time_sync = last_time_sync_;
  out.last_touch_points = last_touch_points_;
  return out;
}

}  // namespace carlife
