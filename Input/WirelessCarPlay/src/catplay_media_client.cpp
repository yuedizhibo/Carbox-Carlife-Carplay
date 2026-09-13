#include "wirelesscarplay/catplay_media_client.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#if defined(__unix__)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
namespace mvp {
// catplay_media::* 是 mvp 的**嵌套**命名空间，里面的名字在 mvp 里并不自动可见，
// 所以这里显式引入本文件要用到的那些（否则连 be32 都编不过）。
namespace ext = catplay_media::ext;
using catplay_media::be16; using catplay_media::be32; using catplay_media::be64;
using catplay_media::put_be16; using catplay_media::put_be32; using catplay_media::put_be64;
using catplay_media::ext::DType; using catplay_media::ext::UType;
using catplay_media::ext::FieldReader; using catplay_media::ext::copy_utf8; using catplay_media::ext::as_view;
namespace {
constexpr auto kRetry=std::chrono::milliseconds(500), kHelloDeadline=std::chrono::milliseconds(500), kLivenessDeadline=std::chrono::milliseconds(10000);
// 本输入在 Core 里的唯一源名（源表/控制面/发布闸门都用它）。
constexpr std::string_view kSourceId="catplay-real";

// CarPlay 音频类型 → 语义角色。序号出处：Reference/CatPlaySource/carplay/catplay_carplay/
// src/msg/streams.rs 的 `enum AudioType { Default, Alert, Media, Telephony, SpeechRecognition,
// Compatibility }`（0..5，与 CPMF AudioStart/AudioChunk 的 p0 上限 5 一致）。
AudioRole role_from_audio_type(uint32_t t) {
  switch (t) {
    case 0: return AudioRole::Media;            // Default
    case 1: return AudioRole::Alert;            // Alert
    case 2: return AudioRole::Media;            // Media
    case 3: return AudioRole::Telephony;        // Telephony
    case 4: return AudioRole::Voice;            // SpeechRecognition
    case 5: return AudioRole::Media;            // Compatibility
    default: return AudioRole::Unknown;
  }
}

// 16 字节 AudioStart 描述符里声明的 codec → Core 的 AudioCodec。序号与
// AudioFormat.codec 一致（0 未声明 1 Pcm 2 Pcm16 3 Pcm24 4 Alac 5 AacLc 6 AacEld 7 Opus），
// 但这里**显式打表**，不依赖两个枚举的数值巧合。未声明/越界一律按 PCM16（旧生产者行为）。
AudioCodec codec_from_audio_format(uint8_t c) {
  switch (c) {
    case 1: return AudioCodec::Pcm;
    case 2: return AudioCodec::Pcm16;
    case 3: return AudioCodec::Pcm24;
    case 4: return AudioCodec::Alac;
    case 5: return AudioCodec::AacLc;
    case 6: return AudioCodec::AacEld;
    case 7: return AudioCodec::Opus;
    default: return AudioCodec::Pcm16;
  }
}
// 描述符里声明的 role → AudioRole（0 未声明 → 交给 role_from_audio_type 推导）。
AudioRole role_from_declared_role(uint8_t r) { return (r>=1&&r<=5)?static_cast<AudioRole>(r):AudioRole::Unknown; }

// 硬键名 → CarPlay 标识。类别放在高 8 位：0=MediaButton，1=TelephonyButton。
// 标识号出处：Reference/CatPlaySource/carplay/catplay_hid/src/media_buttons.rs（0..6）
// 与 .../telephony.rs（0..17）。注意**不是 Android keycode**：CarPlay 侧走的是
// HID Consumer/Telephony 按钮，两者编号空间独立，所以必须带类别位。
struct KeyName { std::string_view name; uint32_t code; };
constexpr uint32_t kKindMedia = 0u << 16, kKindTel = 1u << 16, kKindKnob = 2u << 16;
constexpr KeyName kKeyNames[] = {
  {"media.play", kKindMedia | 1}, {"play", kKindMedia | 1},
  {"media.pause", kKindMedia | 2}, {"pause", kKindMedia | 2},
  {"media.play_pause", kKindMedia | 3}, {"play_pause", kKindMedia | 3}, {"playpause", kKindMedia | 3},
  {"media.next", kKindMedia | 4}, {"next", kKindMedia | 4},
  {"media.prev", kKindMedia | 5}, {"prev", kKindMedia | 5},
  {"media.ac_nav", kKindMedia | 6}, {"ac_nav", kKindMedia | 6},
  {"tel.up", kKindTel | 0}, {"tel.hook", kKindTel | 1}, {"hook", kKindTel | 1},
  {"tel.flash", kKindTel | 2}, {"flash", kKindTel | 2},
  {"tel.drop", kKindTel | 3}, {"drop", kKindTel | 3},
  {"tel.mute", kKindTel | 4}, {"mute", kKindTel | 4},
  {"tel.0", kKindTel | 5}, {"tel.1", kKindTel | 6}, {"tel.2", kKindTel | 7}, {"tel.3", kKindTel | 8},
  {"tel.4", kKindTel | 9}, {"tel.5", kKindTel | 10}, {"tel.6", kKindTel | 11}, {"tel.7", kKindTel | 12},
  {"tel.8", kKindTel | 13}, {"tel.9", kKindTel | 14},
  {"tel.star", kKindTel | 15}, {"tel.pound", kKindTel | 16}, {"tel.delete", kKindTel | 17},
  {"select", kKindKnob | 1}, {"enter", kKindKnob | 1},
  {"home", kKindKnob | 2}, {"back", kKindKnob | 3},
  {"left", kKindKnob | 4}, {"right", kKindKnob | 5},
  {"up", kKindKnob | 6}, {"down", kKindKnob | 7},
  {"wheel_left", kKindKnob | 8}, {"wheel_right", kKindKnob | 9},
};
bool parse_u32(std::string_view s, uint32_t& out) {
  if (s.empty() || s.size() > 10) return false;
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const auto digit = uint32_t(c - '0');
    if (v > (std::numeric_limits<uint32_t>::max() - digit) / 10) return false;
    v = v * 10 + digit;
  }
  out = v; return true;
}
}  // namespace

// 名字可给符号名（见 kKeyNames），也可直接给十进制标识号（此时按 Media 类别）。
uint32_t CatPlayMediaClient::hard_key_code(std::string_view name) {
  for (const auto& k : kKeyNames) if (k.name == name) return k.code;
  uint32_t raw{};
  if (parse_u32(name, raw) && raw <= 6) return kKindMedia | raw;
  return 0;
}

CatPlayMediaClient::CatPlayMediaClient(SessionCore& core,RealMediaStore& store,std::string path):core_(core),store_(store),path_(std::move(path)) { if(path_.empty()) { const char* env=std::getenv("CP_CATPLAY_MEDIA_SOCKET"); path_=env&&*env?env:"/run/zero2w/catplay-media-v1.sock"; } next_connect_=std::chrono::steady_clock::now(); }
CatPlayMediaClient::~CatPlayMediaClient(){stop();}
// 只换掉观察者：不调用任何回调、不清空/重置当前观察者、不动会话与镜像状态。
// 正在进行的回调由它自己那份 shared_ptr 保活，所以换/清观察者不会造成 use-after-free。
void CatPlayMediaClient::set_record_observer(std::shared_ptr<CatPlayRecordObserver> observer){record_observer_.store(std::move(observer));}
void CatPlayMediaClient::start(){bool expected=false;if(running_.compare_exchange_strong(expected,true)){core_.register_control_sink(kSourceId,[this](const ControlEvent&e){queue_control(e);});thread_=std::thread(&CatPlayMediaClient::run,this);}}
void CatPlayMediaClient::stop(){core_.unregister_control_sink(kSourceId);bool expected=true;if(running_.compare_exchange_strong(expected,false)&&thread_.joinable())thread_.join();close();}
void CatPlayMediaClient::run(){while(running_){tick();std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
// ── 会话作用域 ────────────────────────────────────────────────────────────────
bool CatPlayMediaClient::session_live_in_store(uint64_t id) const {
  if(!id)return false;
  // 锁序：store_ 锁 →（无）Core 锁；本函数不持任何锁调用，与 ingest() 也不交叉。
  const auto s=store_.snapshot();
  return s.session==RealMediaSnapshot::Session::Active&&s.session_id==id;
}
void CatPlayMediaClient::accept_session(uint64_t id){
  if(!id||id==session_id_)return;   // 同一会话重复 begin 是幂等快照，不清已发布的状态
  session_id_=id;reset_mirrors();
}
void CatPlayMediaClient::clear_session(){session_id_=0;reset_mirrors();}
// 清空全部本地镜像与半份分片：新会话/会话结束/链路失败都必须从这里走一遍，
// 否则旧会话的元数据、封面分片或音频镜像会漏到新会话里（跨会话混片）。
void CatPlayMediaClient::reset_mirrors(){
  media_=MediaInfo{};display_=DisplayConfig{};input_=InteractionState{};audio_=AudioState{};
  vehicle_=VehicleState{};tele_=TelephonyState{};link_=LinkState{};nav_=NavigationState{};
  artwork_used_=artwork_seen_=0;artwork_total_=0;artwork_rev_=0;artwork_bytes_=0;
  lyrics_used_=lyrics_seen_=0;lyrics_total_=0;lyrics_rev_=0;lyrics_bytes_=0;
  audio_stream_count_=0;audio_streams_.fill(0);
}
bool CatPlayMediaClient::track_audio_stream(uint32_t id){
  for(std::size_t i=0;i<audio_stream_count_;++i)if(audio_streams_[i]==id)return true;   // 重复快照：不再计数
  if(audio_stream_count_==audio_streams_.size())return false;                            // 有界：表满就不登记
  audio_streams_[audio_stream_count_++]=id;return true;
}
bool CatPlayMediaClient::untrack_audio_stream(uint32_t id){
  for(std::size_t i=0;i<audio_stream_count_;++i)if(audio_streams_[i]==id){audio_streams_[i]=audio_streams_[--audio_stream_count_];return true;}
  return false;
}
void CatPlayMediaClient::skip_record(uint16_t type,std::size_t bytes,const char* why){
  ++record_skipped_;
  if(record_skipped_<=5){std::fprintf(stderr,"[media] skipped record type=0x%04X bytes=%zu (%s)\n",unsigned(type),bytes,why?why:"");std::fflush(stderr);}
}
// ── 活跃源发布闸门 ────────────────────────────────────────────────────────────
// 未选中的 catplay-real 仍在后台收记录，但绝不能把别的输入已发布的状态覆盖掉，
// 所以每个 Core 状态写入都先过这一道。本地镜像（media_/display_/…）照常更新，
// 重新被选中后的下一次记录会自然把最新值带上去（不做跨输入回放）。
bool CatPlayMediaClient::publishable() const { const auto s=core_.snapshot(); return std::string_view(s.active.data())==kSourceId; }
void CatPlayMediaClient::publish_media(){if(publishable())core_.set_media_info(media_);}
void CatPlayMediaClient::publish_display(){if(publishable())core_.set_display_config(display_);}
void CatPlayMediaClient::publish_input(){if(publishable())core_.set_input_state(input_);}
void CatPlayMediaClient::publish_audio(){if(publishable())core_.set_audio_state(audio_);}
void CatPlayMediaClient::publish_vehicle(){if(publishable())core_.set_vehicle_state(vehicle_);}
void CatPlayMediaClient::publish_navigation(){if(publishable())core_.set_navigation_state(nav_);}
void CatPlayMediaClient::publish_telephony(){if(publishable())core_.set_telephony_state(tele_);}
void CatPlayMediaClient::publish_link(){if(publishable())core_.set_link_state(link_);}
void CatPlayMediaClient::publish_artwork(uint32_t revision){
  if(!publishable())return;
  core_.set_artwork(revision,artwork_.data(),artwork_used_);core_.set_media_info(media_);
}
void CatPlayMediaClient::publish_lyrics(uint32_t revision){
  if(!publishable())return;
  core_.set_lyrics(revision,std::string_view(lyrics_.data(),lyrics_used_));core_.set_media_info(media_);
}
void CatPlayMediaClient::fail(bool protocol_error) { if(fd_>=0) { ::close(fd_);fd_=-1; } connected_=false; connecting_=hello_=false;parser_.reset();request_offset_=request_size_=0;{std::lock_guard lock(control_mutex_);control_begin_=control_count_=0;}
  // 链路断了：本地镜像与半份封面/歌词全部作废，会话作用域也一并失效
  // （重连后必须由下一次被接受的 SessionBegin 重新打开，陈旧分片不得再落地）。
  clear_session();
  if(source_active_){core_.upsert_source(kSourceId,InputSourceKind::External,false);source_active_=false;}store_.transport_closed(protocol_error);next_connect_=std::chrono::steady_clock::now()+kRetry;
  std::fprintf(stderr,"[media] transport closed (protocol_error=%d, records_dropped=%llu, skipped=%llu)\n",protocol_error?1:0,static_cast<unsigned long long>(record_dropped_),static_cast<unsigned long long>(record_skipped_));
  std::fflush(stderr);
  // 断开通知：每次 fail 调用恰好一次。放在状态清理之后且同步完成，因此重连后到达的
  // 新记录一定晚于这次通知；观察者侧的重置必须幂等（本函数可能被重复调用）。
  if(const auto observer=record_observer_.load();observer)observer->on_catplay_disconnect(); }
void CatPlayMediaClient::close(){fail(false);}
void CatPlayMediaClient::connect_if_due() {
#if defined(__unix__)
  const auto now=std::chrono::steady_clock::now(); if(fd_>=0 || now<next_connect_ || path_.size()>=sizeof(sockaddr_un::sun_path)) return;
  int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0){next_connect_=now+kRetry;return;} sockaddr_un a{};a.sun_family=AF_UNIX;std::memcpy(a.sun_path,path_.c_str(),path_.size()+1);int r=::connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a));if(r && errno!=EINPROGRESS){::close(fd);next_connect_=now+kRetry;return;}fd_=fd;connecting_=r!=0;connected_=true;hello_=false;parser_.reset();request_offset_=request_size_=0;connected_at_=last_record_=now;store_.transport_connected();
#else
  next_connect_=now+kRetry;
#endif
}
bool CatPlayMediaClient::on_record(void* context,const catplay_media::Header& h,std::span<const uint8_t> payload){return static_cast<CatPlayMediaClient*>(context)->handle(h,payload);}

bool CatPlayMediaClient::handle(const catplay_media::Header& h,std::span<const uint8_t> payload) {
  if(h.type==catplay_media::Type::ServerHello) { if(hello_)return false;hello_=true; }
  else if(!hello_) return false;
  // 活性以“收到了记录”为准，而不是“记录被媒体面接受”。
  // 不支持/暂无法转发的记录（例如放音乐时的编码音频）不能拆整个会话，
  // 否则一放音乐 CarPlay 就断：分帧错误才断，语义上不接受只丢并计数。
  last_record_=std::chrono::steady_clock::now();
  // 原始记录观察者（可选）：在【媒体面裁决之前】、且仅在记录通过协议校验时通知一次。
  // 位置很关键——这样即使 store 拒收（例如放音乐时的编码音频），Core/Forward 也能拿到
  // 原始记录；会话/流过滤不在本层做，由观察者自己决定。hello 与会话起止同样会到达。
  // payload 只是回调期间借出的视图（本函数不保存）；observer 本地持有整个回调期间，
  // 即使回调内部把观察者换掉/清掉，也不会在回调进行中析构。
  if(ext::valid_declared_ext(h)&&ext::valid_static_ext(h,payload)){
    const auto observer=record_observer_.load();
    if(observer)observer->on_catplay_record(h,payload);
  }
  const uint16_t type=static_cast<uint16_t>(h.type);
  if(catplay_media::known_type(h.type)) {
    // 媒体面是唯一裁判：只有它接受的记录才允许继续改状态面。被拒的记录
    // （握手前 / 不属于本会话 / 载荷不合协议）只计数并丢弃：绝不更新 Core，
    // 也不动本地音频镜像，否则 Core 会被没落地过的字节牵着走。
    const bool accepted=store_.ingest(h,payload);
    // SessionBegin 对【同一活跃会话的重复 begin】是幂等拒绝（store 里那条
    // if(session==Active&&h.session_id==snapshot_.session_id)return false），
    // 那不是一条被丢的记录，而是同一会话的快照，所以以 store 的实际会话状态为准确认。
    const bool live_begin=h.type==catplay_media::Type::SessionBegin&&session_live_in_store(h.session_id);
    if(!accepted&&!live_begin){
      ++record_dropped_;
      if(record_dropped_<=5){
        std::fprintf(stderr,"[media] dropped record type=0x%04X bytes=%zu flags=0x%04X p0=%u p1=%u p2=%u p3=%u\n",
                     unsigned(h.type),payload.size(),unsigned(h.flags),h.p0,h.p1,h.p2,h.p3);
        std::fflush(stderr);
      }
      return true;
    }
    if(h.type==catplay_media::Type::SessionBegin){
      // 新会话：先作废旧镜像（含半份封面/歌词），再把源标为在线。
      accept_session(h.session_id);
      core_.upsert_source(kSourceId,InputSourceKind::External,true);source_active_=true;
    }
    if(h.type==catplay_media::Type::SessionEnd){
      // 会话结束：本地镜像作废，源离线（若本会话刚才是活跃源）。
      if(source_active_){core_.upsert_source(kSourceId,InputSourceKind::External,false);source_active_=false;}
      clear_session();
    }
    // 音频流的路由/角色/格式信息同步进 Core 状态面（媒体面只负责字节，语义在这里）。
    if(h.type==catplay_media::Type::AudioStart){
      // 格式只在描述符【声明过】时才改：声明了就按 codec/role 走，旧生产者（描述符 [4..15]
      // 全零）保持 PCM16 + role_from_audio_type 的原行为；声道/采样率一律如实保留。
      const auto fmt=catplay_media::decode_audio_format(payload.data(),payload.size(),h.p2);
      if(track_audio_stream(h.stream_id)){
        audio_.active_role=fmt.declared?role_from_declared_role(fmt.role):AudioRole::Unknown;
        if(audio_.active_role==AudioRole::Unknown)audio_.active_role=role_from_audio_type(h.p0);
        audio_.sample_rate=h.p1;audio_.channels=uint8_t(h.p2);
        audio_.codec=fmt.declared?codec_from_audio_format(fmt.codec):AudioCodec::Pcm16;
        // 计数由 id 表长度推出：同一条流的重复快照不再累加。
        audio_.simultaneous_streams=uint8_t(audio_stream_count_);
        publish_audio();
      } else skip_record(type,payload.size(),"audio stream table full");
    } else if(h.type==catplay_media::Type::AudioEnd){
      // 只摘掉匹配的那条流；结束一条没登记过的流不改任何镜像
      //（旧实现无条件 --simultaneous_streams，0 时会下溢到 255）。
      if(untrack_audio_stream(h.stream_id)){audio_.simultaneous_streams=uint8_t(audio_stream_count_);publish_audio();}
    } else if(h.type==catplay_media::Type::AudioGain){
      // AudioGain 是绝对增益：作用在当前活跃角色上（导航压低另见 ext Ducking）。
      audio_.duck_transition_ms=h.p0;
      if(audio_.active_role==AudioRole::Navigation||audio_.active_role==AudioRole::Voice)audio_.nav_volume_ppm=int32_t(h.p1);
      else audio_.media_volume_ppm=int32_t(h.p1);
      publish_audio();
    }
    return true;
  }
  return handle_ext(type,h,payload);
}

bool CatPlayMediaClient::handle_ext(uint16_t type,const catplay_media::Header& h,std::span<const uint8_t> payload) {
  const auto t=static_cast<DType>(type);
  if(!ext::is_down(t)) {
    // 不认识的类型（含引擎回显的上行类型）：有 payload_bytes 就能整条跳过。
    // 照 CarLife 侧的做法：不理解即丢并计数，绝不因此拆掉会话。
    skip_record(type,payload.size(),"unknown type");
    return true;
  }
  // 已知扩展类型只对【当前活跃会话】生效：没有会话、或带着陈旧会话 id 的记录一律
  // 跳过，不动任何镜像也不落 Core —— 否则上一会话的封面/歌词分片会被拼进新会话。
  if(!session_live(h.session_id)){skip_record(type,payload.size(),"stale/absent session");return true;}
  switch(t) {
    case DType::Metadata: {
      media_.valid=(h.p0&1)!=0; media_.playing=uint8_t((h.p0>>1)&1);
      media_.duration_ms=h.p1; media_.position_ms=h.p2;
      media_.track_number=h.p3>>16; media_.track_count=h.p3&0xFFFF;
      FieldReader r(payload);
      copy_utf8(media_.title,r.next()); copy_utf8(media_.artist,r.next());
      copy_utf8(media_.album,r.next()); copy_utf8(media_.album_artist,r.next());
      copy_utf8(media_.app,r.next());   copy_utf8(media_.genre,r.next());
      publish_media();
      return true;
    }
    case DType::ArtworkChunk: {
      if(h.p0==0){artwork_rev_=h.p3;artwork_used_=0;artwork_seen_=0;artwork_total_=uint16_t(h.p1);artwork_bytes_=h.p2;}
      if(h.p3==artwork_rev_ && h.p0==artwork_seen_ && h.p1==artwork_total_ && h.p2==artwork_bytes_ &&
         artwork_total_ && artwork_used_+payload.size()<=artwork_.size() && artwork_used_+payload.size()<=artwork_bytes_){
        std::memcpy(artwork_.data()+artwork_used_,payload.data(),payload.size());artwork_used_+=payload.size();
        if(h.p0+1==h.p1&&artwork_used_==h.p2){
          media_.has_artwork=true; media_.artwork_revision=h.p3; media_.artwork_bytes=uint32_t(artwork_used_);
          publish_artwork(h.p3);
          artwork_seen_=artwork_total_;
        } else if(h.p0+1==h.p1) { artwork_used_=0; artwork_seen_=0; }   // 长度对不上：整轮作废，绝不截断出一张坏图
        else ++artwork_seen_;
      } else { artwork_used_=0; artwork_seen_=0; artwork_total_=0; ++record_skipped_; }
      return true;
    }
    case DType::LyricsChunk: {
      if(h.p0==0){lyrics_rev_=h.p3;lyrics_used_=0;lyrics_seen_=0;lyrics_total_=uint16_t(h.p1);lyrics_bytes_=h.p2;}
      if(h.p3==lyrics_rev_ && h.p0==lyrics_seen_ && h.p1==lyrics_total_ && h.p2==lyrics_bytes_ &&
         lyrics_total_ && lyrics_used_+payload.size()<=lyrics_.size() && lyrics_used_+payload.size()<=lyrics_bytes_){
        std::memcpy(lyrics_.data()+lyrics_used_,payload.data(),payload.size());lyrics_used_+=payload.size();
        if(h.p0+1==h.p1 && lyrics_used_==lyrics_bytes_){
          media_.has_lyrics=true; media_.lyrics_revision=h.p3; media_.lyrics_bytes=uint32_t(lyrics_used_);
          publish_lyrics(h.p3);
          lyrics_seen_=lyrics_total_;
        } else if(h.p0+1==h.p1) { lyrics_used_=0;lyrics_seen_=0;lyrics_total_=0;++record_skipped_; }
        else ++lyrics_seen_;
      } else { lyrics_used_=0; lyrics_seen_=0; lyrics_total_=0; ++record_skipped_; }
      return true;
    }
    case DType::MediaLibrary: {
      switch(h.p0) {
        case 0: media_.playing=1; break;                     // resume
        case 1: media_.playing=0; break;                     // pause
        case 2: media_.position_ms=h.p1; break;              // seek
        case 3: case 4: media_.position_ms=0; break;         // next / prev
        case 5: media_.media_library_revision=h.p2; break;   // 媒体库内容版本
        default: break;
      }
      publish_media();
      return true;
    }
    case DType::Navigation: {
      // 载荷是定长布局：四种文本字段定长 NUL 填充，后面跟二进制字段，
      // 所以不用 FieldReader（它靠 '\0' 找边界，后面有二进制会歧义）。
      if(payload.size()!=ext::kNavPayloadBytes){++record_skipped_;return true;}
      nav_.valid=true;
      nav_.active=uint8_t(h.p3&1);
      // maneuver_code 永远**原值透传**（合同明确要求：绝不编 CarLife/Android Auto 映射表）。
      // maneuver 只在 p0 落在本协议自身枚举的序号范围内时顺序透传，否则留 None ——
      // 它是“本 CPMF 消息自定义的编号”，不是跨生态对照表；越界值宁可空着也不猜。
      nav_.maneuver_code=h.p0;
      nav_.maneuver=(h.p0<=uint32_t(Maneuver::Destination))?Maneuver(h.p0):Maneuver::None;
      copy_utf8(nav_.road_name,ext::nav_text(payload,ext::kNavRoadOffset,ext::kNavRoadBytes));
      copy_utf8(nav_.next_road_name,ext::nav_text(payload,ext::kNavNextRoadOffset,ext::kNavNextRoadBytes));
      copy_utf8(nav_.icon,ext::nav_text(payload,ext::kNavIconOffset,ext::kNavIconBytes));
      copy_utf8(nav_.destination,ext::nav_text(payload,ext::kNavDestOffset,ext::kNavDestBytes));
      nav_.distance_to_maneuver_m=h.p1; nav_.time_remaining_s=h.p2;
      nav_.distance_remaining_m=be32(payload.data()+ext::kNavDistanceRemainingOffset);
      nav_.eta_epoch_s=be64(payload.data()+ext::kNavEtaOffset);
      nav_.lane_bitmap=be16(payload.data()+ext::kNavLaneOffset);
      nav_.destination_reached=payload[ext::kNavDestReachedOffset];
      publish_navigation();
      return true;
    }
    case DType::Vehicle: {
      if(payload.size()!=ext::kVehiclePayloadBytes) { ++record_skipped_; return true; }
      const uint8_t* b=payload.data();
      vehicle_.speed_kph=int32_t(be32(b+0)); vehicle_.rpm=int32_t(be32(b+4));
      vehicle_.fuel_pct=int32_t(be32(b+8)); vehicle_.range_km=int32_t(be32(b+12));
      vehicle_.outside_temp_c=int32_t(be32(b+16)); vehicle_.odometer_km=be32(b+20);
      vehicle_.latitude=ext::get_f64(b+24); vehicle_.longitude=ext::get_f64(b+32);
      vehicle_.heading_deg=ext::get_f32(b+40); vehicle_.doors=be16(b+44);
      vehicle_.gear=b[46]; vehicle_.night_mode=b[47]; vehicle_.lights=b[48];
      vehicle_.parking_brake=b[49]; vehicle_.valid=(b[50]&1)!=0;
      copy_utf8(vehicle_.vin,as_view(reinterpret_cast<const char*>(b+ext::kVehicleVinOffset),ext::kVehicleVinBytes));
      display_.day_night=vehicle_.night_mode?1:display_.day_night;   // 车辆昼夜会顺带更新显示
      publish_vehicle(); publish_display();
      return true;
    }
    case DType::Telephony: {
      FieldReader r(payload);
      tele_.call_state=uint8_t(h.p0); tele_.call_duration_s=h.p1;
      tele_.signal_bars=uint8_t((h.p2>>8)&0xFF); tele_.battery_pct=uint8_t(h.p2&0xFF);
      copy_utf8(tele_.caller,r.next()); copy_utf8(tele_.caller_number,r.next());
      tele_.dtmf_supported=(h.p3&1)?1:0;
      publish_telephony();
      return true;
    }
    case DType::ContactsChunk: {
      tele_.contact_count=uint16_t(std::min<uint32_t>(h.p2,65535));
      tele_.contacts_ready=(h.p0+1==h.p1)?1:0;
      publish_telephony();
      return true;
    }
    case DType::CallLogChunk: {
      tele_.call_log_count=uint16_t(std::min<uint32_t>(h.p2,65535));
      tele_.call_log_ready=(h.p0+1==h.p1)?1:0;
      publish_telephony();
      return true;
    }
    case DType::Ducking: {
      // 语义化闪避：p1 = 压低后的媒体音量（百万分比，满量程 1000000），p2 = 导航音量。
      // duck_ratio_ppm 就取压低后的媒体量，因为满量程 1000000 时它等价于比例。
      audio_.nav_active=uint8_t(h.p0&1);
      audio_.media_volume_ppm=int32_t(h.p1); audio_.nav_volume_ppm=int32_t(h.p2);
      audio_.duck_transition_ms=h.p3;
      if(h.p1) audio_.duck_ratio_ppm=int32_t(h.p1);
      publish_audio();
      return true;
    }
    case DType::SafeArea: {
      display_.safe_top=uint16_t(h.p0>>16); display_.safe_bottom=uint16_t(h.p0&0xFFFF);
      display_.safe_left=uint16_t(h.p1>>16); display_.safe_right=uint16_t(h.p1&0xFFFF);
      display_.width=uint16_t(h.p2>>16); display_.height=uint16_t(h.p2&0xFFFF);
      publish_display();
      return true;
    }
    case DType::AuxPlane: {
      display_.aux_enabled=uint8_t(h.p0&1);
      display_.aux_x=uint16_t(h.p1>>16); display_.aux_y=uint16_t(h.p1&0xFFFF);
      display_.aux_w=uint16_t(h.p2>>16); display_.aux_h=uint16_t(h.p2&0xFFFF);
      display_.primary_plane=uint8_t(h.p3); display_.aux_enabled=display_.aux_enabled||(h.p3>0);
      publish_display();
      return true;
    }
    case DType::Calibration: {
      display_.gamma_pct=uint8_t(h.p0>>16); display_.contrast_pct=uint8_t(h.p0&0xFFFF);
      display_.saturation_pct=uint8_t(h.p1&0xFF);
      publish_display();
      return true;
    }
    case DType::DayNight: {
      display_.day_night=uint8_t(h.p0&1);
      publish_display();
      return true;
    }
    case DType::FrameRate: {
      display_.target_fps=uint16_t(h.p0); display_.actual_fps=uint16_t(h.p1);
      publish_display();
      return true;
    }
    case DType::FileTransfer: {
      link_.file_transfer_active=uint8_t(h.p0&1);
      link_.file_transfer_bytes=h.p1; link_.file_transfer_total=h.p2;
      publish_link();
      return true;
    }
    case DType::Ota: {
      link_.ota_state=uint8_t(h.p0);
      publish_link();
      return true;
    }
    case DType::Activation: {
      link_.activation_state=uint8_t(h.p0);
      publish_link();
      return true;
    }
    case DType::ContentEncryption: {
      link_.content_encryption=uint8_t(h.p0);
      publish_link();
      return true;
    }
    case DType::MultiSession: {
      FieldReader r(payload);
      link_.session_count=0; uint8_t idx=0;
      std::array<char,32> active{};
      // 会话表是状态面的一部分（允许会话已到但 panel 还没切过来），所以同样过发布闸门。
      const bool pub=publishable();
      for(std::string_view id=r.next();!id.empty();id=r.next()){
        if(link_.session_count<kMaxSessionList){
          copy_utf8(link_.sessions[link_.session_count],id);
          link_.session_active[link_.session_count]=(idx==uint8_t(h.p1))?1:0;
          if(idx==uint8_t(h.p1)) active=link_.sessions[link_.session_count];
          ++link_.session_count;
        }
        if(pub)core_.upsert_session(id,idx==uint8_t(h.p1));
        ++idx;
      }
      if(active[0]){link_.active_session=active; if(pub)core_.set_active_session(as_view(active.data(),active.size()));}
      if(pub)core_.set_link_state(link_);
      return true;
    }
    case DType::AssistiveTouch:  { input_.assistive_touch=uint8_t(h.p0&1); publish_input(); return true; }
    case DType::VoiceOverState:  { input_.voiceover=uint8_t(h.p0&1);       publish_input(); return true; }
    case DType::HidModeState:    { input_.hid_mode=uint8_t(h.p0);         publish_input(); return true; }
    case DType::ProximityState:  { input_.proximity=uint8_t(h.p0&1);      publish_input(); return true; }
    case DType::Capability: {
      input_.multi_touch_points=uint8_t(h.p0);
      input_.touchpad=uint8_t((h.p1>>8)&0xFF); input_.knob=uint8_t(h.p1&0xFF);
      publish_input();
      return true;
    }
    default: break;
  }
  skip_record(type,payload.size(),"unhandled ext type");
  return true;
}

// 控制面不再丢弃非触控事件：硬键/多点/旋钮/手势/接近/语音/电话/车控全部入队下发。
// 只有 Touch 的 Move 做合并（高频），其余事件即使队列满也要挤掉最旧的送出去。
void CatPlayMediaClient::queue_control(const ControlEvent&e){
  std::lock_guard lock(control_mutex_);
  const bool move=(e.type==ControlEvent::Type::Touch&&e.phase==ControlEvent::TouchPhase::Move);
  if(control_count_&&move){auto&last=controls_[(control_begin_+control_count_-1)%kControlCapacity];if(last.type==ControlEvent::Type::Touch&&last.phase==ControlEvent::TouchPhase::Move){last=e;return;}}
  if(control_count_==kControlCapacity){
    if(move)return;                                    // 高频 Move 可以丢
    control_begin_=(control_begin_+1)%kControlCapacity;--control_count_;   // 其余丢最旧也必须下发
  }
  controls_[(control_begin_+control_count_)%kControlCapacity]=e;++control_count_;
}
bool CatPlayMediaClient::pop_control(ControlEvent&e){std::lock_guard lock(control_mutex_);if(!control_count_)return false;e=controls_[control_begin_];control_begin_=(control_begin_+1)%kControlCapacity;--control_count_;return true;}

void CatPlayMediaClient::flush_request() {
#if defined(__unix__)
  if(fd_<0 || connecting_)return;
  if(!request_size_){
    ControlEvent e;
    while(pop_control(e)){
      // 触控仍走既有的 Touch（带 session/sequence，引擎侧已实现）；
      // 所有发送到引擎的控制必须带当前 session/sequence，防止跨会话重放。
      if(!session_id_ || !session_live_in_store(session_id_) || !publishable())continue;
      if(e.type==ControlEvent::Type::Touch){
        const auto session=store_.snapshot().session_id;
        if(!session||e.x<0||e.y<0)continue;
        const auto rec=catplay_media::touch_request(session,++control_sequence_,uint32_t(e.phase),uint32_t(e.x),uint32_t(e.y));
        std::memcpy(request_.data(),rec.data(),rec.size());
        request_size_=rec.size();request_offset_=0;break;
      }
      ext::OutRecord rec{};
      switch(e.type){
        case ControlEvent::Type::Key: {
          const uint32_t code=hard_key_code(as_view(e.key.data(),e.key.size()));
          if(!code)continue;   // 名字不认识就不发，避免下发一个语义为零的键
          rec=ext::encode_up(UType::Key,code,0,0,0,as_view(e.key.data(),e.key.size()));
          // 回传确认用：把"最近下发的键"写进状态。同样过发布闸门——本输入不是活跃源时
          // 就不应该往状态面写任何东西。
          if(publishable())core_.note_hard_key(code);
          break;
        }
        case ControlEvent::Type::MultiTouch: {
          std::array<ext::TouchPoint,mvp::kMaxMultiTouch> pts{};
          const uint8_t n=(e.point_count<=mvp::kMaxMultiTouch)?e.point_count:uint8_t(mvp::kMaxMultiTouch);
          for(uint8_t i=0;i<n;++i)pts[i]={e.points[i].x,e.points[i].y,e.points[i].id,e.points[i].phase};
          if(!n)continue;
          rec=ext::encode_multitouch({pts.data(),n});
          break;
        }
        case ControlEvent::Type::Knob:
          rec=ext::encode_up(UType::Knob,uint32_t(e.knob_dir),uint32_t(int32_t(e.knob_steps)),0,0);
          break;
        case ControlEvent::Type::Gesture:
          if(!e.gesture[0])continue;
          rec=ext::encode_up(UType::Gesture,0,0,0,0,as_view(e.gesture.data(),e.gesture.size()));
          break;
        case ControlEvent::Type::Proximity:
          rec=ext::encode_up(UType::ProximityEvent,e.x?1u:0u,0,0,0);
          break;
        case ControlEvent::Type::Voice:
          rec=ext::encode_up(UType::Voice,uint32_t(e.x),0,0,0);
          break;
        case ControlEvent::Type::Telephony: {
          const std::string_view dtmf=as_view(e.dtmf.data(),e.dtmf.size());
          rec=ext::encode_up(UType::TelephonyCtrl,uint32_t(e.x),0,0,0,dtmf);
          break;
        }
        case ControlEvent::Type::VehicleCtrl:
          if(!e.ctrl[0])continue;
          rec=ext::encode_up(UType::VehicleCtrl,uint32_t(e.x),0,0,0,as_view(e.ctrl.data(),e.ctrl.size()));
          break;
        default: continue;
      }
      if(!rec.size)continue;
      put_be64(rec.bytes.data()+16,session_id_);
      put_be64(rec.bytes.data()+24,++control_sequence_);
      std::memcpy(request_.data(),rec.bytes.data(),rec.size);
      request_size_=rec.size;request_offset_=0;break;
    }
    if(!request_size_){uint64_t session{};uint32_t stream{};if(store_.take_keyframe_request(session,stream)){const auto rec=catplay_media::keyframe_request(session,stream);std::memcpy(request_.data(),rec.data(),rec.size());request_size_=rec.size();request_offset_=0;}}
  }
  while(request_offset_<request_size_){auto n=send(fd_,request_.data()+request_offset_,request_size_-request_offset_,MSG_NOSIGNAL);if(n>0){request_offset_+=std::size_t(n);continue;}if(n<0&&errno==EINTR)continue;if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return;fail(false);return;}request_size_=request_offset_=0;
#endif
}
void CatPlayMediaClient::tick() {
#if defined(__unix__)
  connect_if_due();if(fd_<0)return;const auto now=std::chrono::steady_clock::now();if(connecting_){int error{};socklen_t n=sizeof(error);if(getsockopt(fd_,SOL_SOCKET,SO_ERROR,&error,&n)||error){fail(false);return;}connecting_=false;}
  if(!hello_ && now-connected_at_>kHelloDeadline){fail(true);return;}if(hello_ && now-last_record_>kLivenessDeadline){fail(false);return;}
  const auto drain_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(5);for(int i=0;i<128&&std::chrono::steady_clock::now()<drain_deadline;++i){auto n=recv(fd_,read_.data(),read_.size(),0);if(n>0){auto result=parser_.push(read_.data(),std::size_t(n),&CatPlayMediaClient::on_record,this);if(result==catplay_media::ParseResult::Invalid||result==catplay_media::ParseResult::HandlerRejected){fail(true);return;}continue;}if(n==0){fail(false);return;}if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)break;fail(false);return;}flush_request();
#else
  connect_if_due();
#endif
}
} // namespace mvp
