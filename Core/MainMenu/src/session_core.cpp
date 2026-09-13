#include "core/session_core.hpp"
#include <algorithm>
#include <cstring>
namespace mvp { namespace {
std::string_view text_of(const std::array<char,32>& a){std::size_t n=0;while(n<a.size()&&a[n])++n;return {a.data(),n};}
int priority(InputSourceKind k){return k==InputSourceKind::External?3:k==InputSourceKind::Synthetic?2:1;}
void copy_event(std::array<char,kMaxText>& dst,std::string_view src){dst.fill(0);auto n=std::min(dst.size()-1,src.size());std::memcpy(dst.data(),src.data(),n);}
// UTF-8 边界安全长度：若被截断处正好落在多字节序列中间（下一个字节是 10xxxxxx），
// 就往前退到该序列的起点。按字节硬截会劈开汉字，进而让 /api/state 整份 JSON 非法
// （这是踩过的真实 bug，不是理论风险）。
std::size_t utf8_len(std::string_view src,std::size_t cap){
  if(src.size()<=cap)return src.size();
  std::size_t n=cap;
  while(n>0&&(static_cast<unsigned char>(src[n])&0xC0u)==0x80u)--n;
  return n;
}
// 定长文本写入（本 TU 内部复用；调用方不必预清）。
template<std::size_t N>
void put_text(std::array<char,N>& dst,std::string_view src){
  dst.fill(0);
  const std::size_t n=utf8_len(src,dst.size()-1);
  if(n)std::memcpy(dst.data(),src.data(),n);
}
// Compare fields, not indeterminate structure padding (which differs in Debug builds).
template<typename T> bool same(const T& a,const T& b){return a==b;}
// 控制事件类型的可读名，只用于会话日志（页面/审计靠它区分"硬键"与"多点"）。
std::string_view control_name(ControlEvent::Type t){
  switch(t){
    case ControlEvent::Type::Touch:return "touch";
    case ControlEvent::Type::Key:return "key";
    case ControlEvent::Type::MultiTouch:return "multitouch";
    case ControlEvent::Type::Knob:return "knob";
    case ControlEvent::Type::Gesture:return "gesture";
    case ControlEvent::Type::Proximity:return "proximity";
    case ControlEvent::Type::Voice:return "voice";
    case ControlEvent::Type::Telephony:return "telephony";
    case ControlEvent::Type::VehicleCtrl:return "vehicle";
  }
  return "unknown";       // 未定义取值也照常转发，只是名字不认得
}
}
void SessionCore::copy_text(std::array<char,32>& dst,std::string_view src){put_text(dst,src);}
void SessionCore::copy_text(std::array<char,kMaxTextLong>& dst,std::string_view src){put_text(dst,src);}
void SessionCore::log_locked(SessionEvent::Type type,std::string_view detail,uint64_t seq){if(control_count_==controls_.size()){std::move(controls_.begin()+1,controls_.end(),controls_.begin());--control_count_;++control_dropped_;}auto&e=controls_[control_count_++];e.type=type;e.sequence=seq?seq:++event_sequence_;copy_event(e.detail,detail);}
void SessionCore::resolve_active_locked(){
  auto old=active_;active_.fill(0);int best=-1;bool preferred=false;
  if(mode_==SelectionMode::Manual)
    for(std::size_t i=0;i<source_count_;++i)
      if(sources_[i].connected&&text_of(sources_[i].id)==text_of(selected_)){active_=sources_[i].id;preferred=true;break;}
  // PROJECT_ARCHITECTURE §2 第 4 条：多个手机输入同时在线时，输出 Core 桌面并要求手动选择。
  // 桌面源只有在自己在线时才能充当这个角色；没有桌面源时退回到按优先级挑选。
  if(mode_==SelectionMode::Automatic&&!preferred&&!text_of(desktop_).empty()){
    int external=0;
    for(std::size_t i=0;i<source_count_;++i)
      if(sources_[i].connected&&sources_[i].kind==InputSourceKind::External)++external;
    if(external>=2)
      for(std::size_t i=0;i<source_count_;++i)
        if(sources_[i].connected&&text_of(sources_[i].id)==text_of(desktop_)){active_=sources_[i].id;preferred=true;break;}
  }
  if(!preferred)
    for(std::size_t i=0;i<source_count_;++i){
      if(!sources_[i].connected)continue;
      const int p=priority(sources_[i].kind);
      // 同一优先级内，Core 桌面优先于其它本地桌面源（同为零输入兼底场景）。
      const bool is_desktop=text_of(sources_[i].id)==text_of(desktop_);
      if(p>best||(p==best&&is_desktop)){best=p;active_=sources_[i].id;}
    }
  if(old!=active_){has_video_=false;has_audio_=false;latest_video_={};latest_audio_={};log_locked(SessionEvent::Type::SourceChanged,text_of(active_));}
}
bool SessionCore::set_desktop_source(std::string_view id){if(id.size()>=32)return false;std::lock_guard lock(mutex_);copy_text(desktop_,id);resolve_active_locked();return true;}
bool SessionCore::upsert_source(std::string_view id,InputSourceKind kind,bool connected){if(id.empty()||id.size()>=32)return false;std::lock_guard lock(mutex_);for(std::size_t i=0;i<source_count_;++i)if(text_of(sources_[i].id)==id){sources_[i].kind=kind;sources_[i].connected=connected;resolve_active_locked();return true;}if(source_count_==sources_.size())return false;auto&s=sources_[source_count_++];copy_text(s.id,id);s.kind=kind;s.connected=connected;resolve_active_locked();return true;}
bool SessionCore::set_selection(SelectionMode mode,std::string_view id){if(mode==SelectionMode::Manual&&(id.empty()||id.size()>=32))return false;std::lock_guard lock(mutex_);if(mode==SelectionMode::Manual){bool connected=false;for(std::size_t i=0;i<source_count_;++i)if(text_of(sources_[i].id)==id&&sources_[i].connected){connected=true;break;}if(!connected)return false;}mode_=mode;copy_text(selected_,id);resolve_active_locked();return true;}
bool SessionCore::register_control_sink(std::string_view source,std::function<void(const ControlEvent&)> sink){if(source.empty()||source.size()>=32||!sink)return false;std::lock_guard lock(mutex_);for(auto&slot:sinks_)if(text_of(slot.id)==source){slot.sink=std::move(sink);return true;}for(auto&slot:sinks_)if(text_of(slot.id).empty()){copy_text(slot.id,source);slot.sink=std::move(sink);return true;}return false;}
void SessionCore::unregister_control_sink(std::string_view source){std::lock_guard lock(mutex_);for(auto&slot:sinks_)if(text_of(slot.id)==source){slot.id.fill(0);slot.sink={};return;}}
bool SessionCore::submit_video(std::string_view source,const VideoFrame& f){if(f.size>f.payload.size()){std::lock_guard lock(mutex_);++video_dropped_;return false;}std::lock_guard lock(mutex_);if(source!=text_of(active_)){++video_dropped_;log_locked(SessionEvent::Type::FrameDropped,"video inactive",f.sequence);return false;}latest_video_=f;has_video_=true;return true;}
bool SessionCore::submit_audio(std::string_view source,const AudioChunk& a){if(a.sample_count>a.samples.size())return false;std::lock_guard lock(mutex_);if(source!=text_of(active_)){++audio_dropped_;return false;}latest_audio_=a;has_audio_=true;return true;}
// 控制事件路由。两条硬约束：
//   1) point_count 必须在入口夹到数组容量：生产者可能填成 11/255，若原样透传，
//      下游按 point_count 遍历 points[] 就是越界读（数组只有 kMaxMultiTouch 个）。
//   2) 类型不认识也不能拆会话：只如实转发 + 记日志（汽车协议上"不理解即丢"）。
bool SessionCore::route_control(const ControlEvent& raw){
  ControlEvent e=raw;
  if(e.point_count>kMaxMultiTouch)e.point_count=static_cast<uint8_t>(kMaxMultiTouch);
  std::function<void(const ControlEvent&)> sink;
  {
    std::lock_guard lock(mutex_);
    if(text_of(active_).empty()){++control_dropped_;return false;}
    for(auto&slot:sinks_)if(text_of(slot.id)==text_of(active_)){sink=slot.sink;break;}
    if(!sink){++control_dropped_;return false;}
    log_locked(SessionEvent::Type::Control,control_name(e.type));
  }
  sink(e);
  return true;
}
bool SessionCore::latest_video(VideoFrame&out)const{std::lock_guard lock(mutex_);if(!has_video_)return false;out=latest_video_;return true;}
bool SessionCore::has_video()const{std::lock_guard lock(mutex_);return has_video_;}
VideoEncoding SessionCore::active_video_encoding()const{std::lock_guard lock(mutex_);return latest_video_.encoding;}bool SessionCore::latest_audio(AudioChunk&out)const{std::lock_guard lock(mutex_);if(!has_audio_)return false;out=latest_audio_;return true;}SessionSnapshot SessionCore::snapshot()const{std::lock_guard lock(mutex_);SessionSnapshot s;s.mode=mode_;s.selected=selected_;s.active=active_;s.sources=sources_;s.source_count=source_count_;s.video_dropped=video_dropped_;s.audio_dropped=audio_dropped_;s.control_dropped=control_dropped_;s.controls=controls_;s.control_count=control_count_;
  // 本轮新增字段一并拷出：snapshot() 仍是"一把拿全"的值语义，页面无需多次取值。
  s.media=media_;s.display=display_;s.input=input_;s.audio=audio_;
  s.vehicle=vehicle_;s.telephony=tele;s.link=link_;s.nav=nav_;
  return s;}
// ── 新增写入面 ────────────────────────────────────────────────────────────
// 每个 setter 都返回"是否真的有变化"：调用方可据此跳过无意义的页面重构。
// 按字段值比较，忽略结构体填充字节。
bool SessionCore::set_media_info(const MediaInfo& info){std::lock_guard lock(mutex_);if(same(media_,info))return false;media_=info;return true;}
bool SessionCore::set_display_config(const DisplayConfig& cfg){std::lock_guard lock(mutex_);if(same(display_,cfg))return false;display_=cfg;return true;}
bool SessionCore::set_input_state(const InteractionState& st){std::lock_guard lock(mutex_);if(same(input_,st))return false;input_=st;return true;}
bool SessionCore::set_audio_state(const AudioState& st){std::lock_guard lock(mutex_);if(same(audio_,st))return false;audio_=st;return true;}
bool SessionCore::set_vehicle_state(const VehicleState& st){std::lock_guard lock(mutex_);if(same(vehicle_,st))return false;vehicle_=st;return true;}
bool SessionCore::set_navigation_state(const NavigationState& st){std::lock_guard lock(mutex_);if(same(nav_,st))return false;nav_=st;return true;}
bool SessionCore::set_telephony_state(const TelephonyState& st){std::lock_guard lock(mutex_);if(same(tele,st))return false;tele=st;return true;}
bool SessionCore::set_link_state(const LinkState& st){std::lock_guard lock(mutex_);if(same(link_,st))return false;link_=st;return true;}

// 封面正文不进 snapshot（最大 128 KiB），只在这里存一份；状态里留 revision/字节数。
// 契约：**revision 相同即视为同一张封面**，不做逐字节比对（比对代价远高于收益）。
// 因此调用方的 revision 必须从 1 开始递增，不能用 0 表示"第一张"（0 = 初值）。
bool SessionCore::set_artwork(uint32_t revision,const char* data,std::size_t size){
  std::lock_guard lock(mutex_);
  if(revision==artwork_rev_)return false;
  const bool had=artwork_size_!=0;
  if(!data||!size){
    artwork_size_=0;artwork_rev_=revision;
    media_.has_artwork=false;media_.artwork_bytes=0;media_.artwork_revision=revision;
    return had;
  }
  const std::size_t n=std::min(size,kMaxArtwork);   // 超长即截断，不做压缩/转码
  if(n)std::memcpy(artwork_.data(),data,n);
  artwork_size_=n;artwork_rev_=revision;
  media_.has_artwork=true;media_.artwork_bytes=static_cast<uint32_t>(n);media_.artwork_revision=revision;
  return true;
}
// 歌词同封面：revision 语义，正文存定长缓冲。截断处按 UTF-8 边界回退。
bool SessionCore::set_lyrics(uint32_t revision,std::string_view lrc){
  std::lock_guard lock(mutex_);
  if(revision==lyrics_rev_)return false;
  const bool had=lyrics_size_!=0;
  if(lrc.empty()){
    lyrics_size_=0;lyrics_rev_=revision;
    media_.has_lyrics=false;media_.lyrics_bytes=0;media_.lyrics_revision=revision;
    return had;
  }
  const std::size_t n=utf8_len(lrc,kMaxLyrics-1);
  lyrics_.fill(0);
  if(n)std::memcpy(lyrics_.data(),lrc.data(),n);
  lyrics_size_=n;lyrics_rev_=revision;
  media_.has_lyrics=true;media_.lyrics_bytes=static_cast<uint32_t>(n);media_.lyrics_revision=revision;
  return true;
}
// 封面读取面：返回内部缓冲指针，调用方须在下一次 set_artwork 前用完
// （契约如此，否则每次取值都要拷 128 KiB）。
bool SessionCore::artwork(const char** data,std::size_t* size,uint32_t* revision)const{
  if(!data||!size||!revision)return false;
  std::lock_guard lock(mutex_);
  *revision=artwork_rev_;
  if(!artwork_size_){*data=nullptr;*size=0;return false;}
  *data=artwork_.data();*size=artwork_size_;return true;
}
bool SessionCore::lyrics(std::string_view* out,uint32_t* revision)const{
  if(!out||!revision)return false;
  std::lock_guard lock(mutex_);
  *revision=lyrics_rev_;
  if(!lyrics_size_){*out={};return false;}
  *out=std::string_view(lyrics_.data(),lyrics_size_);return true;
}
// 便捷路径：只改音频闪避。导航播报中把媒体音压到 duck_ratio_ppm（默认 1/3，
// 照抄参考实现 AudioTrackManagerDualNormal 的 maxVolume/VolumReduceRatio 比例语义）。
bool SessionCore::set_ducking(bool nav_active,int32_t media_ppm,int32_t nav_ppm){
  std::lock_guard lock(mutex_);
  const uint8_t nav=nav_active?1:0;
  if(audio_.nav_active==nav&&audio_.media_volume_ppm==media_ppm&&audio_.nav_volume_ppm==nav_ppm)return false;
  audio_.nav_active=nav;audio_.media_volume_ppm=media_ppm;audio_.nav_volume_ppm=nav_ppm;
  audio_.active_role=nav_active?AudioRole::Navigation:AudioRole::Media;
  return true;
}
bool SessionCore::note_hard_key(uint32_t keycode){
  std::lock_guard lock(mutex_);
  if(input_.last_key_code==keycode)return false;
  input_.last_key_code=keycode;return true;
}
// 多设备会话表：同 id 复用槽位并更新活跃位；新 id 追加（表满则拒绝并如实返回 false）。
// 任一时刻至多一个活跃会话（车机侧硬件也只有一个前台会话）。
bool SessionCore::upsert_session(std::string_view id,bool active){
  if(id.empty()||id.size()>=32)return false;
  std::lock_guard lock(mutex_);
  for(std::size_t i=0;i<link_.session_count;++i)
    if(text_of(link_.sessions[i])==id){
      const bool changed=link_.session_active[i]!=(active?1:0);
      if(active){
        for(std::size_t j=0;j<link_.session_count;++j)link_.session_active[j]=(j==i)?1:0;
        copy_text(link_.active_session,id);
      }else{
        link_.session_active[i]=0;
        if(text_of(link_.active_session)==id)link_.active_session.fill(0);
      }
      return changed;
    }
  if(link_.session_count>=kMaxSessionList)return false;
  const std::size_t idx=link_.session_count++;
  copy_text(link_.sessions[idx],id);
  link_.session_active[idx]=active?1:0;
  if(active)copy_text(link_.active_session,id);
  return true;
}
bool SessionCore::set_active_session(std::string_view id){
  std::lock_guard lock(mutex_);
  if(id.empty()){                     // 空 id = 全部置非活跃（手机都断开）
    if(text_of(link_.active_session).empty())return false;
    link_.active_session.fill(0);
    for(std::size_t i=0;i<link_.session_count;++i)link_.session_active[i]=0;
    return true;
  }
  if(id.size()>=32)return false;
  for(std::size_t i=0;i<link_.session_count;++i)
    if(text_of(link_.sessions[i])==id){
      if(text_of(link_.active_session)==id&&link_.session_active[i])return false;
      for(std::size_t j=0;j<link_.session_count;++j)link_.session_active[j]=(j==i)?1:0;
      copy_text(link_.active_session,id);
      return true;
    }
  return false;                   // 不在表里不接受，避免出现"活跃但不存在"的会话
}
} // namespace mvp
