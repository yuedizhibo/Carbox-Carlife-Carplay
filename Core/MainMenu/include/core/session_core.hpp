#pragma once
// 会话核心模型 —— CarPlay / CarLife 双输入共用的**唯一**状态契约。
//
// 设计约束（务必遵守，改动会影响 5 个模块）：
//   1) **定长、无堆分配**：所有字段都是 std::array / 标量，可拷可比较，不抛异常。
//   2) **纯增量**：只在末尾加字段/加枚举值，不删不改既有成员的语义，保证老调用点仍可编译。
//   3) **有界**：任何来自车的字符串都必须被 copy_text 截断，绝不能越界。
//   4) 这里只放"状态"，不放"行为"；行为在 Input 适配器与 Convert 存储里。
//
// 覆盖范围（对照 Temp/CarPlay-CarLife-CAPABILITY-AUDIT.md 的 ❌ 清单）：
//   元数据/封面/歌词/进度、safe area、副屏平面、显示校准、昼夜、多声道与音频仲裁、
//   多点/旋钮/触摸板手势/接近/HID/VoiceOver/AssistiveTouch、硬键与媒体键、
//   导航逐向与 GPS/车况、电话与通讯录、文件传输/OTA、激活与内容加密、
//   多设备会话切换、帧率、DTMF、车控。
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>

namespace mvp {
constexpr std::size_t kMaxSources=8,kMaxVideoBytes=256*1024,kMaxAudioBytes=2048;
constexpr std::size_t kMaxControls=32,kMaxText=96;
// 新增容量上限：一律取固定值，避免任何动态分配。
constexpr std::size_t kMaxTextLong=192;     // 标题/歌手/专辑/来电者等
constexpr std::size_t kMaxLyrics=4096;      // 歌词正文（LRC 原文），整块定长
constexpr std::size_t kMaxArtwork=128*1024; // 专辑封面（JPEG/PNG 原字节）
constexpr std::size_t kMaxMultiTouch=10;    // 同时触点数（iAP2/CarLife 的实际上限）
constexpr std::size_t kMaxContacts=64;      // 通讯录条目（可截断，完备性由 count 报告）
constexpr std::size_t kMaxSessionList=8;    // 多设备会话表

enum class InputSourceKind { External, LocalDesktop, Synthetic };
enum class SelectionMode { Automatic, Manual };
enum class VideoEncoding { Svg, H264AnnexB };

// 音频编码：CarPlay 的 AudioFormat 与 CarLife 的音频类型并集（AUDIT 2.4 / 6.1）
enum class AudioCodec : uint8_t {
  Unknown=0, Pcm=1, Pcm16=2, Pcm24=3, Alac=4, AacLc=5, AacEld=6, Opus=7
};
// 音频用途：决定客户端音量策略（媒体 1.0 / 导航 0.5 / 通话 / 提示音）
enum class AudioRole : uint8_t { Unknown=0, Media=1, Navigation=2, Telephony=3, Alert=4, Voice=5 };
// 导航逐向的机动类型（对齐 iAP2 ManeuverType / CarLife NAV_NEXT_TURN_INFO）
enum class Maneuver : uint8_t {
  None=0, Straight=1, SlightLeft=2, Left=3, SharpLeft=4,
  SlightRight=5, Right=6, SharpRight=7, Uturn=8,
  Merge=9, ForkLeft=10, ForkRight=11, Roundabout=12, Exit=13, Arrive=14, Destination=15
};

// Bounded encoded payload. H264AnnexB is a transport seam only; this MVP does not decode it.
struct VideoFrame { VideoEncoding encoding{VideoEncoding::Svg}; uint32_t width{},height{}; uint64_t pts{},sequence{}; std::array<char,kMaxVideoBytes> payload{}; std::size_t size{}; };

// 音频块：新增 codec/channels/role —— 多声道与杜比**不走这里解码**，只如实转发，
// 由浏览器侧按 role 选播放器（AUDIT 6.1：不自行解码串改）。
struct AudioChunk {
  uint64_t sequence{}; uint32_t sample_rate{8000};
  AudioCodec codec{AudioCodec::Pcm16}; AudioRole role{AudioRole::Media};
  uint8_t channels{2}; uint8_t bits{16};
  std::array<int16_t,kMaxAudioBytes/sizeof(int16_t)> samples{}; std::size_t sample_count{};
};

// 控制事件：从"只有 Touch/Key"扩展为全部单点/多点/旋钮/手势/语音/电话键。
// 保持既有 Type::Touch / Type::Key 的编号与语义不变（老代码不受影响）。
struct ControlEvent {
  enum class Type : uint8_t {
    Touch=0,      // 单点触控（现有）
    Key=1,        // 硬键/媒体键（现有）
    MultiTouch=2, // 多点触控
    Knob=3,       // 旋钮（带方向与步长）
    Gesture=4,    // 触摸板手势
    Proximity=5,  // 接近传感器
    Voice=6,      // 语音按键（Siri/语音助手）
    Telephony=7,  // 电话键（接听/挂断/DTMF）
    VehicleCtrl=8 // 车控指令（空调/车窗等）
  } type{Type::Key};
  enum class TouchPhase : uint8_t { Up=0, Down=1, Move=2 } phase{TouchPhase::Up};
  enum class KnobDir : uint8_t { Left=0, Right=1, Up=2, Down=3, Press=4 } knob_dir{KnobDir::Right};
  int x{}; int y{};
  std::array<char,24> key{};      // 硬键/媒体键名（现有）
  // 多点触控：最多 kMaxMultiTouch 个触点，points 是有效个数。
  struct Point { int16_t x{},y{}; uint8_t id{},phase{}; };
  std::array<Point,kMaxMultiTouch> points{}; uint8_t point_count{};
  int16_t knob_steps{};           // 旋钮步数（正=右/下，负=左/上）
  std::array<char,32> gesture{};  // 手势名：swipe_left / pinch_in ...
  std::array<char,32> ctrl{};     // 车控名：ac_temp_up / window_open ...
  std::array<char,16> dtmf{};     // DTMF 数字串
};

struct SessionEvent { enum class Type { SourceChanged, FrameDropped, Control } type{}; std::array<char,kMaxText> detail{}; uint64_t sequence{}; };
class InputAdapter { public: virtual ~InputAdapter()=default; virtual std::string_view id()const=0; virtual InputSourceKind kind()const=0; virtual void start()=0; virtual void stop()=0; virtual void on_control(const ControlEvent&)=0; };
struct SourceState { std::array<char,32> id{}; InputSourceKind kind{}; bool connected{}; };

// ── 元数据（Now Playing / 封面 / 歌词 / 进度）── AUDIT 2.6、6.3、6.5
// 封面与歌词体积大，**状态里只放引用与修订号**，正文放 Core/Convert 的存储
// （kMaxArtwork/kMaxLyrics 两块定长缓冲），页面按 revision 变化再取。
struct MediaInfo {
  bool operator==(const MediaInfo&) const = default;
  bool valid{};
  std::array<char,kMaxTextLong> title{},artist{},album{},album_artist{};
  std::array<char,32> app{};           // 来源 App 名
  std::array<char,24> genre{};
  uint32_t track_number{}; uint32_t track_count{};
  uint32_t duration_ms{}; uint32_t position_ms{};
  uint8_t playing{};                   // 0=暂停 1=播放
  uint8_t repeat_mode{}; uint8_t shuffle{};
  uint32_t artwork_revision{}; uint32_t artwork_bytes{}; bool has_artwork{};
  uint32_t lyrics_revision{}; uint32_t lyrics_bytes{}; bool has_lyrics{};
  uint32_t media_library_revision{};   // 媒体库/播放控制（AUDIT 2.6）
};

// ── 显示：safe area / 副屏平面 / 校准 / 昼夜 ── AUDIT 8.1、8.2、8.3
struct DisplayConfig {
  bool operator==(const DisplayConfig&) const = default;
  uint16_t safe_top{},safe_bottom{},safe_left{},safe_right{}; // 像素
  uint16_t width{},height{};                                  // 当前协商分辨率
  uint8_t day_night{};             // 0=日 1=夜
  uint8_t gamma_pct{100},contrast_pct{100},saturation_pct{100}; // 显示校准
  uint8_t aux_enabled{};           // 副屏（仪表/aux）平面是否启用
  uint16_t aux_x{},aux_y{},aux_w{},aux_h{};
  uint8_t primary_plane{};         // 主屏平面号（多屏内容注入）
  uint16_t target_fps{},actual_fps{}; // 帧率动态调整
};

// ── 交互能力状态 ── AUDIT 2.5、6.2
struct InteractionState {
  bool operator==(const InteractionState&) const = default;
  uint8_t multi_touch_points{};    // 车机声明的最大触点数
  uint8_t multi_touch_used{};      // 最近一次实际触点数
  uint8_t touchpad{};
  uint8_t knob{};
  uint8_t proximity{};             // 0=远 1=近
  uint8_t hid_mode{};              // HID SetInputMode 当前模式
  uint8_t voiceover{};             // 无障碍
  uint8_t assistive_touch{};
  uint32_t last_key_code{};        // 最近下发的硬键 keycode
};

// ── 音频：多声道 + 仲裁/闪避 ── AUDIT 2.4、6.1、8.5
struct AudioState {
  bool operator==(const AudioState&) const = default;
  AudioRole active_role{AudioRole::Media};
  uint8_t nav_active{};                 // 导航播报中（压低媒体）
  int32_t media_volume_ppm{1000000};    // 媒体音量（百万分比）
  int32_t nav_volume_ppm{1000000};
  int32_t duck_ratio_ppm{333333};       // 压低比例（默认 1/3，照抄参考实现）
  uint32_t duck_transition_ms{300};
  uint8_t channels{};                   // 当前流声道数
  uint32_t sample_rate{};               // 当前流采样率
  AudioCodec codec{AudioCodec::Unknown};
  uint8_t simultaneous_streams{};       // 同时活跃流数（媒体+导航=2）
};

// ── 车辆数据（GPS/车况）── AUDIT 2.7、7.1
struct VehicleState {
  bool operator==(const VehicleState&) const = default;
  bool valid{};
  uint8_t gear{};                  // 0=P 1=R 2=N 3=D
  int32_t speed_kph{};
  int32_t rpm{};
  double latitude{},longitude{};   // 十进制度
  float heading_deg{};
  uint32_t odometer_km{};
  int32_t fuel_pct{-1};            // -1=无数据
  int32_t range_km{-1};
  int32_t outside_temp_c{};        // 整数摄氏度
  uint8_t night_mode{};
  uint16_t doors{};                // 位图
  uint8_t lights{};
  uint8_t parking_brake{};
  std::array<char,24> vin{};
};

// ── 导航逐向 / 路线指引 ── iAP2 RouteGuidance 0x5200–0x5203、CarLife NAV_NEXT_TURN_INFO
// 注意：CarLife 的 action 码表由百度导航 App 产生，参考树里**没有**对应映射表，
// 所以 maneuver_code 保留**原值**透传，maneuver 只在能确定时填 —— 绝不编映射表。
struct NavigationState {
  bool operator==(const NavigationState&) const = default;
  bool valid{};
  uint8_t active{};                    // 是否正在导航
  Maneuver maneuver{Maneuver::None};
  uint32_t maneuver_code{};            // 原始 action 码（无法映射时保留原值）
  std::array<char,kMaxTextLong> road_name{};       // 当前道路
  std::array<char,kMaxTextLong> next_road_name{};  // 下一段路
  std::array<char,32> icon{};          // 转向图标名（原样透传，供页面选图）
  std::array<char,32> destination{};
  uint32_t distance_to_maneuver_m{};   // 到下一动作的距离（米）
  uint32_t distance_remaining_m{};     // 全程剩余距离
  uint32_t time_remaining_s{};         // 剩余时间
  uint64_t eta_epoch_s{};              // 预计到达（unix 秒）
  uint8_t destination_reached{};
  uint16_t lane_bitmap{};              // 车道指示位图
};

// ── 电话 / 通讯录 ── AUDIT 2.7、7.1
struct TelephonyState {
  bool operator==(const TelephonyState&) const = default;
  uint8_t call_state{};            // 0=空闲 1=来电 2=拨出 3=通话中 4=保持
  std::array<char,kMaxTextLong> caller{};
  std::array<char,24> caller_number{};
  uint32_t call_duration_s{};
  uint8_t signal_bars{};           // HFP 信号强度（0..5）
  uint8_t battery_pct{};           // HFP 电量
  uint8_t contacts_ready{}; uint16_t contact_count{};
  uint8_t call_log_ready{}; uint16_t call_log_count{};
  uint8_t dtmf_supported{};
};

// ── 链路/会话：激活、内容加密、文件传输/OTA、多设备 ── AUDIT 3.x、5.3
struct LinkState {
  bool operator==(const LinkState&) const = default;
  uint8_t activation_state{};      // 0=未知 1=未激活 2=激活中 3=已激活
  uint8_t content_encryption{};    // 0=关 1=要求 2=已启用
  uint8_t file_transfer_active{};
  uint32_t file_transfer_bytes{};
  uint32_t file_transfer_total{};
  uint8_t ota_state{};             // 0=无 1=下载中 2=校验 3=可安装 4=失败
  uint8_t session_count{};
  std::array<std::array<char,32>,kMaxSessionList> sessions{};
  std::array<uint8_t,kMaxSessionList> session_active{};
  std::array<char,32> active_session{};
};

struct SessionSnapshot {
  SelectionMode mode{}; std::array<char,32> selected{},active{}; std::array<SourceState,kMaxSources> sources{}; std::size_t source_count{}; uint64_t video_dropped{},audio_dropped{},control_dropped{}; std::array<SessionEvent,kMaxControls> controls{}; std::size_t control_count{};
  // ↓ 本轮新增（全部为值语义，snapshot() 一把拷出）
  MediaInfo media{}; DisplayConfig display{}; InteractionState input{};
  AudioState audio{}; VehicleState vehicle{}; TelephonyState telephony{}; LinkState link{}; NavigationState nav{};
};

class SessionCore {
public:
 bool upsert_source(std::string_view id,InputSourceKind kind,bool connected); bool set_selection(SelectionMode mode,std::string_view id={});
 // 指定 Core 桌面源。多个外部输入同时在线时，自动模式会选中它并要求手动选择
 // （PROJECT_ARCHITECTURE §2 第 4 条），而不是按优先级自动挑一个输入。
 bool set_desktop_source(std::string_view id);
 bool register_control_sink(std::string_view source,std::function<void(const ControlEvent&)> sink); void unregister_control_sink(std::string_view source);
 bool submit_video(std::string_view source,const VideoFrame& frame); bool submit_audio(std::string_view source,const AudioChunk& chunk); bool route_control(const ControlEvent& event);
 bool latest_video(VideoFrame&out)const; bool latest_audio(AudioChunk&out)const;
 // 状态查询用的廉价访问器：不复制 256 KiB 的帧体，只报告有没有帧、编码是什么。
 // （桌面是 SVG 帧，页面需要知道该走 SVG 渲染还是 H.264 解码通路。）
 bool has_video()const; VideoEncoding active_video_encoding()const; SessionSnapshot snapshot()const;

 // ── 新增写入面：每个 setter 只更新自己那块，返回"是否真的有变化"──────
 // 返回值用于避免页面无意义重绘（false = 与上次相同，调用方可跳过）。
 bool set_media_info(const MediaInfo& info);
 bool set_artwork(uint32_t revision,const char* data,std::size_t size); // 存进 Convert 存储，状态里只留 revision
 bool set_lyrics(uint32_t revision,std::string_view lrc);
 bool set_display_config(const DisplayConfig& cfg);
 bool set_input_state(const InteractionState& st);
 bool set_audio_state(const AudioState& st);
 bool set_vehicle_state(const VehicleState& st);
 bool set_navigation_state(const NavigationState& st);
 bool set_telephony_state(const TelephonyState& st);
 bool set_link_state(const LinkState& st);
 // 便捷路径：只改音频闪避（导航播报压低/恢复），避免整块覆盖。
 bool set_ducking(bool nav_active,int32_t media_ppm,int32_t nav_ppm);
 // 便捷路径：只改一个硬键 keycode（回传确认用）。
 bool note_hard_key(uint32_t keycode);
 // 便捷路径：多设备会话表维护（upsert + 置活跃）。
 bool upsert_session(std::string_view id,bool active); bool set_active_session(std::string_view id);
 // 封面/歌词正文的读取面（页面按 revision 变化再调，避免每帧拷 128 KiB）。
 bool artwork(const char** data,std::size_t* size,uint32_t* revision)const;
 bool lyrics(std::string_view* out,uint32_t* revision)const;
private:
 struct SinkSlot { std::array<char,32> id{}; std::function<void(const ControlEvent&)> sink{}; };
 void resolve_active_locked(); void log_locked(SessionEvent::Type,std::string_view,uint64_t=0); static void copy_text(std::array<char,32>&,std::string_view);
 // 定长文本拷贝：按 UTF-8 边界回退，绝不劈开多字节字符（踩过的坑）。
 static void copy_text(std::array<char,kMaxTextLong>&,std::string_view);
 template <std::size_t N> static void copy_text(std::array<char,N>& dst,std::string_view src);
 mutable std::mutex mutex_; std::array<SourceState,kMaxSources> sources_{}; std::size_t source_count_{}; SelectionMode mode_{SelectionMode::Automatic}; std::array<char,32> selected_{},active_{},desktop_{};
 VideoFrame latest_video_{}; AudioChunk latest_audio_{}; bool has_video_{},has_audio_{}; uint64_t video_dropped_{},audio_dropped_{},control_dropped_{},event_sequence_{};
 std::array<SessionEvent,kMaxControls> controls_{}; std::size_t control_count_{}; std::array<SinkSlot,kMaxSources> sinks_{};
 // ↓ 本轮新增的存储（含两块大缓冲，所以 SessionCore 不可放到栈上反复拷贝）
 MediaInfo media_{}; DisplayConfig display_{}; InteractionState input_{};
 AudioState audio_{}; VehicleState vehicle_{}; TelephonyState tele{}; LinkState link_{}; NavigationState nav_{};
 std::array<char,kMaxArtwork> artwork_{}; std::size_t artwork_size_{}; uint32_t artwork_rev_{};
 std::array<char,kMaxLyrics> lyrics_{}; std::size_t lyrics_size_{}; uint32_t lyrics_rev_{};
};
} // namespace mvp
