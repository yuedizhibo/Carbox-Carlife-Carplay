// 帧头编解码 + 极简 protobuf wire 编解码（本机 WSL/板子上没有 libprotobuf，也不做代码生成）
//
// 帧头布局完全对齐参考实现 CarLifeMessage.kt（BitConverter.kt 全为大端）：
//   CMD / TOUCH 通道 (commandSize=8) : [0..1] payloadSize(u16) [2..3] 保留 [4..7] serviceType(i32)
//   其余通道     (commandSize=12): [0..3] payloadSize(i32) [4..7] timestamp(i32) [8..11] serviceType(i32)
// AOA(USB 单通道)另有 8 字节前缀：[0..3] channel(i32) [4..7] size(i32)，见 header(fillByteArray)。
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace carlife {

void be16_put(uint8_t* p, uint16_t v);
void be32_put(uint8_t* p, uint32_t v);
uint16_t be16_get(const uint8_t* p);
uint32_t be32_get(const uint8_t* p);

int headerSize(int32_t channel);              // 8 或 12
bool isShortHeaderChannel(int32_t channel);   // CMD / TOUCH

struct Frame {
  int32_t channel = 1;
  uint32_t serviceType = 0;
  uint32_t timestamp = 0;
  std::vector<uint8_t> payload;

  size_t totalSize() const { return static_cast<size_t>(headerSize(channel)) + payload.size(); }
  std::vector<uint8_t> encode() const;        // 直接写 socket 的字节
};

// 从已读到的 commandSize 字节解析头，得到 payload 长度
struct HeaderInfo {
  uint32_t payloadSize = 0;
  uint32_t timestamp = 0;
  uint32_t serviceType = 0;
};
bool parseHeader(const uint8_t* p, int32_t channel, HeaderInfo* out);

// ---------- protobuf ----------
class PbWriter {
 public:
  void fieldVarint(int field, uint64_t v);
  void fieldInt32(int field, int32_t v);
  void fieldBool(int field, bool v);
  void fieldString(int field, const std::string& s);
  void fieldBytes(int field, const uint8_t* p, size_t n);
  void fieldMessage(int field, const PbWriter& sub);
  // 本轮新增：新 proto 里用到的其余标量类型。
  // uint32/uint64 与 int32 的区别只在负数：int32 负数要符号扩展到 10 字节，uint 不需要。
  void fieldUint32(int field, uint32_t v) { fieldVarint(field, v); }
  void fieldUint64(int field, uint64_t v) { fieldVarint(field, v); }
  // sint32/sint64 用 zigzag 编码（CarlifeVehicleControl 的 int32_values 就是 sint32）。
  void fieldSint32(int field, int32_t v);
  void fieldSint64(int field, int64_t v);
  // float = wiretype 5（小端 4 字节），double = wiretype 1（小端 8 字节）。
  void fieldFloat(int field, float v);
  void fieldDouble(int field, double v);
  const std::vector<uint8_t>& data() const { return buf_; }
  size_t size() const { return buf_.size(); }

 private:
  void rawVarint(uint64_t v);
  std::vector<uint8_t> buf_;
};

class PbReader {
 public:
  PbReader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
  // 读下一个 tag；返回 false 表示结束或格式错误
  bool next(uint32_t* field, uint32_t* wireType);
  bool readVarint(uint64_t* v);
  bool readInt32(int32_t* v);
  bool readBool(bool* v);
  bool readLengthDelimited(const uint8_t** p, size_t* n);
  bool readString(std::string* s);
  bool skip(uint32_t wireType);
  // 本轮新增标量读法（与 PbWriter 的写侧一一对应）。
  bool readUint32(uint32_t* v);
  bool readUint64(uint64_t* v);
  bool readSint32(int32_t* v);
  bool readSint64(int64_t* v);
  bool readFloat(float* v);
  bool readDouble(double* v);
  size_t remaining() const { return static_cast<size_t>(end_ - p_); }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
};

// ---------- 本项目用到的消息（手写，字段号与 proto 一致） ----------
namespace pb {

struct ProtocolVersion {
  int32_t majorVersion = 0;
  int32_t minorVersion = 1;
  std::vector<uint8_t> encode() const;
};

struct ProtocolVersionMatchStatus {
  int32_t matchStatus = 1;
  int32_t carlifeProtocolVersion = 2;
  static bool decode(const uint8_t* p, size_t n, ProtocolVersionMatchStatus* out);
};

struct AuthenRequest {
  std::string randomValue;
  std::vector<uint8_t> encode() const;
};
struct AuthenResponse {
  std::string encryptValue;
  std::vector<uint8_t> encode() const;
  static bool decode(const uint8_t* p, size_t n, AuthenResponse* out);
};
struct AuthenResult {
  bool authenResult = false;
  std::vector<uint8_t> encode() const;
  static bool decode(const uint8_t* p, size_t n, AuthenResult* out);
};

struct VideoEncoderInfo {
  int32_t width = 1920;
  int32_t height = 1080;
  int32_t frameRate = 30;
  std::vector<uint8_t> encode() const;
  static bool decode(const uint8_t* p, size_t n, VideoEncoderInfo* out);
};
struct VideoFrameRate {
  int32_t frameRate = 30;
  std::vector<uint8_t> encode() const;
  static bool decode(const uint8_t* p, size_t n, VideoFrameRate* out);
};

struct DeviceInfo {  // CarlifeDeviceInfo（HU_INFO / MD_INFO 共用）
  std::string os, board, bootloader, brand, cpuAbi, cpuAbi2, device, display;
  std::string fingerprint, hardware, host, cid, manufacturer, model, product, serial;
  std::string codename, incremental, release, sdk, token, btAddress, carlifeVersion;
  int32_t sdkInt = 0;
  std::vector<uint8_t> encode() const;
  static bool decode(const uint8_t* p, size_t n, DeviceInfo* out);
};

struct StatisticsInfo {  // CarlifeStatisticsInfo
  std::string cuid, versionName, channel;
  int32_t versionCode = 1;
  int32_t connectCount = 0;
  int32_t connectSuccessCount = 0;
  int32_t connectTime = 0;
  std::vector<uint8_t> encode() const;
};

struct FeatureConfig {  // CarlifeFeatureConfig
  std::string key;
  int32_t value = 0;
};
struct FeatureConfigList {  // CarlifeFeatureConfigList
  std::vector<FeatureConfig> configs;
  bool huBtAudioSupport = false;
  std::string huBtName, huBtMac;
  std::vector<uint8_t> encode() const;
  static bool decode(const uint8_t* p, size_t n, FeatureConfigList* out);
};

struct TouchSinglePoint {  // CarlifeTouchSinglePoint
  int32_t x = 0, y = 0, pointerX = 0, pointerY = 0;
  std::vector<uint8_t> encode() const;
};
struct CarHardKeyCode {  // CarlifeCarHardKeyCode
  int32_t keycode = 0;
  std::vector<uint8_t> encode() const;
};
// CarlifeMediaProgressBar { required int32 progressBar = 1; }
// 依据：Reference/apollo-DuerOS/.../carlife-sdk/src/main/proto/CarlifeMediaProgressBarProto.proto
// （消息 ID = MSG_CMD_MEDIA_PROGRESS_BAR 0x00010036，MD->HU）。
struct MediaProgressBar {
  int32_t progressBar = 0;   // 0..100
  static bool decode(const uint8_t* p, size_t n, MediaProgressBar* out);
};
struct ModuleStatusList {  // CarlifeModuleStatusList
  std::vector<std::pair<int32_t, int32_t>> items;  // moduleID, statusID
  static bool decode(const uint8_t* p, size_t n, ModuleStatusList* out);
};
struct AudioInit {  // CarlifeMusicInit / CarlifeTTSInit {sampleRate, channelConfig, sampleFormat}
  int32_t sampleRate = 44100;
  int32_t channelConfig = 12;  // AudioFormat.CHANNEL_OUT_STEREO
  int32_t sampleFormat = 2;    // AudioFormat.ENCODING_PCM_16BIT
  static bool decode(const uint8_t* p, size_t n, AudioInit* out);
};

// ────────────────────────────────────────────────────────────────
// A..U：本轮「CarLife 全量接口」新增的消息。
// 字段号逐条取自 Reference/apollo-DuerOS/.../carlife-sdk/src/main/proto/*.proto。
// 约定：写了 decode 的就是我们要【解析】的（MD->HU），
//       写了 encode 的就是我们要【构造】的（HU->MD）。
// ────────────────────────────────────────────────────────────────

// CarlifeMediaInfoProto：source/song/artist/album/albumArt/duration/playlistNum/songId/mode
struct MediaInfo {
  std::string source, song, artist, album;
  std::string album_art;  // field 5，bytes：封面原图（JPEG/PNG）
  int32_t duration = 0;
  int32_t playlist_num = 0;
  std::string song_id;
  int32_t mode = 0;       // 播放模式（repeat/shuffle）
  bool has_song = false;
  static bool decode(const uint8_t* p, size_t n, MediaInfo* out);
};

// CarlifeNaviNextTurnInfoProto：action/nextTurn/roadName/totalDistance/remainDistance/field6
// 【field 6 有两个互相矛盾的官方版本，两个都要兼容】：
//   * Kotlin SDK V2.0（我们手机端的对应版本）：`required bytes turnIconData = 6;`
//   * 官方 C++ 车机库：                      `required int32 time = 6;`
// 因此解析时按 wire type 分流：2=bytes → turn_icon；0=varint → time_s。
struct NaviNextTurnInfo {
  int32_t action = 0;
  int32_t next_turn = 0;
  std::string road_name;
  int32_t total_distance = 0;
  int32_t remain_distance = 0;
  std::string turn_icon;  // field 6，bytes（Kotlin SDK 版）：转向图标
  int32_t time_s = 0;     // field 6，varint（C++ 库版）：预计剩余时间（秒）
  bool has_action = false;
  static bool decode(const uint8_t* p, size_t n, NaviNextTurnInfo* out);
};

// CarlifeNaviAssitantGuideInfoProto：action/assistantType/trafficSignType/total/remain/cameraSpeed
struct NaviAssistantGuideInfo {
  int32_t action = 0, assistant_type = 0, traffic_sign_type = 0;
  int32_t total_distance = 0, remain_distance = 0, camera_speed = 0;
  static bool decode(const uint8_t* p, size_t n, NaviAssistantGuideInfo* out);
};

// CarlifeCarSpeedProto { int32 speed = 1; optional uint64 timeStamp = 2; }
struct CarSpeed { int32_t speed = 0; uint64_t timestamp = 0; std::vector<uint8_t> encode() const; };
// CarlifeGearInfoProto { int32 gear = 1; }
// 两个方向都要：CAR_GEAR(0x00018029) 是 HU->MD（encode），
// GEAR_INFO(0x00010029) 是 MD->HU（decode）。
struct GearInfo {
  int32_t gear = 0;
  std::vector<uint8_t> encode() const;
  static bool decode(const uint8_t* p, size_t n, GearInfo* out);
};
// CarlifeOilProto { int32 level = 1; int32 range = 2; optional bool lowFuleWarning = 3; }
struct Oil { int32_t level = 0, range = 0; bool low_fuel_warning = false; bool has_level = false; std::vector<uint8_t> encode() const; };
// CarlifeGyroscopeProto { int32 gyroType; double gyroX/gyroy/gyroZ; optional uint64 timeStamp; }
struct Gyroscope { int32_t gyro_type = 0; double x = 0, y = 0, z = 0; uint64_t timestamp = 0; std::vector<uint8_t> encode() const; };
// CarlifeAccelerationProto { double accX/accY/accZ; optional uint64 timeStamp; }
struct Acceleration { double x = 0, y = 0, z = 0; uint64_t timestamp = 0; std::vector<uint8_t> encode() const; };
// CarlifeCarGpsProto：只取我们要用的字段（其余按 wire 跳过，不丢帧）
struct CarGps {
  uint32_t antenna_state = 0, signal_quality = 0;
  int32_t latitude = 0, longitude = 0, height = 0;
  uint32_t speed = 0, heading = 0, fix = 0, sats_used = 0, sats_visible = 0;
  int32_t north_speed = 0, east_speed = 0, vert_speed = 0;
  uint64_t timestamp = 0;
  bool has_latitude = false;
  std::vector<uint8_t> encode() const;
};

// CarlifeContactsProto/CarlifeContactsListProto
struct Contact {
  int32_t cid = 0;
  std::string name, number;
  std::vector<uint8_t> encode() const;
};
struct ContactsList {
  int32_t cnt = 0;
  std::vector<Contact> contacts;
  static bool decode(const uint8_t* p, size_t n, ContactsList* out);
};

// CarlifeCallRecordsProto/CarlifeCallRecordsListProto
struct CallRecord {
  int32_t cid = 0;
  std::string name, number, duration, time;
  int32_t type = 0;  // call_type::*
};
struct CallRecordsList {
  int32_t cnt = 0;
  std::vector<CallRecord> records;
  static bool decode(const uint8_t* p, size_t n, CallRecordsList* out);
};

// CarlifeBTHfpRequestProto / ResponseProto / IndicationProto / ConnectionProto /
// StatusRequestProto / StatusResponseProto / CallStatusCoverProto
struct BTHfpRequest {
  int32_t command = 0;
  std::string phone_num;
  int32_t dtmf_code = 0;
  static bool decode(const uint8_t* p, size_t n, BTHfpRequest* out);
};
struct BTHfpResponse {
  int32_t status = 0, cmd = 0, dtmf_code = 0;
  std::vector<uint8_t> encode() const;
};
struct BTHfpIndication {
  int32_t state = 0;
  std::string phone_num, phone_name, address;
  std::vector<uint8_t> encode() const;
};
struct BTHfpConnection {
  int32_t state = 0;
  std::string address, name;
  std::vector<uint8_t> encode() const;
};
struct BTHfpStatusRequest {
  int32_t type = 0;
  static bool decode(const uint8_t* p, size_t n, BTHfpStatusRequest* out);
};
struct BTHfpStatusResponse {
  int32_t status = 0, type = 0;
  std::vector<uint8_t> encode() const;
};
struct BTHfpCallStatusCover {
  int32_t state = 0;
  std::string phone_num, name;
  static bool decode(const uint8_t* p, size_t n, BTHfpCallStatusCover* out);
};

// CarlifeActiveRequestProto / CarlifeActiveResponseProto（激活）
struct ActiveRequest {
  std::string mac;
  int32_t is_active = 0, random_value = 0;
  static bool decode(const uint8_t* p, size_t n, ActiveRequest* out);
};
struct ActiveResponse {
  std::string statue, token;
  std::vector<uint8_t> encode() const;
};

// CarlifeHuRsaPublicKeyResponseProto { string rsaPublicKey = 1; }
struct HuRsaPublicKeyResponse {
  std::string rsa_public_key;
  std::vector<uint8_t> encode() const;
};
// CarlifeMdAesKeyRequestProto { string aesKey = 1; }
struct MdAesKeyRequest {
  std::string aes_key;
  static bool decode(const uint8_t* p, size_t n, MdAesKeyRequest* out);
};

// CarlifeFileTransferBeginProto { int64 fileSize = 1; optional int32 version = 2; }
struct FileTransferBegin {
  int64_t file_size = 0;
  int32_t version = 0;
  static bool decode(const uint8_t* p, size_t n, FileTransferBegin* out);
};

// CarlifeVehicleControlProto（915 B，字段最多）
struct VehicleControl {
  int32_t type = 0, id = 0;
  bool support = false;
  std::string token_string;
  int32_t area_id = 0;
  std::vector<int32_t> area_value;
  int32_t value_type = 0;
  std::string bytes_value;
  std::vector<int32_t> int32_values;   // field 9，sint32
  std::vector<int64_t> int64_values;   // field 10，sint64
  std::vector<float> float_values;     // field 11，float
  std::string string_value;
  static bool decode(const uint8_t* p, size_t n, VehicleControl* out);
};

// CarlifeTouchEventAllDeviceProto / DeviceProto / EventProto（多点触控）
struct TouchEvent {
  int32_t type = 0, code = 0, value = 0;
  static bool decode(const uint8_t* p, size_t n, TouchEvent* out);
};
struct TouchEventDevice {
  int32_t cid = 0, eventx = 0;
  int32_t screen_width = 0, screen_height = 0;
  int32_t abs_x_min = 0, abs_x_max = 0, abs_y_min = 0, abs_y_max = 0;
  std::string device;
  std::vector<TouchEvent> down_events, up_events, move_events;
  static bool decode(const uint8_t* p, size_t n, TouchEventDevice* out);
};
struct TouchEventAllDevice {
  int32_t version = 0, cnt = 0;
  std::vector<TouchEventDevice> devices;
  static bool decode(const uint8_t* p, size_t n, TouchEventAllDevice* out);
};

// CarLifeTouchPadActionProto（package com.yftech）：Down/Move/Up/Focus/Pinch
// MSG_TOUCH_ACTION_3 的定长布局（非 protobuf）—— 见 service_types.h 的 TOUCH_ACTION_3。
//   action(4B BE)  [pointerId(1B) x(2B BE) y(2B BE)] × pointers
// 依据 receiver/touch/RemoteControlManager.kt:76-104（发送侧）与 util/BitConverter.kt（大端）。
struct TouchAction3 {
  static constexpr std::size_t kPointerBytes = 5;
  struct Pointer {
    int32_t id = 0;
    int32_t x = 0, y = 0;
  };
  int32_t action = 0;  // Android MotionEvent action，见 service_types.h 的 motion::*
  std::vector<Pointer> pointers;
  // 编码：供车机向手机下发多点触控。
  std::vector<uint8_t> encode() const;
  // 解码：手机向车机做“反控”时走到同一条。
  static bool decode(const uint8_t* p, size_t n, TouchAction3* out);
};

struct TouchPadMove {
  int32_t timestamp = 0, delta_x = 0, delta_y = 0;
  static bool decode(const uint8_t* p, size_t n, TouchPadMove* out);
};
struct TouchPadPinch {
  float scale = 1.0f;
  static bool decode(const uint8_t* p, size_t n, TouchPadPinch* out);
};
struct TouchPadSimple {  // Down / Up / Focus 都只有一个 timestamp
  int32_t timestamp = 0;
  static bool decode(const uint8_t* p, size_t n, TouchPadSimple* out);
};

// CarlifeTouchScrollProto / CarlifeTouchFlingProto（手势）
struct TouchScroll {
  int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  float distance_x = 0, distance_y = 0;
  static bool decode(const uint8_t* p, size_t n, TouchScroll* out);
};
struct TouchFling {
  int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  float velocity_x = 0, velocity_y = 0;
  static bool decode(const uint8_t* p, size_t n, TouchFling* out);
};

// CarlifeVoiceControlRequestProto { int32 command = 1; optional int32 opt = 2; }
struct VoiceControlRequest {
  int32_t command = 0, opt = 0;
  static bool decode(const uint8_t* p, size_t n, VoiceControlRequest* out);
};

// CarlifeBTStartPairReqProto / CarlifeBTIdentifyResultIndProto（蓝牙配对/识别）
struct BTStartPairReq {
  int32_t ostype = 0;
  std::string address;
  std::vector<uint8_t> encode() const;
};
struct BTIdentifyResultInd {
  int32_t status = 0;
  std::string address;
  static bool decode(const uint8_t* p, size_t n, BTIdentifyResultInd* out);
};
// CarlifeBTPairInfoProto（OOB 配对信息）
struct BTPairInfo {
  std::string address, pass_key, hash, randomizer, uuid, name;
  int32_t status = 0;
  std::vector<uint8_t> encode() const;
  static bool decode(const uint8_t* p, size_t n, BTPairInfo* out);
};
// CarlifeConnectTimeSyncProto { int32 timeStamp = 1; }
struct ConnectTimeSync {
  int32_t timestamp = 0;
  static bool decode(const uint8_t* p, size_t n, ConnectTimeSync* out);
};
// CarlifeBTAudioInfoProto { int32 supportBtAudio = 1; }
struct BTAudioInfo {
  int32_t support_bt_audio = 0;
  std::vector<uint8_t> encode() const;
};

}  // namespace pb
}  // namespace carlife