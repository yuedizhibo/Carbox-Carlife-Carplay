#include "wirelesscarplay/real_media_store.hpp"
#include <cstring>
namespace mvp { namespace {
using namespace catplay_media;
constexpr auto Ipc=RealMediaSnapshot::Ipc::Connected;
void append_record(std::string& out,const Header& h,const uint8_t* payload){auto raw=encode_header(h);out.append(reinterpret_cast<const char*>(raw.data()),raw.size());if(h.payload_bytes)out.append(reinterpret_cast<const char*>(payload),h.payload_bytes);}
// CPMF 的 audio_type（p0，0..5）→ AudioRole。与 Core/Web 的 media_audio_type 名字表一一对应：
// 0 default / 1 alert / 2 media / 3 telephony / 4 speech-recognition / 5 compatibility。
// 其中 CarLife 的导航播报走 type=1（见 MAPPING §3），因此 1 归为 Navigation：它是闪避的触发者。
uint32_t audio_role_for_type(uint32_t type){switch(type){case 1:return uint32_t(AudioRole::Navigation);case 3:return uint32_t(AudioRole::Telephony);case 4:return uint32_t(AudioRole::Voice);case 5:return uint32_t(AudioRole::Alert);default:return uint32_t(AudioRole::Media);}}
}
void RealMediaStore::sync_screen_locked(uint32_t role) {
  auto& out=snapshot_.screens[role]; const auto& in=video_[role];
  out.role=role; out.stream_id=in.started?in.id:0; out.config_bytes=uint32_t(in.config_size); out.queued_frames=uint32_t(in.count); out.config_generation=in.config_generation; out.selected=selected_;
  // 帧率：实测值来自收帧计数，目标值来自 set_target_fps（属于请求，不是降载）。
  out.frames_received=in.frames_received; out.actual_fps=in.actual_fps; out.target_fps=target_fps_[role];
  out.state=selected_?in.state:RealMediaSnapshot::Video::Inactive;
  if(role==0){snapshot_.video=out.state;snapshot_.video_stream=out.stream_id;snapshot_.video_config_generation=out.config_generation;}
}
void RealMediaStore::clear_media_locked() {
  snapshot_.session=RealMediaSnapshot::Session::Inactive;snapshot_.audio_count=0;snapshot_.session_id=0;
  hello_=false;last_heartbeat_pts=0;next_audio_generation_=0;audio_gain_generation_=0;audio_gain_ppm_=1000000;audio_gain_duration_ms_=0;audio_={};
  // 闪避的当前增益归位，但【保留用户配置】（ratio/transition 是配置而非会话状态）。
  duck_active_=false;media_ppm_=1000000;nav_ppm_=1000000;
  snapshot_.ducking.active=false;snapshot_.ducking.media_ppm=1000000;snapshot_.ducking.nav_ppm=1000000;snapshot_.ducking.generation=0;
  snapshot_.ducking.ratio_ppm=duck_ratio_ppm_;snapshot_.ducking.transition_ms=duck_transition_ms_;
  // 元数据/封面/歌词也是会话状态：不清就会让上一会话的封面泄漏到新会话。
  relay_.reset();snapshot_.media=RealMediaSnapshot::Media{};
  // 帧率目标保留（配置），只清请求与实测窗口。
  rate_request_fps_={};rate_requested_={};last_rate_request_={};
  for(uint32_t role=0;role<kVideoRoles;++role){auto&v=video_[role];v.started=false;v.have_config=false;v.waiting_keyframe=true;v.have_sequence=false;v.keyframe_requested=false;v.state=RealMediaSnapshot::Video::Inactive;v.role=role;v.id=v.config_id=v.width=v.height=0;v.config_generation=v.last_sequence=0;v.config_size=v.begin=v.count=0;v.last_keyframe_sent={};v.frames_received=v.frames_in_window=0;v.actual_fps=0;v.fps_window_start={};sync_screen_locked(role);}
}
// 注意：不要在这里改 ipc 或清空媒体面。此前写成 snapshot_.ipc=Ipc; 是非法 C++
// （Ipc 是 RealMediaSnapshot 的嵌套枚举，不能裸用），导致 g++ 编译失败、
// 而部署脚本未把编译失败当致命错误 —— 结果所有 C++ 改动静默未上线。
// ipc 由 ServerHello/会话路径置为 Connected。
void RealMediaStore::transport_connected(){(void)0;}
bool RealMediaStore::push_video_frame_locked(VideoStream&v,uint64_t sequence,uint64_t pts,bool keyframe,bool discontinuity,std::span<const uint8_t>annexb){
  const bool gap=v.have_sequence&&sequence!=v.last_sequence+1;
  if(gap||discontinuity){v.waiting_keyframe=true;v.begin=v.count=0;v.state=RealMediaSnapshot::Video::WaitingKeyframe;request_keyframe_locked(v);}
  if(v.have_sequence&&sequence<=v.last_sequence)return false;
  v.have_sequence=true;v.last_sequence=sequence;
  // 实测帧率统计的是【手机实际推来的帧】，不是入 ring 的帧：
  // 这样 actual_fps 才能与 target_fps 对得上，也能分辨“手机真的只给 30”与“我们丢了帧”。
  update_fps_locked(v);
  if(!selected_){sync_screen_locked(v.role);return true;}
  const bool key=keyframe&&has_annexb_nal(annexb,5);
  if(v.waiting_keyframe&&!key){++snapshot_.video_dropped;sync_screen_locked(v.role);return true;}
  if(key){v.waiting_keyframe=false;v.keyframe_requested=false;if(v.count)v.begin=v.count=0;}
  if(v.count==kVideoFrames){v.begin=(v.begin+1)%kVideoFrames;--v.count;++snapshot_.video_dropped;++snapshot_.screens[v.role].frames_dropped;}
  auto&slot=v.frames[(v.begin+v.count)%kVideoFrames];slot.sequence=sequence;slot.pts=pts;slot.config_id=v.config_id;slot.width=v.width;slot.height=v.height;slot.keyframe=key;slot.size=uint32_t(annexb.size());slot.payload.resize(annexb.size());if(!annexb.empty())std::memcpy(slot.payload.data(),annexb.data(),annexb.size());++v.count;v.state=RealMediaSnapshot::Video::Active;sync_screen_locked(v.role);return true;
}
bool RealMediaStore::push_audio_locked(AudioStream&a,uint64_t sequence,uint64_t pts,bool discontinuity,std::span<const uint8_t>pcm){
  return push_audio_payload_locked(a,sequence,pts,discontinuity,false,0,pcm);
}
// 唯一入队实现。PCM：按声明格式算帧长，必须整除（不重采样、不下混、不拆包）；
// opaque：压缩载荷，**一个字节都不动**，帧数由编码器声明（p3）。
bool RealMediaStore::push_audio_payload_locked(AudioStream&a,uint64_t sequence,uint64_t pts,bool discontinuity,bool opaque,uint32_t frames,std::span<const uint8_t>pcm){
  if(pcm.empty()||pcm.size()>kMaxAudioPayloadBytes)return false;
  uint32_t declared=frames;
  if(!opaque){
    // 每帧字节数按【该流声明的格式】算（不再硬编码 16bit）：8/16/24/32 位容器与多声道都能如实入队。
    const uint32_t frame_bytes=effective_bytes_per_frame(a);
    if(!frame_bytes||pcm.size()%frame_bytes)return false;
    declared=uint32_t(pcm.size()/frame_bytes);
  }
  if(!declared)return false;
  const bool broken=discontinuity||(a.has_sequence&&sequence!=a.last_sequence+1);
  if(a.has_sequence&&sequence<=a.last_sequence)return false;
  a.has_sequence=true;a.last_sequence=sequence;
  if(!selected_)return true;
  if(a.count==kAudioChunks){a.begin=(a.begin+1)%kAudioChunks;--a.count;++snapshot_.audio_dropped;}
  auto&slot=a.chunks[(a.begin+a.count)%kAudioChunks];slot.sequence=sequence;slot.pts=pts;slot.frames=declared;slot.size=uint32_t(pcm.size());slot.discontinuity=broken;slot.opaque=opaque;
  std::memcpy(slot.payload.data(),pcm.data(),pcm.size());
  ++a.count;a.state=broken?RealMediaSnapshot::Audio::State::Discontinuous:RealMediaSnapshot::Audio::State::Active;
  // 重建快照（而非只改 state）：opaque 标记与格式必须同步，
  // 否则“先 start 后 push”的订阅者永远看不到本流是压缩的。
  publish_audio_locked();
  return true;
}
// 如实汇报的每帧字节数：生产者声明了就用它，否则按 16 位容器（既有行为）。
uint32_t RealMediaStore::effective_bytes_per_frame(const AudioStream& a) const {
  const uint32_t total = a.format.total_bits ? a.format.total_bits : (a.format.valid_bits ? a.format.valid_bits : 16);
  const uint32_t want = a.channels * total / 8;
  if (a.format.bytes_per_frame && a.format.bytes_per_frame == want) return a.format.bytes_per_frame;
  return want;
}
// 重建音频快照的唯一执行点：避免“codec/channels/role/bytes_per_frame 只在部分路径被填”的不一致。
// only_stream_id==0 表示重建全部打开流。
void RealMediaStore::publish_audio_locked(uint32_t only_stream_id){
  snapshot_.audio_count=0;
  if(!selected_)return;
  for(auto&x:audio_){
    if(!x.open)continue;
    if(only_stream_id&&x.id!=only_stream_id)continue;
    auto& out=snapshot_.audio[snapshot_.audio_count++];
    out.stream_id=x.id;out.audio_type=x.type;out.generation=x.generation;out.state=x.state;
    out.sample_rate=x.rate;out.channels=x.channels;out.role=x.role;
    out.codec=x.format.declared?uint32_t(x.format.codec):2u;
    out.bits=x.format.total_bits?uint32_t(x.format.total_bits):16u;
    out.bytes_per_frame=effective_bytes_per_frame(x);
    // 压缩流：载荷是 AAC-ELD/ALAC/OPUS，本层未解码、未重采样，浏览器是唯一解码方。
    out.opaque=(x.format.declared&&(x.format.codec==4||x.format.codec==5||x.format.codec==6||x.format.codec==7))?uint8_t(1):uint8_t(0);
  }
}
// 增益/闪避的唯一执行点：同时更新 CPMF 的 AudioGain 通道与快照里的 ppm。
void RealMediaStore::apply_gain_locked(uint32_t duration_ms,int32_t ppm){
  if(ppm<0)ppm=0;if(ppm>1000000)ppm=1000000;
  audio_gain_duration_ms_=duration_ms;audio_gain_ppm_=uint32_t(ppm);++audio_gain_generation_;
  media_ppm_=ppm;
  snapshot_.ducking.media_ppm=ppm;snapshot_.ducking.nav_ppm=nav_ppm_;snapshot_.ducking.generation=audio_gain_generation_;
}
// 1 秒滑动窗实测帧率。窗口未满时不清零，保证首秒也能报出一个合理值。
void RealMediaStore::update_fps_locked(VideoStream& v){
  ++v.frames_received;++v.frames_in_window;
  const auto now=std::chrono::steady_clock::now();
  if(v.fps_window_start==std::chrono::steady_clock::time_point{})v.fps_window_start=now;
  const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-v.fps_window_start).count();
  if(elapsed>=1000){
    v.actual_fps=uint32_t(v.frames_in_window*1000/uint64_t(elapsed));
    v.frames_in_window=0;v.fps_window_start=now;
    sync_screen_locked(v.role);
  }
}
bool RealMediaStore::transport_ready(){std::lock_guard lock(mutex_);if(!hello_)clear_media_locked();hello_=true;snapshot_.ipc=Ipc;return true;}
bool RealMediaStore::begin_session(uint64_t session_id){
  std::lock_guard lock(mutex_);if(!session_id)return false;
  if(snapshot_.session==RealMediaSnapshot::Session::Active&&session_id==snapshot_.session_id)return false;
  clear_media_locked();hello_=true;snapshot_.ipc=Ipc;snapshot_.session=RealMediaSnapshot::Session::Active;snapshot_.session_id=session_id;return true;
}
bool RealMediaStore::end_session(uint64_t session_id){
  std::lock_guard lock(mutex_);if(snapshot_.session!=RealMediaSnapshot::Session::Active||session_id!=snapshot_.session_id)return false;
  clear_media_locked();hello_=true;snapshot_.ipc=Ipc;return true;
}
bool RealMediaStore::set_video_stream(uint32_t role,uint32_t stream_id,uint32_t width,uint32_t height,std::span<const uint8_t>sps_pps){
  if(role>=kVideoRoles||!stream_id||!valid_dimensions(width,height))return false;
  std::lock_guard lock(mutex_);if(snapshot_.session!=RealMediaSnapshot::Session::Active)return false;
  if(video_id_locked(stream_id)||(video_[role].started&&video_[role].id!=stream_id))return false;
  auto&v=video_[role];
  if(v.started&&v.have_config&&v.width==width&&v.height==height&&sps_pps.size()==v.config_size&&(sps_pps.empty()||std::memcmp(v.config.data(),sps_pps.data(),sps_pps.size())==0))return true;
  v.started=true;v.id=stream_id;v.width=width;v.height=height;v.config_size=0;v.have_config=false;v.waiting_keyframe=true;v.have_sequence=false;v.keyframe_requested=false;v.begin=v.count=0;v.config_id=(v.config_id%UINT32_MAX)+1;
  if(valid_annexb(sps_pps)&&has_annexb_nal(sps_pps,7)&&has_annexb_nal(sps_pps,8)&&sps_pps.size()<=kMaxVideoConfigBytes){std::memcpy(v.config.data(),sps_pps.data(),sps_pps.size());v.config_size=sps_pps.size();v.have_config=true;v.state=RealMediaSnapshot::Video::WaitingKeyframe;request_keyframe_locked(v);}
  else if(!sps_pps.empty())v.state=RealMediaSnapshot::Video::Failed;
  else v.state=RealMediaSnapshot::Video::WaitingKeyframe;
  ++v.config_generation;sync_screen_locked(role);return true;
}
bool RealMediaStore::push_video_frame(uint32_t role,uint64_t sequence,uint64_t pts,bool keyframe,std::span<const uint8_t>annexb){
  if(role>=kVideoRoles||!sequence||!valid_annexb(annexb)||annexb.size()>kMaxVideoFrameBytes)return false;
  std::lock_guard lock(mutex_);if(snapshot_.session!=RealMediaSnapshot::Session::Active)return false;
  auto&v=video_[role];if(!v.started||!v.have_config)return false;
  return push_video_frame_locked(v,sequence,pts,keyframe,false,annexb);
}
bool RealMediaStore::end_video_stream(uint32_t role){
  std::lock_guard lock(mutex_);if(role>=kVideoRoles)return false;auto&v=video_[role];if(!v.started)return false;
  v.started=false;v.have_config=false;v.waiting_keyframe=true;v.have_sequence=false;v.keyframe_requested=false;v.state=RealMediaSnapshot::Video::Inactive;v.id=0;v.config_size=v.begin=v.count=0;sync_screen_locked(role);return true;
}
bool RealMediaStore::start_audio(uint32_t stream_id,uint32_t audio_type,uint32_t sample_rate,uint32_t channels){
  // 旧签名保留（既有调用点：carlife_input.cpp:503、catplay_media_client.cpp），格式默认为未声明。
  return start_audio(stream_id,audio_type,sample_rate,channels,catplay_media::AudioFormat{});
}
bool RealMediaStore::start_audio(uint32_t stream_id,uint32_t audio_type,uint32_t sample_rate,uint32_t channels,const catplay_media::AudioFormat& format){
  // 多声道/高采样率：只做【合理性边界】，不做能力裁剪 —— 多声道与杜比靠“原样透传”支持。
  // 声道 1..8（5.1=6、7.1=8）；采样率 8000..192000；位深 8/16/24/32（在 format 里校验）。
  // 超界一律拒绝（避免有人拿 255 声道来分配缓冲）。Convert 层没有日志设施，
  // 也不应为记一条日志引入 stdout 依赖，因此以返回值 false 告知调用方自行记录。
  if(!stream_id||!valid_rate(sample_rate)||channels<1||channels>catplay_media::kMaxAudioChannels||audio_type>5)return false;
  std::lock_guard lock(mutex_);if(snapshot_.session!=RealMediaSnapshot::Session::Active||audio_locked(stream_id))return false;
  // 同一 role 只占一条独立 ring（AUDIT 6.1）：复用空槽即天然满足（每流一槽一 ring）。
  AudioStream*slot=nullptr;for(auto&a:audio_)if(!a.open){slot=&a;break;}if(!slot)return false;
  slot->open=true;slot->id=stream_id;slot->type=audio_type;slot->rate=sample_rate;slot->channels=channels;slot->generation=++next_audio_generation_;slot->format=format;slot->role=format.role?format.role:audio_role_for_type(audio_type);slot->descriptor.fill(0);slot->descriptor[0]=96;slot->descriptor[1]=1;
  if(format.declared)catplay_media::encode_audio_format(slot->descriptor.data(),format);
  slot->state=RealMediaSnapshot::Audio::State::StartedNoData;
  publish_audio_locked();
  return true;
}
// 流已开好后再声明/更新格式（适配器从原生消息里晚一步拿到格式时用）。
bool RealMediaStore::set_audio_format(uint32_t stream_id,const catplay_media::AudioFormat& format){
  std::lock_guard lock(mutex_);auto*a=audio_locked(stream_id);if(!a)return false;
  a->format=format;if(format.role)a->role=format.role;
  a->descriptor.fill(0);a->descriptor[0]=96;a->descriptor[1]=1;
  if(format.declared)catplay_media::encode_audio_format(a->descriptor.data(),format);
  publish_audio_locked();return true;
}
bool RealMediaStore::set_audio_role(uint32_t stream_id,uint32_t role){
  if(role>uint32_t(AudioRole::Voice))return false;
  std::lock_guard lock(mutex_);auto*a=audio_locked(stream_id);if(!a)return false;
  a->role=role;publish_audio_locked();return true;
}
// 导航压媒体：语义照抄 AudioTrackManagerDualNormal.java:376-395
// （导航播报 setVolume(maxVolume / mMusicAudioTrackVolumReduceRatio)，导航轨本身不动）。
bool RealMediaStore::set_ducking(bool nav_active){
  std::lock_guard lock(mutex_);
  if(duck_active_==nav_active)return false;
  duck_active_=nav_active;
  snapshot_.ducking.active=nav_active;
  nav_ppm_=1000000;
  apply_gain_locked(duck_transition_ms_,nav_active?int32_t(duck_ratio_ppm_):1000000);
  return true;
}
bool RealMediaStore::set_duck_config(uint32_t ratio_ppm,uint32_t transition_ms){
  if(!ratio_ppm||ratio_ppm>1000000||transition_ms>60000)return false;
  std::lock_guard lock(mutex_);
  duck_ratio_ppm_=ratio_ppm;duck_transition_ms_=transition_ms;
  snapshot_.ducking.ratio_ppm=ratio_ppm;snapshot_.ducking.transition_ms=transition_ms;
  if(duck_active_)apply_gain_locked(transition_ms,int32_t(ratio_ppm));
  return true;
}
bool RealMediaStore::ducking_active() const{std::lock_guard lock(mutex_);return duck_active_;}
// 帧率：只记录目标并挂一个待下发请求，【不】因此改变码率/分辨率/帧的取舍。
bool RealMediaStore::set_target_fps(uint32_t role,uint32_t fps){
  if(role>=kVideoRoles||fps<1||fps>240)return false;
  std::lock_guard lock(mutex_);
  if(target_fps_[role]==fps)return false;
  target_fps_[role]=fps;rate_request_fps_[role]=fps;rate_requested_[role]=true;
  last_rate_request_[role]=std::chrono::steady_clock::time_point{};  // 允许立即下发一次
  sync_screen_locked(role);
  return true;
}
bool RealMediaStore::take_rate_request(uint32_t& role,uint32_t& fps){
  std::lock_guard lock(mutex_);const auto now=std::chrono::steady_clock::now();
  for(uint32_t r=0;r<kVideoRoles;++r){
    if(!rate_requested_[r]||!video_[r].started)continue;
    if(last_rate_request_[r]!=std::chrono::steady_clock::time_point{}&&now-last_rate_request_[r]<std::chrono::milliseconds(kRateRequestIntervalMs))continue;
    rate_requested_[r]=false;last_rate_request_[r]=now;role=r;fps=rate_request_fps_[r];return true;
  }
  return false;
}
bool RealMediaStore::push_audio(uint32_t stream_id,uint64_t sequence,uint64_t pts,std::span<const uint8_t>pcm){
  if(!sequence||pcm.empty()||pcm.size()>kMaxAudioPayloadBytes)return false;
  std::lock_guard lock(mutex_);if(snapshot_.session!=RealMediaSnapshot::Session::Active)return false;
  auto*a=audio_locked(stream_id);if(!a)return false;return push_audio_locked(*a,sequence,pts,false,pcm);
}
// 压缩载荷直灌：不做任何帧长/样本运算，字节原样入队与转发。
bool RealMediaStore::push_audio_opaque(uint32_t stream_id,uint64_t sequence,uint64_t pts,uint32_t frames,std::span<const uint8_t>payload){
  if(!sequence||!frames||payload.empty()||payload.size()>kMaxAudioPayloadBytes)return false;
  std::lock_guard lock(mutex_);if(snapshot_.session!=RealMediaSnapshot::Session::Active)return false;
  auto*a=audio_locked(stream_id);if(!a)return false;return push_audio_payload_locked(*a,sequence,pts,false,true,frames,payload);
}
bool RealMediaStore::end_audio(uint32_t stream_id){
  std::lock_guard lock(mutex_);auto*a=audio_locked(stream_id);if(!a)return false;*a={};
  publish_audio_locked();return true;
}
bool RealMediaStore::set_audio_gain(uint32_t duration_ms,uint32_t gain_ppm){
  // 与 set_ducking 共用唯一执行点，因此客户端只有一套渐变逻辑
  // （CarLife 的 A1 路径直接调本函数，见 carlife_input.cpp:470/524）。
  if(gain_ppm>1000000)return false;
  std::lock_guard lock(mutex_);apply_gain_locked(duration_ms,int32_t(gain_ppm));return true;
}
void RealMediaStore::transport_closed(bool protocol_error){
  // 【关键】不能把 hello_ 的重置交给 clear_media_locked()（那会连媒体面一起清），
  // 但必须在这里重置：ServerHello 的校验是 if(hello_)return false，
  // 即第二次握手直接拒。重连时引擎会重发 ServerHello，
  // 若 hello_ 永不归零，每次重连的握手都被丢、ipc 也不再置为 connected。
  // 而 ServerHello 被拒会让 ingest() 返回 false，在上层表现为持续丢记录 + 反复重连。
  // 每次清空都会让浏览器正在读的流断掉、并被迫重等关键帧（一次往返回路），
  // 这正是高压下“掉帧 / 画面卡断”的放大器。
  // 真正的会话边界是 SessionBegin（新会话才清），由 ingest() 处理。
  // 保留最后一帧与 SPS/PPS，客户端在最坏情况下停在最后一帧而不是黑屏。
  // 【必须重置 hello_】ServerHello 的处理里写的是 if(hello_)return false;
  // 即“第二次握手直接拒”。而重连时引擎会重发 ServerHello。
  // 之前 hello_ 由 clear_media_locked() 重置，上一轮我为了“不断清媒体面”
  // 删掉了 transport_connected() 里那个调用，把 hello_ 的重置一起弄没了 ——
  // 导致每次重连的 ServerHello 都被丢、ipc 也不再置为 connected。
  // 正确做法：只重置握手状态，媒体面（帧/SPS/PPS/会话）一律保留。
  std::lock_guard lock(mutex_);
  hello_=false;
  snapshot_.ipc=protocol_error?RealMediaSnapshot::Ipc::ProtocolError:RealMediaSnapshot::Ipc::Unavailable;
  if(protocol_error)++snapshot_.protocol_dropped;
}
void RealMediaStore::request_keyframe_locked(VideoStream&v){if(snapshot_.session==RealMediaSnapshot::Session::Active&&v.started)v.keyframe_requested=true;}
RealMediaStore::VideoStream* RealMediaStore::video_id_locked(uint32_t id){for(auto&v:video_)if(v.started&&v.id==id)return&v;return nullptr;}
RealMediaStore::AudioStream* RealMediaStore::audio_locked(uint32_t id){for(auto&a:audio_)if(a.open&&a.id==id)return&a;return nullptr;}
bool RealMediaStore::ingest(const Header&h,std::span<const uint8_t>payload){
  std::lock_guard lock(mutex_);if(!valid_static(h,payload))return false;
  if(h.type==Type::ServerHello){if(hello_)return false;hello_=true;snapshot_.ipc=Ipc;return true;}
  if(!hello_)return false;
  if(h.type==Type::Heartbeat){if((h.session_id!=0&&h.session_id!=snapshot_.session_id)||h.pts<=last_heartbeat_pts)return false;last_heartbeat_pts=h.pts;return true;}
  if(h.type==Type::SessionBegin){if(snapshot_.session==RealMediaSnapshot::Session::Active&&h.session_id==snapshot_.session_id)return false;clear_media_locked();hello_=true;snapshot_.ipc=Ipc;snapshot_.session=RealMediaSnapshot::Session::Active;snapshot_.session_id=h.session_id;return true;}
  if(h.type==Type::SessionEnd){if(snapshot_.session!=RealMediaSnapshot::Session::Active||h.session_id!=snapshot_.session_id)return false;clear_media_locked();hello_=true;snapshot_.ipc=Ipc;return true;}
  if(snapshot_.session!=RealMediaSnapshot::Session::Active||h.session_id!=snapshot_.session_id)return false;
  switch(h.type){
    case Type::VideoStart:{
      // 与 AudioStart 同一类问题：同一 role 或同一 stream id 重新开流是正常事件
      // （会话重建、分辨率变化、手机重连都会发生），必须重置而不是拒绝。
      // 旧实现直接 return false，会让新流不被登记 —— 表现为画面在某一刻永久卡住。
      const uint32_t role=h.p0;if(role>=kVideoRoles)return false;
      VideoStream*slot=video_id_locked(h.stream_id);
      if(!slot)slot=&video_[role];
      const uint32_t use_role=slot->role;
      const uint32_t keep_id=h.stream_id;
      slot->started=true;slot->id=keep_id;
      slot->have_config=false;slot->waiting_keyframe=true;slot->have_sequence=false;
      slot->keyframe_requested=false;slot->config_id=0;
      slot->config_size=slot->begin=slot->count=0;
      slot->width=slot->height=0;slot->last_sequence=0;
      slot->state=RealMediaSnapshot::Video::WaitingConfig;
      sync_screen_locked(use_role);
      return true;
    }
    case Type::VideoConfig:{
      auto*v=video_id_locked(h.stream_id);if(!v||(v->have_config&&h.p3==v->config_id))return false;
      if(v->have_config&&h.p0==1&&v->width==h.p1&&v->height==h.p2&&v->config_size==payload.size()&&std::memcmp(v->config.data(),payload.data(),payload.size())==0){v->config_id=h.p3;sync_screen_locked(v->role);return true;}
      v->config_id=h.p3;v->width=h.p1;v->height=h.p2;v->config_size=0;v->have_config=false;v->waiting_keyframe=true;v->have_sequence=false;v->begin=v->count=0;
      if(h.p0==2)v->state=RealMediaSnapshot::Video::Unsupported;else if(h.p0!=1)v->state=RealMediaSnapshot::Video::Failed;else{std::memcpy(v->config.data(),payload.data(),payload.size());v->config_size=payload.size();v->have_config=true;v->state=RealMediaSnapshot::Video::WaitingKeyframe;request_keyframe_locked(*v);}
      ++v->config_generation;sync_screen_locked(v->role);return true;
    }
    case Type::VideoFrame:{
      auto*v=video_id_locked(h.stream_id);if(!v||!v->have_config||h.p3!=v->config_id||h.p1!=v->width||h.p2!=v->height)return false;
      return push_video_frame_locked(*v,h.sequence,h.pts,(h.flags&kFlagKeyframe)!=0,(h.flags&kFlagDiscontinuity)!=0,payload);
    }
    case Type::VideoEnd:{
      // stream_id==0 表示“结束全部视频流”：引擎在会话收尾时就是这么发的。
      // 旧实现只认带有效 stream_id 的结束记录，于是这些记录全被丢掉，
      // 导致“停止”语义丢失、重启流时状态不干净。
      bool any=false;
      for(uint32_t role=0;role<kVideoRoles;++role){
        auto&v=video_[role];
        if(!v.started)continue;
        if(h.stream_id&&v.id!=h.stream_id)continue;
        v.started=false;v.have_config=false;v.waiting_keyframe=true;v.have_sequence=false;v.keyframe_requested=false;v.state=RealMediaSnapshot::Video::Inactive;v.id=0;v.config_size=v.begin=v.count=0;sync_screen_locked(role);any=true;
      }
      // 即使没有匹配的流也要返回 true：这是“结束”语义的通知，不是无效记录，
      // 返回 false 会被上层计为 dropped 并污染诊断。
      return true;
    }
    case Type::AudioStart:{
      // 同一个 stream id 再次开流是正常事件（音频路由变化、会话重建、音乐起停都会发生），
      // 必须【重置】该流而不是拒绝：否则它会保留旧的 last_sequence，
      // 后续 chunk 全部撞上 sequence<=last_sequence 被丢弃 ——
      // 表现就是“有声音，过一会儿没声音”。
      AudioStream*slot=audio_locked(h.stream_id);
      if(!slot)for(auto&a:audio_)if(!a.open){slot=&a;break;}
      if(!slot)return false;
      const uint32_t keep_id=h.stream_id;
      const catplay_media::AudioFormat fmt=catplay_media::decode_audio_format(payload.data(),payload.size(),h.p2);
      *slot={};                                  // 清掉 has_sequence/last_sequence/chunks
      slot->open=true;slot->id=keep_id;slot->type=h.p0;slot->rate=h.p1;slot->channels=h.p2;
      slot->format=fmt;
      slot->role=fmt.role?fmt.role:audio_role_for_type(h.p0);
      slot->generation=++next_audio_generation_;
      if(payload.size()==slot->descriptor.size())std::memcpy(slot->descriptor.data(),payload.data(),payload.size());
      slot->state=RealMediaSnapshot::Audio::State::StartedNoData;
      publish_audio_locked();
      return true;
    }
    case Type::AudioChunk:{
      auto*a=audio_locked(h.stream_id);if(!a||a->type!=h.p0||a->rate!=h.p1||a->channels!=h.p2)return false;
      // 压缩载荷（AAC-ELD/ALAC/OPUS）：不做 PCM 帧长等式，帧数由 p3 声明，字节原样入队。
      // 本层不解码任何非 PCM 载荷，所以也不去验证它的内部结构。
      const bool opaque=(h.flags&kFlagOpaquePayload)!=0;
      return push_audio_payload_locked(*a,h.sequence,h.pts,(h.flags&kFlagDiscontinuity)!=0,opaque,h.p3,payload);
    }
    case Type::AudioEnd:{
      // 同上：stream_id==0 = 结束全部音频流。
      bool any=false;
      for(auto&a:audio_){
        if(!a.open)continue;
        if(h.stream_id&&a.id!=h.stream_id)continue;
        a={};any=true;
      }
      if(!any)return true;   // 无匹配流时也属于“已处理”，不污染 dropped 计数
      publish_audio_locked();
      return true;
    }
    case Type::AudioGain:{if(h.p3<=audio_gain_generation_)return false;audio_gain_duration_ms_=h.p0;audio_gain_ppm_=h.p1;audio_gain_generation_=h.p3;return true;}
    default:return false;
  }
}
bool RealMediaStore::request_keyframe(uint32_t stream_id){std::lock_guard lock(mutex_);auto*v=video_id_locked(stream_id);if(!selected_||!v)return false;request_keyframe_locked(*v);return v->keyframe_requested;}
bool RealMediaStore::take_keyframe_request(uint64_t&session_id,uint32_t&stream_id){std::lock_guard lock(mutex_);const auto now=std::chrono::steady_clock::now();for(auto&v:video_)if(v.keyframe_requested&&now-v.last_keyframe_sent>=std::chrono::milliseconds(500)){session_id=snapshot_.session_id;stream_id=v.id;if(!session_id||!stream_id)return false;v.keyframe_requested=false;v.last_keyframe_sent=now;return true;}return false;}
void RealMediaStore::set_selected(bool selected){std::lock_guard lock(mutex_);if(selected_==selected)return;selected_=selected;for(auto&v:video_){v.begin=v.count=0;v.have_sequence=false;if(selected_&&v.have_config){v.waiting_keyframe=true;v.state=RealMediaSnapshot::Video::WaitingKeyframe;request_keyframe_locked(v);}else if(selected_)v.state=v.started?RealMediaSnapshot::Video::WaitingConfig:RealMediaSnapshot::Video::Inactive;sync_screen_locked(v.role);}for(auto&a:audio_){a.begin=a.count=0;a.has_sequence=false;a.state=RealMediaSnapshot::Audio::State::StartedNoData;}publish_audio_locked();}
bool RealMediaStore::video_packet(uint32_t role,uint64_t after_sequence,std::string&out)const{std::lock_guard lock(mutex_);if(role>=kVideoRoles)return false;const auto&v=video_[role];if(!selected_||v.state!=RealMediaSnapshot::Video::Active||!v.config_size||!v.count)return false;std::size_t first=v.count;if(after_sequence==0){for(std::size_t i=0;i<v.count;++i)if(v.frames[(v.begin+i)%kVideoFrames].keyframe){first=i;break;}}else{for(std::size_t i=0;i<v.count;++i)if(v.frames[(v.begin+i)%kVideoFrames].sequence>after_sequence){first=i;break;}}// A caught-up client must receive 204 rather than being rewound to an old IDR.
if(first==v.count)return false;constexpr std::size_t kMaxFramesPerPacket=2;const auto frame_count=std::min(kMaxFramesPerPacket,v.count-first);bool include_config=after_sequence==0;std::size_t reserve=kHeaderBytes+v.config_size;for(std::size_t i=0;i<frame_count;++i){const auto&frame=v.frames[(v.begin+first+i)%kVideoFrames];if(frame.payload.empty())return false;include_config=include_config||frame.keyframe;reserve+=kHeaderBytes+frame.size;}out.clear();out.reserve(reserve);if(include_config){Header c{Type::VideoConfig,0,v.id,snapshot_.session_id,0,0,uint32_t(v.config_size),1,v.width,v.height,v.config_id};append_record(out,c,v.config.data());}for(std::size_t i=0;i<frame_count;++i){const auto&frame=v.frames[(v.begin+first+i)%kVideoFrames];Header f{Type::VideoFrame,uint16_t(frame.keyframe?kFlagKeyframe:0),v.id,snapshot_.session_id,frame.sequence,frame.pts,frame.size,1,frame.width,frame.height,frame.config_id};append_record(out,f,frame.payload.data());}return true;}
bool RealMediaStore::audio_packet(AudioCursor&cursor,std::string&out)const{std::lock_guard lock(mutex_);if(!selected_)return false;out.clear();bool any=false;if(cursor.gain_generation!=audio_gain_generation_&&audio_gain_generation_){Header gain{Type::AudioGain,0,0,snapshot_.session_id,0,0,0,audio_gain_duration_ms_,audio_gain_ppm_,0,uint32_t(audio_gain_generation_)};append_record(out,gain,nullptr);cursor.gain_generation=audio_gain_generation_;any=true;}for(std::size_t ai=0;ai<audio_.size();++ai){const auto&a=audio_[ai];if(!a.open||!a.count)continue;if(cursor.stream[ai]!=a.id){cursor.stream[ai]=a.id;cursor.sequence[ai]=0;}bool started=false;for(std::size_t i=0;i<a.count;++i){const auto&slot=a.chunks[(a.begin+i)%kAudioChunks];if(slot.sequence<=cursor.sequence[ai])continue;if(!started){Header start{Type::AudioStart,0,a.id,snapshot_.session_id,0,0,16,a.type,a.rate,a.channels,1};append_record(out,start,a.descriptor.data());started=true;}Header chunk{Type::AudioChunk,uint16_t((slot.discontinuity?kFlagDiscontinuity:0)|(slot.opaque?kFlagOpaquePayload:0)),a.id,snapshot_.session_id,slot.sequence,slot.pts,slot.size,a.type,a.rate,a.channels,slot.frames};append_record(out,chunk,slot.payload.data());cursor.sequence[ai]=slot.sequence;any=true;}}return any;}
bool RealMediaStore::audio_packet(std::string&out)const{AudioCursor cursor;return audio_packet(cursor,out);}
RealMediaSnapshot RealMediaStore::snapshot()const{std::lock_guard lock(mutex_);
  // 元数据正文存在 MediaRelay 的定长缓冲里（自持锁），这里只把 revision/大小同步进返回值：
  // 页面/输出侧据此判断该不该去取正文，避免每帧拷 128 KiB。
  // 用局部副本而不是就地改 snapshot_：它不是 mutable，而快照体本身很小
  //（大 ring 在 AudioStream/VideoStream 里，不在 RealMediaSnapshot 里）。
  RealMediaSnapshot out=snapshot_;
  const char* art=nullptr;std::size_t art_size=0;uint32_t art_rev=0;
  out.media.has_artwork=relay_.artwork(&art,&art_size,&art_rev);
  out.media.artwork_revision=art_rev;out.media.artwork_bytes=uint32_t(art_size);
  std::string_view lrc;uint32_t lyr_rev=0;
  out.media.has_lyrics=relay_.lyrics(&lrc,&lyr_rev);
  out.media.lyrics_revision=lyr_rev;out.media.lyrics_bytes=uint32_t(lrc.size());
  out.media.info_generation=relay_.media_info_generation();
  out.media.valid=relay_.media_info().valid;
  return out;}
} // namespace mvp
