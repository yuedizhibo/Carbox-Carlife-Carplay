#include "carlife/session.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <iostream>
#include <sstream>

#include "carlife/service_types.h"

namespace carlife {
namespace {

uint64_t nowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

std::string hhmmss() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

// dev 模式的"鉴权算法"：仅用于让 verify 分支被真实执行、可被模拟器驱动。
// 真机上的算法在闭源 libencryption.so 的 getVerifyCode/getVerifyResult 里
// （见 SPEC.md 第 4 节），这里不做任何冒充。
std::string devDigest(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[24];
  std::snprintf(buf, sizeof(buf), "sim-%016llx", static_cast<unsigned long long>(h));
  return buf;
}

bool ensureDir(const std::string& path) {
  return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

}  // namespace

const char* toString(State s) {
  switch (s) {
    case State::Idle: return "Idle";
    case State::Connected: return "Connected";
    case State::VersionMatched: return "VersionMatched";
    case State::AuthRequested: return "AuthRequested";
    case State::AuthVerified: return "AuthVerified";
    case State::Established: return "Established";
    case State::VideoInitSent: return "VideoInitSent";
    case State::VideoStarted: return "VideoStarted";
    case State::Failed: return "Failed";
    case State::Closed: return "Closed";
  }
  return "?";
}

Session::Session(SessionConfig cfg) : cfg_(std::move(cfg)) {}

Session::~Session() {
  running_.store(false);
  tx_.close();
  if (hbThread_.joinable()) hbThread_.join();
  if (snapThread_.joinable()) snapThread_.join();
}

void Session::log(const char* level, const std::string& m) const {
  std::cout << "[" << level << "] " << m << std::endl;
}

void Session::requestStop() {
  running_.store(false);
  tx_.close();
}

void Session::setState(State s) {
  if (state_ == s) return;
  std::ostringstream os;
  os << toString(state_) << " -> " << toString(s);
  state_ = s;
  log("state", os.str());
  if (host_) host_->onState(state_, os.str());
}

bool Session::sendCmd(uint32_t serviceType, std::vector<uint8_t> payload) {
  return sendOn(ch::CMD, serviceType, std::move(payload));
}

// 所有出站消息的唯一出口。
// 【为什么在这里加密】照抄 EncryptionTool.kt：加密对象是【commandSize 之后的 payload】，
// 帧头保持明文；否则接收方无法从帧头读出密文长度。Frame::encode() 会用新的
// payload.size() 写 payloadSize，所以“修正 payloadSize”这一步是自动完成的。
// 加密协商本身的六条消息发生在 Ready 之前，因此天然不会被加密（与参考实现一致）。
bool Session::sendOn(int32_t channel, uint32_t serviceType, std::vector<uint8_t> payload) {
  if (cipher_.enabled() && !payload.empty()) {
    if (cipher_.encrypt_payload(payload)) {
      encryptedMessages_.fetch_add(1);
    } else {
      // 加密失败就不能发：发出去对方也解不开，只会在对端变成一个静默的协议错误。
      log("warn", "内容加密：载荷加密失败，本条消息未发送（serviceType=0x" +
                       [](uint32_t v) { char b[12]; std::snprintf(b, sizeof b, "%08X", v); return std::string(b); }(serviceType) + ")");
      return false;
    }
  }
  return tx_.send(channel, serviceType, std::move(payload));
}

// 入站解密。规则全部来自 EncryptionTool.decrypt：
//   * 未就绪（没有 AES 密钥或还没 Ready）→ 原样返回；
//   * 该 serviceType 曾经解密失败过（decryptExcludes）→ 不再尝试；
//   * 解密失败 → 把 serviceType 加入 excludes，并【按明文继续】（绝不因此断会话）。
void Session::decryptInbound(Frame& frame) {
  if (!cipher_.enabled()) return;
  if (frame.payload.empty()) return;
  if (cipher_.service_excluded(frame.serviceType)) return;
  std::vector<uint8_t> copy = frame.payload;
  if (cipher_.decrypt_payload(copy)) {
    frame.payload.swap(copy);
    return;
  }
  cipher_.exclude_service(frame.serviceType);
  decryptFailures_.fetch_add(1);
  char buf[12];
  std::snprintf(buf, sizeof buf, "0x%08X", frame.serviceType);
  log("warn", std::string("内容加密：解密失败，已把 ") + buf +
                  " 加入 decryptExcludes（后续不再尝试，按明文处理）");
}

bool Session::runWithPhone(const std::string& phoneIp, std::string* err) {
  log("net", "wireless: connecting to phone " + phoneIp +
                 " (CMD:" + std::to_string(port::MD_CMD) + " VIDEO:" + std::to_string(port::MD_VIDEO) +
                 " MEDIA:" + std::to_string(port::MD_AUDIO) + " TTS:" + std::to_string(port::MD_TTS) +
                 " VR:" + std::to_string(port::MD_VR) + " TOUCH:" + std::to_string(port::MD_TOUCH) + ")");
  if (!tx_.connectPhone(phoneIp, /*includeUpdateChannel=*/true, err)) return false;
  return start(err);
}

bool Session::runAsServer(std::string* err) {
  log("net", "wired/adb-forward: listening on HU ports " + std::to_string(port::HU_CMD) + "/" +
                 std::to_string(port::HU_VIDEO) + "/" + std::to_string(port::HU_AUDIO));
  if (!tx_.serveHUPorts(true, err)) return false;
  return start(err);
}

bool Session::start(std::string* err) {
  log("net", "channels established: " + std::to_string(tx_.openChannels()));
  // 呈现由宿主机提供：本机工具用 SdlHostSink，Core 接入用 CoreHostSink。
  // Session 本身不再持有解码器、窗口或音频设备。
  if (!host_) {
    const std::string message = "no host sink set; call setHost() before start";
    if (err) *err = message;
    lastError_ = message;
    setState(State::Failed);
    return false;
  }
  negotiated_.width = cfg_.width;
  negotiated_.height = cfg_.height;
  negotiated_.frameRate = cfg_.frameRate;
  host_->onVideoConfig(negotiated_);

  // 内容加密：会话开始就生成好 2048 位 RSA 密钥对（等价于 RSAManager 的构造函数）。
  // 密钥对生成失败不是致命错：手机来要公钥时我们只能拒绝，会话仍可走明文。
  cipher_.reset();
  if (cfg_.contentEncryption) {
    if (cipher_.ensure_keypair()) {
      log("crypto", "内容加密：已生成 2048 位 RSA 密钥对，等手机的 MD_RSA_PUBLIC_KEY_REQUEST");
    } else {
      log("warn", "内容加密：RSA 密钥对生成失败，本次会话将只能走明文");
    }
  } else {
    log("crypto", "内容加密：配置为关闭（CONTENT_ENCRYPTION=0）");
  }

  running_.store(true);
  startedAt_ = std::chrono::steady_clock::now();
  lastVideoByteMs_.store(nowMs());
  lastHeartbeatAckMs_.store(nowMs());
  setState(State::Connected);

  // 第一条消息：车机协议版本（SDK 初始 context.protocolVersion = 0，手机端回自己的版本）
  pb::ProtocolVersion pv;
  pv.majorVersion = sentProtocolMajor_;
  pv.minorVersion = kProtocolMinorVersion;
  if (!sendCmd(msg::HU_PROTOCOL_VERSION, pv.encode())) {
    lastError_ = "failed to send HU_PROTOCOL_VERSION";
    setState(State::Failed);
    return false;
  }
  log("proto", "sent HU_PROTOCOL_VERSION major=" + std::to_string(pv.majorVersion) + " minor=" +
                   std::to_string(pv.minorVersion));

  hbThread_ = std::thread([this] { heartbeatLoop(); });
  snapThread_ = std::thread([this] { snapshotLoop(); });
  mainLoop();
  running_.store(false);
  tx_.close();
  return lastError_.empty();
}

void Session::mainLoop() {
  uint64_t failAtMs = 0;
  Frame f;
  while (running_.load()) {
    if (tx_.recv(&f, mic_recording_ ? 20 : 200)) {
      // 入站解密必须发生在 handleFrame 之前：否则所有解析都会拿到 AES 密文而
      // “看起来像解析失败”——那是本轮之前无法察觉的一类假故障。
      decryptInbound(f);
      handleFrame(f);
      if (f.channel == ch::CMD && f.serviceType == msg::MD_AUTH_RESPONSE && state_ == State::Failed) {
        failAtMs = nowMs() + 5000;  // 协议规定：校验不过 5 秒后车机主动断开
      }
    } else if (tx_.allClosed()) {
      log("net", "all channels closed by peer");
      break;
    }
    if (host_) {
      host_->pump();
      if (host_->quitRequested()) {
        log("ui", "quit requested");
        break;
      }
    }
    pollTouchOutput();
    pollMicrophone();
    pollVehicleReport();
    maybeInjectTouch();
    if (cfg_.seconds > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - startedAt_)
                         .count();
      if (elapsed >= cfg_.seconds) {
        log("app", "runtime limit reached (" + std::to_string(elapsed) + "s)");
        break;
      }
    }
    if (cfg_.minFrames > 0 && host_ &&
        host_->decodedFrames() >= static_cast<uint64_t>(cfg_.minFrames)) {
      log("app", "decoded " + std::to_string(cfg_.minFrames) + " frames, done");
      break;
    }
    if (failAtMs && nowMs() >= failAtMs) {
      log("app", "auth failed, closing per protocol");
      break;
    }
  }
  mic_recording_ = false;
  if (state_ != State::Failed) setState(State::Closed);
}

void Session::handleFrame(const Frame& f) {
  switch (f.channel) {
    case ch::CMD:
      switch (f.serviceType) {
        case msg::PROTOCOL_VERSION_MATCH_STATUS: handleProtocolVersionMatch(f); return;
        case msg::MD_INFO: handleMdInfo(f); return;
        case msg::MD_AUTH_RESPONSE: handleAuthResponse(f); return;
        case msg::MD_AUTH_RESULT: handleAuthResult(f); return;
        case msg::MD_FEATURE_CONFIG_REQUEST: handleFeatureConfigRequest(f); return;
        case msg::VIDEO_ENCODER_INIT_DONE: handleVideoInitDone(); return;
        case msg::MODULE_STATUS: handleModuleStatus(f); return;
        case msg::MD_VIDEO_ENCODER_REQ: log("video", "phone requests encoder re-init (keyframe)"); return;
        case msg::MD_EXIT: log("app", "phone exited CarLife"); running_.store(false); return;
        case msg::ERROR_CODE: log("warn", "phone reported ERROR_CODE"); return;
        case msg::CONNECT_EXCEPTION: log("warn", "phone reported CONNECT_EXCEPTION"); return;
        // ── A：元数据（标题/歌手/专辑/时长/封面）── AUDIT 3.7 / 5.1 #6
        case msg::MEDIA_INFO: handleMediaInfo(f); return;
        // A2：播放进度条。原来只记一行日志（等于丢弃）；现解析并转发给宿主机。
        // 依据 CarlifeMediaProgressBarProto.proto：required int32 progressBar = 1;
        case msg::MEDIA_PROGRESS_BAR: {
          pb::MediaProgressBar mp;
          if (!pb::MediaProgressBar::decode(f.payload.data(), f.payload.size(), &mp)) {
            log("media", "MEDIA_PROGRESS_BAR 解析失败（" + std::to_string(f.payload.size()) + " B）");
            return;
          }
          if (host_) host_->onMediaProgress(mp.progressBar);
          log("media", "MEDIA_PROGRESS_BAR progress=" + std::to_string(mp.progressBar) + "%");
          return;
        }
        // ── C：导航逐向 / 辅助引导 ── AUDIT 3.6 / 5.1 #7
        case msg::NAV_NEXT_TURN_INFO: handleNaviNextTurn(f); return;
        // 辅助引导有两个 ID（官方两套代码不一致），两个都接：
        //   ServiceTypes.kt 0x00018047 / CTranRecvPackageProcess.h 0x00010047
        case msg::NAV_ASSISTANT_GUIDE_INFO:
        case msg::NAV_ASSISTANT_GUIDE: handleNaviAssistantGuide(f); return;
        // 档位：C++ 库的 GEAR_INFO(0x00010029) 是【手机上报】给车机的。
        case msg::GEAR_INFO: handlePhoneGear(f); return;
        // ── J：触摸板 / 手势（罗旋钮：MSG_TOUCH_PAD_*）── AUDIT 3.5 / 5.1 #4 #5
        case msg::TOUCH_PAD_DOWN:
        case msg::TOUCH_PAD_MOVE:
        case msg::TOUCH_PAD_UP:
        case msg::TOUCH_PAD_PINCH: handleTouchPad(f.serviceType, f); return;
        // 多点触控 / 反控（定长格式，不走 protobuf）
        case msg::TOUCH_ACTION_3: handleTouchAction3(f); return;
        // ── F：电话状态 ── AUDIT 3.7 / 5.1 #14
        case msg::TEL_STATE_INCOMING:
        case msg::TEL_STATE_OUTGOING:
        case msg::TEL_STATE_IDLE:
        case msg::TEL_STATE_INCALLING: handleTelephonyState(f.serviceType); return;
        case msg::BT_HFP_CALL_STATUS_COVER: handleHfpCallStatusCover(f); return;
        // ── F/H：HFP 命令（拨号/接听/挂断/拒接/DTMF/静音）与状态请求 ──
        case msg::BT_HFP_REQUEST: handleHfpRequest(f); return;
        case msg::BT_HFP_STATUS_REQUEST: handleHfpStatusRequest(f); return;
        // ── G：通讯录 / 通话记录 ── AUDIT 3.7 / 5.1 #13
        // 【为什么这里没有 case】查遍三份参考都没找到它们的【线上消息 ID】：
        //   * Kotlin SDK  ServiceTypes.kt —— 无 CONTACTS/CALL_RECORDS
        //   * 官方 C++ 车机库 CTranRecvPackageProcess.h 的 E_PACKAGE_HEAD_TYPE 枚举 —— 无
        //   * Android 车机端 CommonParams.java —— 无
        // 而 CarlifeContacts*Proto / CarlifeCallRecords*Proto 确实存在，C++ 库里也有
        // `using CarlifeContactsList;`，但从未被任何 switch 分支使用（死声明）。
        // 结论：通讯录/通话记录在参考实现里【只有 proto，没有传输绑定】——
        // 它们由平台（Android 的 PBAP 客户端）提供，不是 CarLife 线上能力（AUDIT §4.3 同结论）。
        // 我们做了能做的部分：wire 层编解码已实现（见 wire.cpp 的 ContactsList/CallRecordsList
        // decode），并由 HostSink::onContacts/onCallRecords 接住；一旦有传输绑定即可直接用。
        // 【不自己发明一个 serviceType】—— 那会让两端各说各话。
        // ── O：车控（反向控车）── AUDIT 3.6 / 5.1 #18
        case msg::VEHICLE_CONTROL: handleVehicleControl(f); return;
        // ── 语音控制 ── AUDIT 3.3
        case msg::HU_VOICE_CONTROL: handleVoiceControl(f); return;
        case msg::MIC_RECORD_PREPARE_START:
          // 【新增】准备握手：不置 mic_recording_（准备不等于开始录音），
          // 也不动任何录音状态 —— 详见 handleMicRecordPrepare 的边界说明。
          handleMicRecordPrepare();
          return;
        case msg::MIC_RECORD_WAKEUP_START:
        case msg::MIC_RECORD_END:
        case msg::MIC_RECORD_RECOG_START:
          mic_recording_ = f.serviceType != msg::MIC_RECORD_END;
          if (host_) host_->onMicRecord(f.serviceType);
          log("vr", "MIC_RECORD 0x" + [](uint32_t v) { char b[12];
                                                       std::snprintf(b, sizeof b, "%08X", v);
                                                       return std::string(b); }(f.serviceType));
          return;
        // ── R：激活（BOX_ACTIVE）── AUDIT 3.1 / 5.1 #10
        case msg::BOX_ACTIVE: handleActivationRequest(f); return;
        // ── R：内容加密协商（3 条，全在 Ready 之前，因此不会被 AES 加密）── AUDIT 5.4
        case msg::MD_RSA_PUBLIC_KEY_REQUEST:
        case msg::MD_AES_KEY_SEND_REQUEST:
        case msg::MD_ENCRYPT_READY: handleEncryption(f.serviceType, f); return;
        // ── D：车辆数据订阅（两条路径）── AUDIT 3.6 / 5.1 #8
        case msg::CAR_DATA_SUBSCRIBE_REQ:
        case msg::CAR_DATA_SUBSCRIBE_DONE_ID:
        case msg::CAR_DATA_START_REQ:
        case msg::CAR_DATA_STOP_REQ:
        case msg::CARLIFE_DATA_REQ:
        case msg::CARLIFE_DATA_SUBSCRIBE_DONE: handleCarDataSubscribe(f.serviceType, f); return;
        // ── 时间同步（原来只打一行日志）──
        case msg::TIME_SYNC: {
          pb::ConnectTimeSync ts;
          if (pb::ConnectTimeSync::decode(f.payload.data(), f.payload.size(), &ts)) {
            if (host_) host_->onTimeSync(ts.timestamp);
            log("sync", "phone TIME_SYNC " + std::to_string(ts.timestamp));
          } else {
            log("sync", "phone TIME_SYNC（解析失败）");
          }
          return;
        }
        // ── 帧率动态调整（手机侧告知已完成）── AUDIT 3.4 / 5.1 #11
        case msg::VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE: {
          pb::VideoFrameRate fr;
          const bool ok = pb::VideoFrameRate::decode(f.payload.data(), f.payload.size(), &fr);
          if (host_) host_->onVideoControl(f.serviceType, ok ? fr.frameRate : -1);
          log("video", "phone FRAME_RATE_CHANGE_DONE frameRate=" + std::to_string(ok ? fr.frameRate : -1));
          return;
        }
        case msg::GO_TO_DESKTOP: case msg::SCREEN_ON: case msg::SCREEN_OFF:
        case msg::USER_PRESENT: case msg::FOREGROUND: case msg::BACKGROUND:
        case msg::REQUEST_GO_TO_FOREGROUND: case msg::GO_TO_FOREGROUND_RESPONSE:
          if (!f.payload.empty()) { log("app", "unexpected lifecycle payload rejected"); return; }
          try { if (host_) host_->onAppEvent(f.serviceType); }
          catch (...) { log("app", "lifecycle callback failed"); }
          return;
        default:
          if (cfg_.verbose) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "0x%08X", f.serviceType);
            log("proto", "unhandled CMD " + std::string(buf) + " (" + std::to_string(f.payload.size()) + " B)");
          }
          return;
      }
    case ch::VIDEO:
      lastHeartbeatAckMs_.store(nowMs());
      if (f.serviceType == msg::VIDEO_DATA) {
        handleVideoData(f);
      } else if (f.serviceType == msg::VIDEO_HEARTBEAT) {
        lastVideoByteMs_.store(nowMs());
      } else if (cfg_.verbose) {
        log("video", "video-channel msg 0x" + [](uint32_t v) { char b[12]; std::snprintf(b, sizeof b, "%08X", v); return std::string(b); }(f.serviceType));
      }
      return;
    case ch::AUDIO:
      if (f.serviceType == msg::MEDIA_INIT) {
        handleMediaInit(f);
      } else if (f.serviceType == msg::MEDIA_DATA || f.serviceType == msg::MEDIA_DATA_ENCODER) {
        if (host_ && !f.payload.empty()) host_->onAudioPcm(ch::AUDIO, f.payload.data(), f.payload.size());
      } else if (f.serviceType == msg::MEDIA_STOP || f.serviceType == msg::MEDIA_PAUSE) {
        log("media", "media stopped/paused by phone");
        if (host_) host_->onAudioEnd(ch::AUDIO);
      } else if (cfg_.verbose) {
        log("media", "media msg 0x" + [](uint32_t v) { char b[12]; std::snprintf(b, sizeof b, "%08X", v); return std::string(b); }(f.serviceType));
      }
      return;
    case ch::TTS:
      if (f.serviceType == msg::NAV_TTS_INIT) {
        pb::AudioInit ai;
        pb::AudioInit::decode(f.payload.data(), f.payload.size(), &ai);
        const int channels = (ai.channelConfig == 12) ? 2 : ((ai.channelConfig == 4) ? 1 : 2);
        log("tts", "TTS init " + std::to_string(ai.sampleRate) + "Hz fmt=" + std::to_string(ai.sampleFormat));
        if (host_) host_->onAudioInit(ch::TTS, ai.sampleRate, channels, ai.sampleFormat);
      } else if (f.serviceType == msg::NAV_TTS_DATA || f.serviceType == msg::NAV_TTS_DATA_ENCODE) {
        ttsBytes_.fetch_add(f.payload.size());
        if (host_ && !f.payload.empty()) host_->onAudioPcm(ch::TTS, f.payload.data(), f.payload.size());
        if (cfg_.verbose) log("tts", "TTS data " + std::to_string(f.payload.size()) + " B");
      } else if (f.serviceType == msg::NAV_TTS_END) {
        if (host_) host_->onAudioEnd(ch::TTS);
      }
      return;
    case ch::VR:
      // VR_AUDIO_* is downstream playback; VR_DATA is upstream microphone PCM.
      // Reference: ServiceTypes.kt, VoiceMessageHandler.kt and PcmSender.kt.
      if (f.serviceType == msg::VR_AUDIO_INIT) {
        pb::AudioInit info;
        if (pb::AudioInit::decode(f.payload.data(), f.payload.size(), &info) && host_)
          host_->onAudioInit(ch::VR, info.sampleRate, info.channelConfig == 4 ? 1 : 2, info.sampleFormat);
      } else if (f.serviceType == msg::VR_AUDIO_DATA) {
        if (host_ && !f.payload.empty()) host_->onAudioPcm(ch::VR, f.payload.data(), f.payload.size());
      } else if (f.serviceType == msg::VR_AUDIO_STOP || f.serviceType == msg::VR_AUDIO_INTERRUPT) {
        if (host_) host_->onAudioEnd(ch::VR);
      }
      return;
    case ch::UPDATE:
      // A3：UPDATE 通道（MD 9440 / HU 9400）承载固件与文件传输类消息。
      // 参考实现里对应 DATA_MD_TRANSFER_* 与 DATA_HU_UPDATE_*（见 service_types.h）。
      // Q 项：本轮它们不再只是“收到即记录”——已接 HostSink::onFileTransfer，
      // 由适配器维护 LinkState 的 file_transfer_*/ota_state 并上报到 Core。
      // 【绝不】因为不认识就断会话（原来落到 default 静默丢弃，无法判断通道是否在用）。
      updateMsgs_.fetch_add(1);
      updateBytes_.fetch_add(f.payload.size());
      switch (f.serviceType) {
        case msg::DATA_MD_TRANSFER_START:
        case msg::DATA_MD_TRANSFER_SEND:
        case msg::DATA_MD_TRANSFER_END:
        case msg::DATA_HU_UPDATE_START:
        case msg::DATA_HU_UPDATE_END: {
          // 只有 START 带 CarlifeFileTransferBegin{fileSize, version}。
          int64_t total = 0;
          if (f.serviceType == msg::DATA_MD_TRANSFER_START) {
            pb::FileTransferBegin begin;
            if (pb::FileTransferBegin::decode(f.payload.data(), f.payload.size(), &begin)) {
              total = begin.file_size;
            }
          }
          if (host_) host_->onFileTransfer(f.serviceType, total, f.payload.size());
          log("update", "文件传输/OTA msg=0x" +
                            [](uint32_t v) {
                              char b[12];
                              std::snprintf(b, sizeof b, "%08X", v);
                              return std::string(b);
                            }(f.serviceType) +
                            " total=" + std::to_string(total) + "B chunk=" +
                            std::to_string(f.payload.size()) + "B");
          return;
        }
        default:
          // G 类的其余消息（SEND_START/SENDING_DATA/SEND_FINISH/SEND_STOP/STOP_RECEIVE）
          if (f.serviceType == msg::SEND_START || f.serviceType == msg::SENDING_DATA ||
              f.serviceType == msg::SEND_FINISH || f.serviceType == msg::SEND_STOP ||
              f.serviceType == msg::STOP_RECEIVE) {
            if (host_) host_->onFileTransfer(f.serviceType, 0, f.payload.size());
            log("update", "文件传输 msg=0x" +
                              [](uint32_t v) {
                                char b[12];
                                std::snprintf(b, sizeof b, "%08X", v);
                                return std::string(b);
                              }(f.serviceType) +
                              " " + std::to_string(f.payload.size()) + "B");
            return;
          }
          log("update", "UPDATE 通道 msg 0x" +
                            [](uint32_t v) {
                              char b[12];
                              std::snprintf(b, sizeof b, "%08X", v);
                              return std::string(b);
                            }(f.serviceType) +
                            " " + std::to_string(f.payload.size()) + " B");
          return;
      }
    case ch::TOUCH:
      // 【TOUCH 通道也可能收到 ACTION_3】mdsim 与真机都会在这条通道上做“反控”。
      // 原来这里只打一行 verbose 日志就丢弃；现在真正的定长解析在下面。
      if (f.serviceType == msg::TOUCH_ACTION_3) {
        handleTouchAction3(f);
        return;
      }
      if (cfg_.verbose) log("touch", "inbound touch msg");
      return;
    default:
      return;
  }
}

void Session::handleProtocolVersionMatch(const Frame& f) {
  pb::ProtocolVersionMatchStatus st;
  if (!pb::ProtocolVersionMatchStatus::decode(f.payload.data(), f.payload.size(), &st)) {
    lastError_ = "cannot parse PROTOCOL_VERSION_MATCH_STATUS";
    setState(State::Failed);
    running_.store(false);
    return;
  }
  adoptedProtocolVersion_ = st.carlifeProtocolVersion;
  log("proto", "phone matchStatus=" + std::to_string(st.matchStatus) +
                   " carlifeProtocolVersion=" + std::to_string(st.carlifeProtocolVersion));
  if (st.matchStatus != msg::PROTOCOL_VERSION_MATCH) {
    // 【真机实测教训】我们原来固定发 major=0，手机直接回 NOT_MATCH(2) 并告知它的版本。
    // 参考实现里根本没有 0 这个版本：
    //   PROTOCOL_VERSION_MAJOR_VERSION_1..4 = 1..4，MINOR = 1，MATCH = 1，NOT_MATCH = 2
    // 而 CarLifeReceiverImpl 里 protocolVersion 是【可配置的】，默认 major=4。
    // 手机既然把它的版本告诉我们了，就先用它重发一次（自适配），
    // 只有连手机的版本也不接受才判失败 —— 不把“我声明错了”当成“协议不支持”。
    if (st.carlifeProtocolVersion > 0 && st.carlifeProtocolVersion != sentProtocolMajor_) {
      sentProtocolMajor_ = st.carlifeProtocolVersion;
      pb::ProtocolVersion pv;
      pv.majorVersion = sentProtocolMajor_;
      pv.minorVersion = kProtocolMinorVersion;
      sendCmd(msg::HU_PROTOCOL_VERSION, pv.encode());
      log("proto", "改用手机告知的版本重发 HU_PROTOCOL_VERSION major=" +
                       std::to_string(sentProtocolMajor_));
      return;
    }
    lastError_ = "phone rejected protocol version";
    setState(State::Failed);
    running_.store(false);
    return;
  }
  pb::StatisticsInfo si;
  si.cuid = cfg_.cuid;
  si.versionName = cfg_.versionName;
  si.versionCode = 1;
  si.channel = cfg_.channelId;
  si.connectCount = 1;
  si.connectSuccessCount = 1;
  si.connectTime = static_cast<int32_t>(std::time(nullptr));
  sendCmd(msg::STATISTIC_INFO, si.encode());
  setState(State::VersionMatched);
}

void Session::handleMdInfo(const Frame& f) {
  if (!pb::DeviceInfo::decode(f.payload.data(), f.payload.size(), &mdInfo_)) {
    log("warn", "cannot parse MD_INFO");
  }
  log("proto", "MD_INFO brand=" + mdInfo_.brand + " model=" + mdInfo_.model +
                   " sdk=" + mdInfo_.sdk + " carlifeVersion=" + mdInfo_.carlifeVersion +
                   " token=" + (mdInfo_.token.empty() ? "(none)" : mdInfo_.token));

  pb::DeviceInfo hu;
  hu.os = "Linux";
  hu.board = "zero2w";
  hu.bootloader = "u-boot";
  hu.brand = cfg_.brand;
  hu.cpuAbi = "x86_64";
  hu.device = "carlife-hu";
  hu.display = cfg_.huName;
  hu.fingerprint = cfg_.brand + "/carlife-hu/zero2w";
  hu.hardware = "zero2w";
  hu.host = "wsl";
  hu.cid = "1";
  hu.manufacturer = "zero2w";
  hu.model = cfg_.model;
  hu.product = "carlife-hu";
  hu.serial = cfg_.cuid;
  hu.codename = "release";
  hu.incremental = "1";
  hu.release = "1.0.0";
  hu.sdk = "33";
  hu.sdkInt = 33;
  hu.btAddress = cfg_.btMac;
  hu.carlifeVersion = kSdkVersionCode;
  sendCmd(msg::HU_INFO, hu.encode());
  log("proto", "sent HU_INFO (Linux head unit, bt=" + cfg_.btMac + ")");

  // connectTime 的格式必须与参考实现【完全一致】（util/Dates.kt）：
  //     const val TIME_FORMAT_FOR_HMS = "yyyy-MM-dd-HH-mm-ss"
  // randomValue = SDK_VERSION_CODE + ";" + connectTime
  //             → 形如 "2.0;2026-09-11-12-28-39"
  //
  // 【真机实测教训】原来用 hhmmss() 拼成 "2.0;12:28:39"：
  // 手机无法解析这个时间，于是直接跳过 MD_AUTH_RESPONSE、回
  // MD_AUTH_RESULT{authenResult=false} —— 鉴权必然失败。
  // 日志里能看到它连 encryptValue 都没发就判了失败。
  {
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char buf[40];
    if (std::strftime(buf, sizeof buf, "%Y-%m-%d-%H-%M-%S", &tmv) > 0) {
      authConnectTime_ = buf;
    } else {
      authConnectTime_ = hhmmss();
    }
  }
  pb::AuthenRequest ar;
  ar.randomValue = std::string(kSdkVersionCode) + ";" + authConnectTime_;
  authSeed_ = mdInfo_.brand + mdInfo_.model + mdInfo_.sdk + authConnectTime_;
  if (!sendCmd(msg::HU_AUTH_REQUEST, ar.encode())) {
    lastError_ = "cannot send HU_AUTH_REQUEST";
    setState(State::Failed);
    running_.store(false);
    return;
  }
  log("auth", "sent HU_AUTH_REQUEST randomValue=\"" + ar.randomValue + "\" (30s timeout)");
  setState(State::AuthRequested);
}

bool Session::verifyAuth(const std::string& seed, const std::string& encryptValue) {
  if (verify_seam_) {
    try {
      const bool ok = verify_seam_->verify(seed, encryptValue);
      log("auth", ok ? "verify seam => PASS" : "verify seam => FAIL");
      return ok;
    } catch (...) {
      // Callback exception messages can contain credential material.
      log("auth", "verify seam threw => fail closed");
      return false;
    }
  }
  if (cfg_.authMode == "trust") {
    log("auth", "mode=trust: 车机自判通过（真机上此步应调用 libencryption.so getVerifyResult）");
    return true;
  }
  if (cfg_.authMode == "deny") {
    log("auth", "mode=deny: 故意判失败，用于验证协议规定的 5 秒断开路径");
    return false;
  }
  if (cfg_.authMode == "sdk") {
    SdkPhoneVerifier verifier;
    const bool ok = verifier.verify(seed, encryptValue);
    log("auth", ok ? "mode=sdk => PASS" : "mode=sdk => FAIL");
    return ok;
  }
  if (cfg_.authMode != "dev") return false;
  const std::string expect = devDigest(cfg_.authSecret + "|" + seed);
  const bool ok = (expect == encryptValue);
  log("auth", ok ? "mode=dev => PASS" : "mode=dev => FAIL");
  return ok;
}

void Session::handleAuthResponse(const Frame& f) {
  pb::AuthenResponse resp;
  if (!pb::AuthenResponse::decode(f.payload.data(), f.payload.size(), &resp)) {
    lastError_ = "cannot parse MD_AUTH_RESPONSE";
    setState(State::Failed);
    running_.store(false);
    return;
  }
  log("auth", "got MD_AUTH_RESPONSE");
  const bool ok = verifyAuth(authSeed_, resp.encryptValue);
  pb::AuthenResult res;
  res.authenResult = ok;
  sendCmd(msg::HU_AUTH_RESULT, res.encode());
  log("auth", std::string("sent HU_AUTH_RESULT authenResult=") + (ok ? "true" : "false"));
  if (ok) {
    setState(State::AuthVerified);
  } else {
    lastError_ = "phone failed HU-side authentication";
    setState(State::Failed);
  }
}

void Session::handleAuthResult(const Frame& f) {
  pb::AuthenResult res;
  pb::AuthenResult::decode(f.payload.data(), f.payload.size(), &res);
  sendCmd(msg::MD_AUTH_RESULT_RESPONSE);
  log("auth", std::string("phone reported MD_AUTH_RESULT authenResult=") + (res.authenResult ? "true" : "false"));
  if (!res.authenResult) {
    lastError_ = "phone refused the session (MD_AUTH_RESULT=false)";
    setState(State::Failed);
    running_.store(false);
    return;
  }
  onEstablished();
}

void Session::handleFeatureConfigRequest(const Frame& f) {
  pb::FeatureConfigList req;
  pb::FeatureConfigList::decode(f.payload.data(), f.payload.size(), &req);
  std::ostringstream os;
  for (size_t i = 0; i < req.configs.size(); ++i) {
    if (i) os << ", ";
    os << req.configs[i].key << "=" << req.configs[i].value;
  }
  log("feature", "MD_FEATURE_CONFIG_REQUEST {" + os.str() + "}");

  const pb::FeatureConfigList resp = buildFeatureConfig();
  sendCmd(msg::HU_FEATURE_CONFIG_RESPONSE, resp.encode());
  log("feature", "sent HU_FEATURE_CONFIG_RESPONSE (btName=" + cfg_.btName + ", btMAC=" + cfg_.btMac + ", CONNECT_TYPE=2)");
}

// 能力协商表。除 CONTENT_ENCRYPTION / MULTI_TOUCH 两项（本轮修的两处缺陷）之外，
// 其余位的取值与修复前逐字节一致 —— 不趁机扩大任何已宣告的能力。
pb::FeatureConfigList Session::buildFeatureConfig() const {
  // 内容加密：报 1 等于向手机承诺“可以走 RSA 交换 AES”。只有同时满足
  //   ① 配置允许（cfg_.contentEncryption），且
  //   ② 本会话真的持有 RSA 密钥对（cipher_.has_keypair()；OpenSSL 不可用或生成失败时为假）
  // 才报 1；否则报 0，手机就不会来要一个我们给不出的公钥。
  const int32_t content_encryption = (cfg_.contentEncryption && cipher_.has_keypair()) ? 1 : 0;
  pb::FeatureConfigList resp;
  resp.configs = {
      {feature::kContentEncryption, content_encryption},
      {feature::kConnectType, 2},         // 2 = 无线
      {feature::kMultiTouch, multiTouch_ ? 1 : 0},
      {feature::kUsbMtu, 16384},
      {feature::kIFrameInterval, 1},
      {feature::kAacSupport, 0},
      {feature::kAudioTransmissionMode, 0},
      {feature::kMediaSampleRate, 44100},
      {feature::kVoiceMic, host_ && host_->microphoneAvailable() ? 1 : 0},
      {feature::kVoiceWakeup, 0},
      {feature::kBluetoothAutoPair, 1},
      {feature::kBluetoothInternalUi, 1},
      {feature::kFocusAreaAutoSet, 1},
      {feature::kFocusUi, 1},
      {feature::kInputDisable, 0},
      {feature::kMusicHud, 0},
      {feature::kEngineType, 0},
  };
  resp.huBtAudioSupport = true;
  resp.huBtName = cfg_.btName;
  resp.huBtMac = cfg_.btMac;
  return resp;
}

void Session::onEstablished() {
  setState(State::Established);
  negotiated_ = VideoInfo{cfg_.width, cfg_.height, cfg_.frameRate};
  pb::VideoEncoderInfo vi;
  vi.width = cfg_.width;
  vi.height = cfg_.height;
  vi.frameRate = cfg_.frameRate;
  sendCmd(msg::VIDEO_ENCODER_INIT, vi.encode());
  log("video", "sent VIDEO_ENCODER_INIT " + std::to_string(vi.width) + "x" + std::to_string(vi.height) +
                   "@" + std::to_string(vi.frameRate) + " (默认协商分辨率)");
  setState(State::VideoInitSent);
}

void Session::handleVideoInitDone() {
  sendCmd(msg::VIDEO_ENCODER_START);
  log("video", "phone VIDEO_ENCODER_INIT_DONE -> sent VIDEO_ENCODER_START");
  setState(State::VideoStarted);
}

void Session::handleVideoData(const Frame& f) {
  framesReceived_.fetch_add(1);
  videoBytes_.fetch_add(f.payload.size());
  lastVideoByteMs_.store(nowMs());
  // 不解码：Annex-B 直接交给宿主机，由它决定本机呈现还是转发给 Core。
  if (host_ && !f.payload.empty()) host_->onVideoAnnexB(f.payload.data(), f.payload.size());
}

void Session::handleMediaInit(const Frame& f) {
  pb::AudioInit ai;
  pb::AudioInit::decode(f.payload.data(), f.payload.size(), &ai);
  const int channels = (ai.channelConfig == 12) ? 2 : ((ai.channelConfig == 4) ? 1 : 2);
  if (ai.sampleFormat == 2) {  // ENCODING_PCM_16BIT
    log("audio", "MEDIA_INIT PCM " + std::to_string(ai.sampleRate) + "Hz " + std::to_string(channels) +
                     "ch -> 交由宿主机输出");
  } else {
    log("warn", "MEDIA_INIT sampleFormat=" + std::to_string(ai.sampleFormat) +
                    " (非 PCM16，暂不解码，仅计数)");
  }
  if (host_) host_->onAudioInit(ch::AUDIO, ai.sampleRate, channels, ai.sampleFormat);
}

void Session::handleModuleStatus(const Frame& f) {
  pb::ModuleStatusList m;
  if (!pb::ModuleStatusList::decode(f.payload.data(), f.payload.size(), &m)) {
    // 畸形载荷【不】触发宿主机回调：宁可不上报，也不把半解出来的状态当成真实模块状态。
    log("warn", "MODULE_STATUS 解析失败（" + std::to_string(f.payload.size()) + " B）");
    return;
  }
  std::ostringstream os;
  for (auto& it : m.items) os << " module" << it.first << "=" << it.second;
  const std::string detail = "MODULE_STATUS" + os.str();
  log("module", detail);
  // 解码成功就必须交给宿主机（原来只打日志，Core 侧永远看不到）；
  // detail 与上面那行日志是同一个字符串，便于两边对照。
  if (host_) host_->onModuleStatus(detail);
}

// VR 上行：向宿主机要一帧 PCM16，容量校验通过才拷进载荷。
// 【为什么单独抽一个函数】宿主机实现不受本类控制，它回报的 frames 可能超过我们给的
// 容量；未校验就 memcpy 会把 micBuffer_ 之外的内存读进包里（栈上数据上线）。
// 这里把“容量校验 + 构包”放一处，由录音命令启用的 pollMicrophone 调用，
// 也便于单测直接断言越界帧被丢弃。返回 false 时 payload 必为空。
bool Session::takeMicFrame(std::vector<uint8_t>* payload) {
  if (!payload) return false;
  payload->clear();
  if (!host_) return false;
  std::size_t frames = 0;
  if (!host_->takeMicrophone(micBuffer_.data(), micBuffer_.size(), frames)) return false;
  if (frames == 0) return false;
  if (frames > micBuffer_.size()) {
    log("warn", "uplink mic frames=" + std::to_string(frames) + " 超过缓冲 " +
                    std::to_string(micBuffer_.size()) + " 帧 → 丢弃（不发送）");
    return false;
  }
  payload->resize(frames * 2);
  std::memcpy(payload->data(), micBuffer_.data(), frames * 2);
  return true;
}

void Session::pollMicrophone() {
  if (!mic_recording_) return;
  micRequests_.fetch_add(1);
  std::vector<uint8_t> pcm;
  if (!takeMicFrame(&pcm)) return;
  const auto frames = pcm.size() / sizeof(int16_t);
  if (sendOn(ch::VR, msg::VR_DATA, std::move(pcm))) micFrames_.fetch_add(frames);
}

void Session::pollTouchOutput() {
  if (!host_) return;
  HostControl c;
  while (host_->takeControl(c)) {
    // 命令型控制（转前台 0x00018025 / 模块控制 0x00018028）：先在这里发出去。
    // 【必须 continue】不然会掉进下面的触控分支，把一条命令当成坐标发出去。
    ControlCommand command;
    if (encodeControlCommand(c, &command)) {
      const bool ok = sendCmd(command.service_type, std::move(command.payload));
      log("cmd", (c.type == HostControl::Type::Foreground
                       ? std::string("转前台 GO_TO_FOREGROUND(0x18025) 空载荷")
                       : std::string("模块控制 MODULE_CONTROL(0x18028) module=") +
                             std::to_string(c.module_id) + " status=" +
                             std::to_string(c.status_id)) +
                      (ok ? " sent" : " FAILED"));
      continue;
    }
    if (c.type == HostControl::Type::Foreground || c.type == HostControl::Type::ModuleControl) {
      log("cmd", "invalid command control rejected");
      continue;
    }
    if (c.type == HostControl::Type::Key) {
      // 【重要】CarHardKeyCode 用的是 CarLife 自己的 KEYCODE_* 表
      // （service_types.h 的 keycode 命名空间），不是 Android 的 ADB keycode。
      sendOn(ch::TOUCH, msg::TOUCH_CAR_HARD_KEY_CODE, pb::CarHardKeyCode{c.keycode}.encode());
      log("touch", "hard keycode=" + std::to_string(c.keycode));
      continue;
    }
    if (c.type == HostControl::Type::MultiTouch) {
      // 多点触控（AUDIT 3.5 / 5.1 #4）：按参考实现用 MSG_TOUCH_ACTION_3 定长格式
      // （RemoteControlManager.kt:76-104）。action 是 Android MotionEvent 的 action，
      // 手指数量已包含在 body 里，所以一条消息就能表达多指。
      pb::TouchAction3 ev;
      if (c.phase == HostControl::TouchPhase::Down) {
        ev.action = msg::motion::ACTION_DOWN;
      } else if (c.phase == HostControl::TouchPhase::Up) {
        ev.action = msg::motion::ACTION_UP;
      } else {
        ev.action = msg::motion::ACTION_MOVE;
      }
      for (uint8_t i = 0; i < c.point_count && i < HostControl::kMaxPoints; ++i) {
        pb::TouchAction3::Pointer pt;
        pt.id = c.points[i].id;
        pt.x = c.points[i].x;
        pt.y = c.points[i].y;
        ev.pointers.push_back(pt);
      }
      const bool ok = sendOn(ch::TOUCH, msg::TOUCH_ACTION_3, ev.encode());
      log("touch", "多点触控回传 " + std::to_string(ev.pointers.size()) + " 点 action=" +
                       std::to_string(ev.action) + (ok ? " sent" : " FAILED"));
      continue;
    }
    // 单点触控。
    // 【格式选择照抄 RemoteControlManager.kt:61-69】：
    //   能力位 MULTI_TOUCH=1 → 所有触控走 ACTION_3（手机会按多指格式解析）；
    //   否则回退到旧的 TOUCH_ACTION_DOWN/UP/MOVE + TouchSinglePoint
    //   （后者与官方 C++ 车机库 CTranRecvPackageProcess.h 的消息集一致）。
    if (multiTouch_) {
      pb::TouchAction3 ev;
      if (c.phase == HostControl::TouchPhase::Down) {
        ev.action = msg::motion::ACTION_DOWN;
      } else if (c.phase == HostControl::TouchPhase::Up) {
        ev.action = msg::motion::ACTION_UP;
      } else {
        ev.action = msg::motion::ACTION_MOVE;
      }
      pb::TouchAction3::Pointer pt;
      pt.id = 0;
      pt.x = c.x;
      pt.y = c.y;
      ev.pointers.push_back(pt);
      const bool ok = sendOn(ch::TOUCH, msg::TOUCH_ACTION_3, ev.encode());
      if (c.phase != HostControl::TouchPhase::Move) {
        log("touch", "车机->手机 触控回传 action=" + std::to_string(ev.action) + " (" +
                         std::to_string(c.x) + "," + std::to_string(c.y) + ") on " +
                         std::to_string(negotiated_.width) + "x" +
                         std::to_string(negotiated_.height) + (ok ? " sent" : " FAILED"));
      }
      continue;
    }
    pb::TouchSinglePoint tp;
    tp.x = c.x;
    tp.y = c.y;
    tp.pointerX = c.x;
    tp.pointerY = c.y;
    uint32_t type = msg::TOUCH_ACTION_UP;
    const char* name = "up";
    if (c.phase == HostControl::TouchPhase::Down) {
      type = msg::TOUCH_ACTION_DOWN;
      name = "down";
    } else if (c.phase == HostControl::TouchPhase::Move) {
      type = msg::TOUCH_ACTION_MOVE;
      name = "move";
    }
    const bool ok = sendOn(ch::TOUCH, type, tp.encode());
    // 拖动阶段不记日志，避免刷屏。
    if (c.phase != HostControl::TouchPhase::Move) {
      log("touch", std::string("车机->手机 触控回传 ") + name + " (" + std::to_string(c.x) + "," +
                       std::to_string(c.y) + ") on " + std::to_string(negotiated_.width) + "x" +
                       std::to_string(negotiated_.height) + (ok ? " sent" : " FAILED"));
    }
  }
}

void Session::maybeInjectTouch() {
  if (touchInjected_ || cfg_.injectTouch.empty()) return;
  if (static_cast<int>(state_) < static_cast<int>(State::VideoStarted)) return;
  touchInjected_ = true;
  int x = 0;
  int y = 0;
  if (std::sscanf(cfg_.injectTouch.c_str(), "%d,%d", &x, &y) != 2) {
    log("touch", "--inject-touch 格式应为 x,y");
    return;
  }
  pb::TouchSinglePoint tp;
  const int w = negotiated_.width > 0 ? negotiated_.width : cfg_.width;
  const int h = negotiated_.height > 0 ? negotiated_.height : cfg_.height;
  tp.x = static_cast<int32_t>(x * w / 1000);   // 0..1000 千分比 -> 实际像素
  tp.y = static_cast<int32_t>(y * h / 1000);
  tp.pointerX = tp.x;
  tp.pointerY = tp.y;
  const bool ok = tx_.send(ch::TOUCH, msg::TOUCH_ACTION_DOWN, tp.encode()) &&
                  tx_.send(ch::TOUCH, msg::TOUCH_ACTION_UP, tp.encode());
  log("touch", "注入触控 千分比(" + std::to_string(x) + "," + std::to_string(y) + ") -> 像素(" +
                   std::to_string(tp.x) + "," + std::to_string(tp.y) + ") " + (ok ? "已下发" : "下发失败"));
}

// ────────────────────────────────────────────────────────────────────
// A..U：本轮新增的全部解析器（MD->HU）
// 统一的健壮性约定：解析失败只记日志并返回，绝不断会话；
// 不认识的字段由 wire.cpp 的 decode 自行 skip。
// ────────────────────────────────────────────────────────────────────

// A：媒体元数据（CarlifeMediaInfo：source/song/artist/album/albumArt/duration/…）
void Session::handleMediaInfo(const Frame& f) {
  pb::MediaInfo mi;
  if (!pb::MediaInfo::decode(f.payload.data(), f.payload.size(), &mi)) {
    log("media", "MEDIA_INFO 解析失败（" + std::to_string(f.payload.size()) + " B）");
    return;
  }
  mediaInfoCount_.fetch_add(1);
  if (host_) host_->onMediaInfo(mi);
  log("media", "MEDIA_INFO song=\"" + mi.song + "\" artist=\"" + mi.artist + "\" album=\"" +
                   mi.album + "\" duration=" + std::to_string(mi.duration) +
                   "ms cover=" + std::to_string(mi.album_art.size()) + "B source=" + mi.source);
}

// C：导航逐向（action/nextTurn/roadName/totalDistance/remainDistance/turnIconData）
void Session::handleNaviNextTurn(const Frame& f) {
  pb::NaviNextTurnInfo ni;
  if (!pb::NaviNextTurnInfo::decode(f.payload.data(), f.payload.size(), &ni)) {
    log("navi", "NAV_NEXT_TURN_INFO 解析失败（" + std::to_string(f.payload.size()) + " B）");
    return;
  }
  naviCount_.fetch_add(1);
  if (host_) host_->onNaviNextTurn(ni);
  log("navi", "NAV_NEXT_TURN action=" + std::to_string(ni.action) + " nextTurn=" +
                   std::to_string(ni.next_turn) + " road=\"" + ni.road_name + "\" remain=" +
                   std::to_string(ni.remain_distance) + "m total=" +
                   std::to_string(ni.total_distance) + "m icon=" +
                   std::to_string(ni.turn_icon.size()) + "B");
}

// C：导航辅助引导（action/assistantType/trafficSignType/…/cameraSpeed）
void Session::handleNaviAssistantGuide(const Frame& f) {
  pb::NaviAssistantGuideInfo gi;
  if (!pb::NaviAssistantGuideInfo::decode(f.payload.data(), f.payload.size(), &gi)) {
    log("navi", "NAV_ASSISTANT_GUIDE 解析失败（" + std::to_string(f.payload.size()) + " B）");
    return;
  }
  naviCount_.fetch_add(1);
  if (host_) host_->onNaviAssistantGuide(gi);
  log("navi", "NAV_ASSISTANT_GUIDE action=" + std::to_string(gi.action) + " type=" +
                   std::to_string(gi.assistant_type) + " sign=" +
                   std::to_string(gi.traffic_sign_type) + " cameraSpeed=" +
                   std::to_string(gi.camera_speed));
}

// D：手机上报档位（C++ 库的 MSG_CMD_GEAR_INFO = 0x00010029）
// 注意与 CAR_GEAR(0x00018029, HU->MD) 相反：这条是手机把档位告诉我们。
void Session::handlePhoneGear(const Frame& f) {
  pb::GearInfo gi;
  if (!pb::GearInfo::decode(f.payload.data(), f.payload.size(), &gi)) {
    log("car", "GEAR_INFO 解析失败");
    return;
  }
  if (host_) host_->onGearFromPhone(gi.gear);
  log("car", "手机上报档位 gear=" + std::to_string(gi.gear));
}

// J：触摸板（MSG_TOUCH_PAD_*），对应 CarLifeTouchPadActionProto（package com.yftech）
void Session::handleTouchPad(uint32_t service_type, const Frame& f) {
  phoneInputCount_.fetch_add(1);
  if (service_type == msg::TOUCH_PAD_MOVE) {
    pb::TouchPadMove mv;
    if (!pb::TouchPadMove::decode(f.payload.data(), f.payload.size(), &mv)) {
      log("input", "TOUCH_PAD_MOVE 解析失败");
      return;
    }
    if (host_) {
      host_->onPhoneInput(HostSink::PhoneInputKind::TouchPadMove, mv.delta_x, mv.delta_y, 0);
    }
    log("input", "[旋钮/触摸板] TOUCH_PAD_MOVE delta=(" + std::to_string(mv.delta_x) + "," +
                      std::to_string(mv.delta_y) + ")");
    return;
  }
  if (service_type == msg::TOUCH_PAD_PINCH) {
    pb::TouchPadPinch pc;
    if (!pb::TouchPadPinch::decode(f.payload.data(), f.payload.size(), &pc)) {
      log("input", "TOUCH_PAD_PINCH 解析失败");
      return;
    }
    if (host_) {
      host_->onPhoneInput(HostSink::PhoneInputKind::TouchPadPinch, 0, 0, pc.scale);
    }
    log("input", "[手势] TOUCH_PAD_PINCH scale=" + std::to_string(pc.scale));
    return;
  }
  pb::TouchPadSimple sp;
  if (!pb::TouchPadSimple::decode(f.payload.data(), f.payload.size(), &sp)) {
    log("input", "TOUCH_PAD 解析失败（" + std::to_string(f.payload.size()) + " B）");
    return;
  }
  HostSink::PhoneInputKind kind = HostSink::PhoneInputKind::TouchPadDown;
  const char* name = "TOUCH_PAD_DOWN";
  if (service_type == msg::TOUCH_PAD_UP) {
    kind = HostSink::PhoneInputKind::TouchPadUp;
    name = "TOUCH_PAD_UP";
  }
  if (host_) host_->onPhoneInput(kind, 0, 0, 0);
  log("input", std::string("[触摸板] ") + name + " timestamp=" + std::to_string(sp.timestamp));
}

// J：滚动 / 惯性（CarlifeTouchScroll / CarlifeTouchFling）的编解码在 wire.cpp 已实现并有单测覆盖，
// 但【不在这里做分发】：两套官方代码都没有给它们分配 serviceType
// （ServiceTypes.kt 的 Touch 消息段里没有 SCROLL/FLING，C++ 库的排举里也没有），
// 所以无法写一个“真能收到”的 case。不自己发明一个 ID。

// J：多点触控（MSG_TOUCH_ACTION_3，定长格式）。
// 【为什么要接这条】RemoteControlManager.kt 的注释直接说它是“新的反控事件 service type”——
// 手机侧也能用它把多指事件回给车机，所以两个方向都实现。
void Session::handleTouchAction3(const Frame& f) {
  pb::TouchAction3 ev;
  if (!pb::TouchAction3::decode(f.payload.data(), f.payload.size(), &ev)) {
    log("input", "TOUCH_ACTION_3 解析失败（" + std::to_string(f.payload.size()) +
                     " B，应为 4+5n 字节）");
    return;
  }
  multiTouchCount_.fetch_add(1);
  phoneInputCount_.fetch_add(1);
  if (host_) host_->onPhoneTouchAction3(ev);
  log("input", "TOUCH_ACTION_3 action=" + std::to_string(ev.action) + " pointers=" +
                    std::to_string(ev.pointers.size()));
}

// F：电话状态（TEL_STATE_INCOMING/OUTGOING/IDLE/INCALLING，无载荷）
void Session::handleTelephonyState(uint32_t service_type) {
  hfpCount_.fetch_add(1);
  int32_t state = 0;
  const char* name = "IDLE";
  if (service_type == msg::TEL_STATE_INCOMING) {
    state = 1;
    name = "INCOMING";
  } else if (service_type == msg::TEL_STATE_OUTGOING) {
    state = 2;
    name = "OUTGOING";
  } else if (service_type == msg::TEL_STATE_INCALLING) {
    state = 3;
    name = "INCALLING";
  }
  if (host_) host_->onTelephony(service_type, state, std::string(), std::string());
  log("tel", std::string("TEL_STATE_") + name);
}

// F：手机让我们做的 HFP 动作（拨号/挂断/接听/拒接/DTMF/静音）
void Session::handleHfpRequest(const Frame& f) {
  pb::BTHfpRequest req;
  if (!pb::BTHfpRequest::decode(f.payload.data(), f.payload.size(), &req)) {
    log("tel", "BT_HFP_REQUEST 解析失败（" + std::to_string(f.payload.size()) + " B）");
    return;
  }
  hfpCount_.fetch_add(1);
  if (host_) host_->onHfpRequest(req);
  const char* what = "?";
  switch (req.command) {
    case hfp::REQ_START_CALL: what = "START_CALL"; break;
    case hfp::REQ_TERMINATE_CALL: what = "TERMINATE_CALL"; break;
    case hfp::REQ_ANSWER_CALL: what = "ANSWER_CALL"; break;
    case hfp::REQ_REJECT_CALL: what = "REJECT_CALL"; break;
    case hfp::REQ_DTMF_CODE: what = "DTMF"; break;
    case hfp::REQ_MUTE_MIC: what = "MUTE_MIC"; break;
    case hfp::REQ_UNMUTE_MIC: what = "UNMUTE_MIC"; break;
    default: break;
  }
  log("tel", std::string("BT_HFP_REQUEST ") + what +
                  (req.phone_num.empty() ? "" : " num=" + req.phone_num) +
                  (req.command == hfp::REQ_DTMF_CODE ? " dtmf=" + std::to_string(req.dtmf_code) : ""));
}

// F：通话状态补充（BT_HFP_CALL_STATUS_COVER：state + phoneNum + name）
void Session::handleHfpCallStatusCover(const Frame& f) {
  pb::BTHfpCallStatusCover cover;
  if (!pb::BTHfpCallStatusCover::decode(f.payload.data(), f.payload.size(), &cover)) {
    log("tel", "BT_HFP_CALL_STATUS_COVER 解析失败");
    return;
  }
  hfpCount_.fetch_add(1);
  if (host_) host_->onTelephony(f.serviceType, cover.state, cover.phone_num, cover.name);
  log("tel", "BT_HFP_CALL_STATUS_COVER state=" + std::to_string(cover.state) + " num=" +
                  cover.phone_num + " name=" + cover.name);
}

// F：手机查询 HFP 状态（目前只有 MIC_STATUS 一种 type）
void Session::handleHfpStatusRequest(const Frame& f) {
  pb::BTHfpStatusRequest req;
  if (!pb::BTHfpStatusRequest::decode(f.payload.data(), f.payload.size(), &req)) {
    log("tel", "BT_HFP_STATUS_REQUEST 解析失败");
    return;
  }
  hfpCount_.fetch_add(1);
  if (host_) host_->onHfpStatusRequest(req);
  // 参考实现（BtHfpManager.java:258-270）：查电话镜像的麦克风静音状态，
  // 用 HU_MIC_MUTE(1)/HU_MIC_UNMUTE(0) 应答。我们没有电话镜像，
  // 按“未静音”应答（与“车机麦克风未静音”一致，不是假值）。
  pb::BTHfpStatusResponse resp;
  resp.type = req.type;
  resp.status = (req.type == hfp::TYPE_MIC_STATUS) ? hfp::MIC_UNMUTE : hfp::STATUS_INVALID_PARAM;
  sendOn(ch::CMD, msg::BT_HFP_STATUS_RESPONSE, resp.encode());
  log("tel", "BT_HFP_STATUS_REQUEST type=" + std::to_string(req.type) +
                  " -> 应答 status=" + std::to_string(resp.status));
}

// O：车控（CarlifeVehicleControl）—— 手机向车机下发的控车指令
void Session::handleVehicleControl(const Frame& f) {
  pb::VehicleControl vc;
  if (!pb::VehicleControl::decode(f.payload.data(), f.payload.size(), &vc)) {
    log("car", "VEHICLE_CONTROL 解析失败（" + std::to_string(f.payload.size()) + " B）");
    return;
  }
  vehicleControlCount_.fetch_add(1);
  if (host_) host_->onVehicleControl(vc);
  log("car", "VEHICLE_CONTROL type=" + std::to_string(vc.type) + " id=" + std::to_string(vc.id) +
                  " area=" + std::to_string(vc.area_id) + " valueType=" +
                  std::to_string(vc.value_type) + " int32=" + std::to_string(vc.int32_values.size()) +
                  " float=" + std::to_string(vc.float_values.size()) + " str=\"" +
                  vc.string_value + "\"");
}

// 语音控制（CarlifeVoiceControlRequest）
void Session::handleVoiceControl(const Frame& f) {
  pb::VoiceControlRequest vc;
  if (!pb::VoiceControlRequest::decode(f.payload.data(), f.payload.size(), &vc)) {
    log("vr", "HU_VOICE_CONTROL 解析失败");
    return;
  }
  voiceControlCount_.fetch_add(1);
  if (host_) host_->onVoiceControl(vc);
  log("vr", "HU_VOICE_CONTROL command=" + std::to_string(vc.command) + " opt=" +
                  std::to_string(vc.opt));
}

// PREPARE_DONE acknowledges the end of the optional cue, not microphone health.
// VRModule.kt sends it from BOTH onFinish and onError. A missing/failed cue must
// not leave the phone waiting forever. No recording state is changed here.
void Session::handleMicRecordPrepare() {
  bool cue_ok = false;
  try { if (host_) cue_ok = host_->prepareMicrophone(); }
  catch (...) { /* cue failure follows the reference onError completion path */ }
  if (!cue_ok) log("vr", "preparation cue unavailable/failed; completing cue phase");
  micPrepareDoneAttempts_.fetch_add(1);
  if (!sendCmd(msg::MIC_RECORD_PREPARE_DONE)) log("vr", "PREPARE_DONE send failed");
}

// R：激活（BOX_ACTIVE），应答 HU_ACTIVE
void Session::handleActivationRequest(const Frame& f) {
  pb::ActiveRequest req;
  if (!pb::ActiveRequest::decode(f.payload.data(), f.payload.size(), &req)) {
    log("active", "BOX_ACTIVE 解析失败（" + std::to_string(f.payload.size()) + " B）");
    return;
  }
  activationCount_.fetch_add(1);
  if (host_) host_->onActivationRequest(req);
  pb::ActiveResponse resp;
  // proto 里是 required string statue（百度自己的拼写）。取值域取 "1"=激活成功 / "0"=失败。
  const bool provisioned = cfg_.activationState == 3 && !cfg_.activationToken.empty();
  resp.statue = provisioned ? "1" : "0";
  if (provisioned) resp.token = cfg_.activationToken;
  sendOn(ch::CMD, msg::HU_ACTIVE, resp.encode());
  log("active", "BOX_ACTIVE mac=\"" + req.mac + "\" isActive=" + std::to_string(req.is_active) +
                    " randomValue=" + std::to_string(req.random_value) + " -> 应答 statue=" +
                    resp.statue + (resp.token.empty() ? "" : " token=(已下发)"));
}

// R：内容加密的 3 条协商消息（照抄 FeaturesHandler.kt + EncryptSetupManager.java）
void Session::handleEncryption(uint32_t service_type, const Frame& f) {
  if (service_type == msg::MD_RSA_PUBLIC_KEY_REQUEST) {
    if (!cfg_.contentEncryption || !cipher_.has_keypair()) {
      log("warn", "内容加密：手机来要 RSA 公钥，但本机未启用或没有密钥对 → 不应答（不冒充已启用）");
      if (host_) host_->onEncryption(static_cast<uint8_t>(EncryptionState::Off), "request ignored",
                           encryptedMessages_.load(), decryptFailures_.load());
      return;
    }
    pb::HuRsaPublicKeyResponse resp;
    resp.rsa_public_key = cipher_.public_key_base64();
    sendOn(ch::CMD, msg::HU_RSA_PUBLIC_KEY_RESPONSE, resp.encode());
    cipher_.set_state(EncryptionState::Advertised);
    log("crypto", "内容加密：已回 HU_RSA_PUBLIC_KEY_RESPONSE（2048 位 X.509/SPKI，Base64 " +
                      std::to_string(resp.rsa_public_key.size()) + " 字符）");
    if (host_) host_->onEncryption(static_cast<uint8_t>(cipher_.state()), "public key sent",
                           encryptedMessages_.load(), decryptFailures_.load());
    return;
  }
  if (service_type == msg::MD_AES_KEY_SEND_REQUEST) {
    pb::MdAesKeyRequest req;
    if (!pb::MdAesKeyRequest::decode(f.payload.data(), f.payload.size(), &req)) {
      log("warn", "内容加密：MD_AES_KEY_SEND_REQUEST 解析失败");
      return;
    }
    if (!cipher_.install_aes_key_base64(req.aes_key)) {
      // 绝不猜一个密钥继续 —— 那会把“没实现”伪装成“已实现”。
      log("warn", "内容加密：AES 密钥 RSA 解密失败 → 不应答，保持明文");
      if (host_) {
        host_->onEncryption(static_cast<uint8_t>(EncryptionState::Off), "aes key decrypt failed",
                               encryptedMessages_.load(), decryptFailures_.load());
      }
      return;
    }
    // 照抄 FeaturesHandler.kt:handleAESKeyRequest：回一条【空载荷】的 HU_AES_REC_RESPONSE。
    sendOn(ch::CMD, msg::HU_AES_REC_RESPONSE);
    log("crypto", "内容加密：已用私钥解出 AES-" + std::to_string(cipher_.aes_key_bits()) +
                      " 密钥，并发 HU_AES_REC_RESPONSE");
    if (host_) host_->onEncryption(static_cast<uint8_t>(cipher_.state()), "aes key installed",
                           encryptedMessages_.load(), decryptFailures_.load());
    return;
  }
  // MD_ENCRYPT_READY
  // 照抄 FeaturesHandler.kt:handleEncryptReady：先回 MD_ENCRYPT_READY_DONE，再置位。
  sendOn(ch::CMD, msg::MD_ENCRYPT_READY_DONE);
  if (!cipher_.has_aes_key()) {
    log("warn", "内容加密：收到 MD_ENCRYPT_READY 但没有 AES 密钥 → 保持明文（已回 DONE）");
    if (host_) host_->onEncryption(static_cast<uint8_t>(EncryptionState::Off), "ready without key",
                           encryptedMessages_.load(), decryptFailures_.load());
    return;
  }
  cipher_.set_state(EncryptionState::Ready);
  cipher_.set_enabled(true);
  log("crypto", "内容加密：已启用（之后所有载荷走 AES-ECB/PKCS5；帧头仍为明文，payloadSize 随之修正）");
  if (host_) host_->onEncryption(static_cast<uint8_t>(cipher_.state()), "encryption enabled",
                           encryptedMessages_.load(), decryptFailures_.load());
}

// D：车辆数据订阅（两条路径）+ 上报开关
void Session::handleCarDataSubscribe(uint32_t service_type, const Frame& f) {
  carDataSubscribeCount_.fetch_add(1);
  if (host_) host_->onCarDataSubscribe(service_type);
  (void)f;
  if (service_type == msg::CAR_DATA_SUBSCRIBE_REQ) {
    carDataSubscribed_ = true;
    publishVehicleReport();
    log("car", "CAR_DATA_SUBSCRIBE：已开启车况上报（周期 " + std::to_string(cfg_.vehicleReportMs) +
                    "ms）");
    return;
  }
  if (service_type == msg::CARLIFE_DATA_REQ) {
    // 官方 SDK 的 FeaturesHandler：回一条 CARLIFE_DATA_SUBSCRIBE，携带订阅表。
    pb::FeatureConfigList empty;
    sendOn(ch::CMD, msg::CARLIFE_DATA_SUBSCRIBE, empty.encode());
    log("car", "CARLIFE_DATA_REQ -> 已回 CARLIFE_DATA_SUBSCRIBE");
    return;
  }
  if (service_type == msg::CAR_DATA_SUBSCRIBE_START) {
    carDataSubscribed_ = true;
    publishVehicleReport();
    log("car", "CAR_DATA_SUBSCRIBE_START");
    return;
  }
  if (service_type == msg::CAR_DATA_SUBSCRIBE_STOP) {
    carDataSubscribed_ = false;
    log("car", "CAR_DATA_SUBSCRIBE_STOP：已停止车况上报");
    return;
  }
  if (service_type == msg::CARLIFE_DATA_SUBSCRIBE_DONE) {
    sendOn(ch::CMD, msg::CARLIFE_DATA_SUBSCRIBE_DONE_RSP);
    log("car", "CARLIFE_DATA_SUBSCRIBE_DONE -> 已回应");
    return;
  }
  log("car", "CAR_DATA_SUBSCRIBE_DONE");
}

// D：车况上报（HU->MD）。数据由宿主机提供；没有数据的项不发。
void Session::pollVehicleReport() {
  if (!carDataSubscribed_ || cfg_.vehicleReportMs <= 0) return;
  const uint64_t now = nowMs();
  if (lastVehicleReportMs_ &&
      now - lastVehicleReportMs_ < static_cast<uint64_t>(cfg_.vehicleReportMs)) {
    return;
  }
  publishVehicleReport();
}

void Session::publishVehicleReport() {
  if (!host_) return;
  VehicleReport r;
  if (!host_->takeVehicleReport(r)) return;
  lastVehicleReportMs_ = nowMs();
  const uint64_t ts = static_cast<uint64_t>(std::time(nullptr));
  int sent = 0;
  if (r.has_speed) {
    pb::CarSpeed s;
    s.speed = r.speed_kph;
    s.timestamp = ts;
    if (sendOn(ch::CMD, msg::CAR_VELOCITY, s.encode())) ++sent;
  }
  if (r.has_gear) {
    pb::GearInfo g;
    g.gear = r.gear;
    if (sendOn(ch::CMD, msg::CAR_GEAR, g.encode())) ++sent;
  }
  if (r.has_oil) {
    pb::Oil o;
    o.level = r.oil_level;
    o.range = r.oil_range;
    o.low_fuel_warning = r.oil_low_warning;
    if (sendOn(ch::CMD, msg::CAR_OIL, o.encode())) ++sent;
  }
  if (r.has_gps) {
    pb::CarGps g;
    g.antenna_state = r.antenna_state;
    g.signal_quality = r.signal_quality;
    g.latitude = r.latitude_e6;
    g.longitude = r.longitude_e6;
    g.height = r.altitude_m;
    g.speed = r.gps_speed;
    g.heading = r.gps_heading;
    g.fix = r.gps_fix;
    g.sats_used = r.sats_used;
    g.sats_visible = r.sats_visible;
    g.north_speed = r.north_speed;
    g.east_speed = r.east_speed;
    g.vert_speed = r.vert_speed;
    g.timestamp = ts;
    g.has_latitude = true;
    if (sendOn(ch::CMD, msg::CAR_GPS, g.encode())) ++sent;
  }
  if (r.has_gyroscope) {
    pb::Gyroscope g;
    g.x = r.gyro_x;
    g.y = r.gyro_y;
    g.z = r.gyro_z;
    g.timestamp = ts;
    if (sendOn(ch::CMD, msg::CAR_GYROSCOPE, g.encode())) ++sent;
  }
  if (r.has_acceleration) {
    pb::Acceleration a;
    a.x = r.acc_x;
    a.y = r.acc_y;
    a.z = r.acc_z;
    a.timestamp = ts;
    if (sendOn(ch::CMD, msg::CAR_ACCELERATION, a.encode())) ++sent;
  }
  if (sent) {
    vehicleReportSent_.fetch_add(1);
    log("car", "已上报车况 " + std::to_string(sent) + " 项" +
                    (r.has_speed ? " speed=" + std::to_string(r.speed_kph) + "km/h" : "") +
                    (r.has_gear ? " gear=" + std::to_string(r.gear) : "") +
                    (r.has_gps ? " gps=(" + std::to_string(r.latitude_e6) + "," +
                                     std::to_string(r.longitude_e6) + ")"
                               : ""));
  }
}

// T：帧率动态调整（VIDEO_ENCODER_FRAME_RATE_CHANGE）
bool Session::requestFrameRate(int32_t fps) {
  if (fps <= 0) return false;
  if (static_cast<int>(state_) < static_cast<int>(State::Established)) return false;
  pb::VideoFrameRate fr;
  fr.frameRate = fps;
  const bool ok = sendOn(ch::CMD, msg::VIDEO_ENCODER_FRAME_RATE_CHANGE, fr.encode());
  log("video", std::string("请求手机改帧率 -> ") + std::to_string(fps) + (ok ? " sent" : " FAILED"));
  return ok;
}

void Session::heartbeatLoop() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    if (!running_.load()) break;
    if (static_cast<int>(state_) >= static_cast<int>(State::Established) && tx_.channelOpen(ch::VIDEO)) {
      if (tx_.send(ch::VIDEO, msg::VIDEO_HEARTBEAT)) heartbeatsSent_.fetch_add(1);
    }
    if (state_ == State::VideoStarted) {
      const uint64_t silent = nowMs() - lastVideoByteMs_.load();
      if (silent > 10000) {
        log("warn", "no video data for 10s -> protocol heartbeat timeout, closing session");
        running_.store(false);
        tx_.close();
        return;
      }
    }
  }
}

void Session::snapshotLoop() {
  if (cfg_.snapshotDir.empty()) return;
  const int intervalMs = (cfg_.snapshotEverySec > 0 ? cfg_.snapshotEverySec : 2) * 1000;
  int seq = 0;
  uint64_t lastSnapFrame = 0;
  uint64_t nextSnapAt = 0;  // 0 => 出第一帧就立即存一张
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (!running_.load() || !host_) break;
    const uint64_t decoded = host_->decodedFrames();
    if (decoded == 0 || decoded == lastSnapFrame) continue;
    const uint64_t now = nowMs();
    if (now < nextSnapAt) continue;
    if (!ensureDir(cfg_.snapshotDir)) continue;
    char name[64];
    std::snprintf(name, sizeof(name), "/snapshot-%03d.bmp", ++seq);
    const std::string path = cfg_.snapshotDir + name;
    if (host_->saveSnapshot(path)) {
      lastSnapFrame = decoded;
      nextSnapAt = now + static_cast<uint64_t>(intervalMs);
      log("snap", "wrote " + path + " (frame " + std::to_string(decoded) + ")");
    }
  }
}

uint64_t Session::framesDecoded() const { return host_ ? host_->decodedFrames() : 0; }
uint64_t Session::audioBytes() const { return host_ ? host_->decodedAudioBytes() : 0; }

}  // namespace carlife
