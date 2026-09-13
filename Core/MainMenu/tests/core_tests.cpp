#include "core/session_core.hpp"
#include "wirelesscarplay/real_telemetry.hpp"
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
using namespace mvp;
int checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK(" #x ") failed\n"; return 1; } } while (false)
// 校验一段字节序列是否合法 UTF-8。用于证明定长截断没有劈开多字节字符
// （劈开后 /api/state 整份 JSON 会变成非法，这是踩过的真实 bug）。
static bool valid_utf8(std::string_view s){
  for(std::size_t i=0;i<s.size();){
    const unsigned char c=static_cast<unsigned char>(s[i]);
    std::size_t n=0;
    if(c<0x80u)n=1;
    else if((c&0xE0u)==0xC0u)n=2;
    else if((c&0xF0u)==0xE0u)n=3;
    else if((c&0xF8u)==0xF0u)n=4;
    else return false;
    if(i+n>s.size())return false;
    for(std::size_t k=1;k<n;++k)
      if((static_cast<unsigned char>(s[i+k])&0xC0u)!=0x80u)return false;
    i+=n;
  }
  return true;
}
int main(){int sentinel=0;CHECK(++sentinel==1);CHECK(bounded_json_object("{\"id\":1,\"ok\":true}"));CHECK(!bounded_json_object("[1]"));CHECK(!bounded_json_object("{\"x\":\x01}"));CHECK(!bounded_json_object(std::string(kCpNativeTelemetryReplyCap+1,'x')));SessionCore c;CHECK(c.upsert_source("local",InputSourceKind::LocalDesktop,true));CHECK(c.upsert_source("external",InputSourceKind::External,true));CHECK(std::string(c.snapshot().active.data())=="external");int local=0,external=0;CHECK(c.register_control_sink("local",[&](const ControlEvent&){++local;}));CHECK(c.register_control_sink("external",[&](const ControlEvent&){++external;}));ControlEvent e;CHECK(c.route_control(e)&&external==1&&local==0);VideoFrame f;f.encoding=VideoEncoding::Svg;f.width=480;f.height=270;f.size=4;std::memcpy(f.payload.data(),"svg",4);CHECK(c.submit_video("external",f));CHECK(c.set_selection(SelectionMode::Manual,"external"));CHECK(!c.set_selection(SelectionMode::Manual,"missing"));CHECK(c.upsert_source("external",InputSourceKind::External,false));CHECK(std::string(c.snapshot().active.data())=="local");VideoFrame stale;CHECK(!c.latest_video(stale));CHECK(c.route_control(e)&&local==1&&external==1);CHECK(c.submit_video("local",f));CHECK(c.latest_video(stale)&&stale.encoding==VideoEncoding::Svg&&stale.width==480);CHECK(c.upsert_source("external",InputSourceKind::External,true));CHECK(std::string(c.snapshot().active.data())=="external");CHECK(!c.latest_video(stale));VideoFrame h264;h264.encoding=VideoEncoding::H264AnnexB;h264.width=800;h264.height=480;h264.pts=123;h264.size=5;CHECK(c.submit_video("external",h264));CHECK(c.latest_video(stale)&&stale.encoding==VideoEncoding::H264AnnexB&&stale.pts==123);for(int i=0;i<40;i++)CHECK(c.route_control(e));auto s=c.snapshot();CHECK(s.control_count==kMaxControls&&s.control_dropped>=9);VideoFrame oversized;oversized.size=kMaxVideoBytes+1;CHECK(!c.submit_video("external",oversized));{ // ── 模型扩展：元数据（Now Playing）──
  SessionCore c;
  MediaInfo m{};
  CHECK(!c.set_media_info(m));                   // 初值即全零 → 无变化（刻意的语义）
  std::strncpy(m.title.data(),"夜曲",m.title.size()-1);
  m.duration_ms=227000;m.position_ms=41000;m.playing=1;
  CHECK(c.set_media_info(m));
  CHECK(!c.set_media_info(m));                   // 重复设同值 → 无变化
  auto sm=c.snapshot();
  CHECK(std::string(sm.media.title.data())=="夜曲");
  CHECK(sm.media.duration_ms==227000&&sm.media.position_ms==41000&&sm.media.playing==1);
  CHECK(sm.media.artist[0]=='\0');                // 未设字段保持空，不是垃圾
}
{ // ── 模型扩展：专辑封面（revision 语义 + 超长截断 + 读取面）──
  SessionCore c;
  CHECK(!c.snapshot().media.has_artwork);
  CHECK(!c.snapshot().media.artwork_bytes);
  std::string big(kMaxArtwork+500,'A');
  CHECK(c.set_artwork(1,big.data(),big.size()));
  CHECK(!c.set_artwork(1,"x",1));                // 同 revision = 同一张图 → 无变化
  const char* d=nullptr;std::size_t n=0;uint32_t r=0;
  CHECK(c.artwork(&d,&n,&r));
  CHECK(d&&n==kMaxArtwork&&r==1);                // 超长被截到上限，不越界
  CHECK(std::memcmp(d,big.data(),n)==0);
  auto s=c.snapshot();
  CHECK(s.media.has_artwork&&s.media.artwork_bytes==kMaxArtwork&&s.media.artwork_revision==1);
  CHECK(c.set_artwork(2,nullptr,0));             // 清空封面
  CHECK(!c.artwork(&d,&n,&r)&&n==0&&r==2);
  CHECK(!c.snapshot().media.has_artwork&&!c.snapshot().media.artwork_bytes);
}
{ // ── 模型扩展：歌词（revision 语义 + UTF-8 边界截断）──
  SessionCore c;
  std::string lrc;
  for(int i=0;i<2000;++i)lrc+="中";             // 6000 字节 > kMaxLyrics
  CHECK(lrc.size()>kMaxLyrics);
  CHECK(c.set_lyrics(1,lrc));
  CHECK(!c.set_lyrics(1,"x"));                   // 同 revision → 无变化
  std::string_view out;uint32_t r=0;
  CHECK(c.lyrics(&out,&r)&&r==1);
  CHECK(out.size()<=kMaxLyrics-1);
  CHECK(out.size()%3==0);                       // "中"是 3 字节 → 必须整字截断
  CHECK(valid_utf8(out));                       // 绝不出现半个汉字
  CHECK(c.snapshot().media.has_lyrics&&c.snapshot().media.lyrics_bytes==out.size());
  CHECK(c.set_lyrics(2,""));                     // 清空歌词
  CHECK(!c.lyrics(&out,&r)&&out.empty());
  CHECK(!c.snapshot().media.has_lyrics);
}
{ // ── 模型扩展：显示（safe area / 副屏平面 / 校准 / 昼夜 / 帧率）──
  SessionCore c;
  DisplayConfig d{};
  CHECK(!c.set_display_config(d));               // 初值即全零 → 无变化
  d.safe_top=60;d.safe_bottom=40;d.safe_left=20;d.safe_right=20;
  d.width=1920;d.height=1080;d.day_night=1;d.gamma_pct=95;d.contrast_pct=105;d.saturation_pct=90;
  d.aux_enabled=1;d.aux_x=1280;d.aux_y=0;d.aux_w=640;d.aux_h=480;d.target_fps=60;d.actual_fps=30;
  CHECK(c.set_display_config(d));
  auto s=c.snapshot();
  CHECK(s.display.safe_top==60&&s.display.safe_bottom==40&&s.display.safe_left==20);
  CHECK(s.display.day_night==1&&s.display.gamma_pct==95&&s.display.saturation_pct==90);
  CHECK(s.display.aux_enabled==1&&s.display.aux_w==640);
  CHECK(s.display.target_fps==60&&s.display.actual_fps==30);
}
{ // ── 模型扩展：交互能力（多点/旋钮/手势/接近/HID/VoiceOver/硬键）──
  SessionCore c;
  InteractionState in{};
  CHECK(!c.set_input_state(in));                 // 初值即全零 → 无变化
  in.multi_touch_points=10;in.touchpad=1;in.knob=1;in.proximity=1;in.hid_mode=2;
  in.voiceover=1;in.assistive_touch=1;
  CHECK(c.set_input_state(in));
  auto s=c.snapshot();
  CHECK(s.input.multi_touch_points==10&&s.input.proximity==1&&s.input.voiceover==1);
  CHECK(c.note_hard_key(19));CHECK(!c.note_hard_key(19));CHECK(c.note_hard_key(20));
  CHECK(c.snapshot().input.last_key_code==20);
}
{ // ── 模型扩展：音频（多声道 + 编码 + 角色）──
  SessionCore c;
  AudioState a{};
  CHECK(!c.set_audio_state(a));                  // 初值即全零 → 无变化
  a.channels=6;a.sample_rate=48000;a.codec=AudioCodec::Alac;
  a.simultaneous_streams=2;a.active_role=AudioRole::Navigation;
  CHECK(c.set_audio_state(a));
  auto s=c.snapshot();
  CHECK(s.audio.channels==6&&s.audio.sample_rate==48000);
  CHECK(s.audio.codec==AudioCodec::Alac&&s.audio.active_role==AudioRole::Navigation);
}
{ // ── 模型扩展：闪避（导航压低媒体，比例语义）──
  SessionCore c;
  CHECK(c.snapshot().audio.media_volume_ppm==1000000);
  CHECK(c.snapshot().audio.duck_ratio_ppm==333333);      // 默认 1/3
  CHECK(c.set_ducking(true,333333,1000000));
  CHECK(!c.set_ducking(true,333333,1000000));           // 同值 → 无变化
  auto s=c.snapshot();
  CHECK(s.audio.nav_active==1&&s.audio.media_volume_ppm==333333);
  CHECK(s.audio.nav_volume_ppm==1000000&&s.audio.active_role==AudioRole::Navigation);
  CHECK(c.set_ducking(false,1000000,1000000));          // 恢复
  CHECK(c.snapshot().audio.active_role==AudioRole::Media);
  CHECK(!c.set_ducking(false,1000000,1000000));
}
{ // ── 模型扩展：车况（GPS / 车速 / 挡位 / 油量）──
  SessionCore c;
  VehicleState v{};
  CHECK(!c.set_vehicle_state(v));                // 初值即全零 → 无变化
  v.gear=3;v.speed_kph=72;v.latitude=31.23;v.longitude=121.47;v.heading_deg=90.5f;
  v.fuel_pct=63;v.range_km=420;v.outside_temp_c=21;v.odometer_km=12345;v.night_mode=1;
  CHECK(c.set_vehicle_state(v));
  auto s=c.snapshot();
  CHECK(s.vehicle.speed_kph==72&&s.vehicle.gear==3&&s.vehicle.fuel_pct==63);
  CHECK(s.vehicle.latitude>31.0&&s.vehicle.longitude>121.0&&s.vehicle.night_mode==1);
}
{ // ── 模型扩展：电话 / 通讯录 ──
  SessionCore c;
  TelephonyState t{};
  CHECK(!c.set_telephony_state(t));              // 初值即全零 → 无变化
  t.call_state=3;t.signal_bars=4;t.battery_pct=88;t.call_duration_s=42;t.dtmf_supported=1;
  t.contacts_ready=1;t.contact_count=37;t.call_log_ready=1;t.call_log_count=12;
  std::strncpy(t.caller.data(),"张三",t.caller.size()-1);
  std::strncpy(t.caller_number.data(),"13800138000",t.caller_number.size()-1);
  CHECK(c.set_telephony_state(t));
  auto s=c.snapshot();
  CHECK(s.telephony.call_state==3&&s.telephony.signal_bars==4&&s.telephony.contact_count==37);
  CHECK(std::string(s.telephony.caller.data())=="张三");
  CHECK(std::string(s.telephony.caller_number.data())=="13800138000");
}
{ // ── 模型扩展：链路（激活 / 内容加密 / 文件传输 / OTA）──
  SessionCore c;
  LinkState l{};
  CHECK(!c.set_link_state(l));                   // 初值即全零 → 无变化
  l.activation_state=3;l.content_encryption=2;l.file_transfer_active=1;
  l.file_transfer_bytes=4096;l.file_transfer_total=65536;l.ota_state=1;
  CHECK(c.set_link_state(l));
  auto s=c.snapshot();
  CHECK(s.link.activation_state==3&&s.link.content_encryption==2);
  CHECK(s.link.file_transfer_active==1&&s.link.file_transfer_bytes==4096&&s.link.ota_state==1);
}
{ // ── 模型扩展：多设备会话表 ──
  SessionCore c;
  CHECK(c.upsert_session("iphone",true));
  auto s=c.snapshot();
  CHECK(s.link.session_count==1&&s.link.session_active[0]==1);
  CHECK(std::string(s.link.active_session.data())=="iphone");
  CHECK(c.upsert_session("android",false));
  CHECK(c.snapshot().link.session_count==2);
  CHECK(c.set_active_session("android"));
  CHECK(!c.set_active_session("android"));               // 已经是活跃 → 无变化
  s=c.snapshot();
  CHECK(std::string(s.link.active_session.data())=="android");
  CHECK(s.link.session_active[0]==0&&s.link.session_active[1]==1);   // 至多一个活跃
  CHECK(!c.set_active_session("missing"));                // 不在表里 → 拒绝
  CHECK(!c.upsert_session("android",true));               // 同 id 重复只更新活跃位
  CHECK(c.snapshot().link.session_count==2);
  CHECK(c.upsert_session("iphone",true));
  CHECK(std::string(c.snapshot().link.active_session.data())=="iphone");
  for(std::size_t i=c.snapshot().link.session_count;i<kMaxSessionList;++i)
    CHECK(c.upsert_session(std::string("dev")+std::to_string(i),false));
  CHECK(c.snapshot().link.session_count==kMaxSessionList);
  CHECK(!c.upsert_session("overflow",false));            // 表满→如实拒绝，不静默丢弃
  CHECK(c.set_active_session(""));
  CHECK(std::string(c.snapshot().link.active_session.data()).empty());
  CHECK(c.snapshot().link.session_active[0]==0);
}
{ // ── 会话 id 的契约：非空且短于槽位；超长/空如实拒绝（不静默截成半个汉字）──
  SessionCore c;
  std::string cn;
  for(int i=0;i<20;++i)cn+="中";                 // 60 字节，超过 32 字节槽位
  CHECK(!c.upsert_session(cn,false));            // 超长 → 拒绝
  CHECK(!c.upsert_session("",false));             // 空 id → 拒绝
  CHECK(c.snapshot().link.session_count==0);
  CHECK(c.upsert_session("huawei_mate60",true));  // 合法 id 正常入库
  CHECK(std::string(c.snapshot().link.sessions[0].data())=="huawei_mate60");
  // 注：copy_text 的 UTF-8 边界回退是纵深防护（所有调用点都先校验了长度）；
  // 它真正可达且被验证的路径是 set_lyrics（见上方歌词用例）。
}
{ // ── ControlEvent 全部新类型都要能路由，不认识的类型也不能拆会话 ──
  SessionCore c;
  CHECK(c.upsert_source("phone",InputSourceKind::External,true));
  int got=0;ControlEvent last{};
  CHECK(c.register_control_sink("phone",[&](const ControlEvent& e){++got;last=e;}));
  const ControlEvent::Type types[]={
    ControlEvent::Type::Touch,ControlEvent::Type::Key,ControlEvent::Type::MultiTouch,
    ControlEvent::Type::Knob,ControlEvent::Type::Gesture,ControlEvent::Type::Proximity,
    ControlEvent::Type::Voice,ControlEvent::Type::Telephony,ControlEvent::Type::VehicleCtrl};
  for(auto t:types){ControlEvent e{};e.type=t;CHECK(c.route_control(e));}
  CHECK(got==9);
  CHECK(last.type==ControlEvent::Type::VehicleCtrl);
  ControlEvent knob{};knob.type=ControlEvent::Type::Knob;
  knob.knob_dir=ControlEvent::KnobDir::Left;
  knob.knob_steps=-3;
  CHECK(c.route_control(knob)&&got==10);
  CHECK(last.knob_steps==-3&&last.knob_dir==ControlEvent::KnobDir::Left);
  // 未定义的类型值：汽车协议上"不理解即丢"，但绝不能拆会话或计为丢弃
  ControlEvent weird{};weird.type=static_cast<ControlEvent::Type>(200);
  CHECK(c.route_control(weird)&&got==11);
  CHECK(c.snapshot().control_dropped==0);
  CHECK(std::string(c.snapshot().active.data())=="phone");     // 会话仍在
  // 其它新字段也要原样透传
  ControlEvent gt{};gt.type=ControlEvent::Type::Gesture;
  std::strncpy(gt.gesture.data(),"swipe_left",gt.gesture.size()-1);
  CHECK(c.route_control(gt)&&std::string(last.gesture.data())=="swipe_left");
  ControlEvent vc{};vc.type=ControlEvent::Type::VehicleCtrl;
  std::strncpy(vc.ctrl.data(),"ac_temp_up",vc.ctrl.size()-1);
  CHECK(c.route_control(vc)&&std::string(last.ctrl.data())=="ac_temp_up");
  ControlEvent dt{};dt.type=ControlEvent::Type::Telephony;
  std::strncpy(dt.dtmf.data(),"1234#",dt.dtmf.size()-1);
  CHECK(c.route_control(dt)&&std::string(last.dtmf.data())=="1234#");
}
{ // ── 多点触控：10 点上限，超出的必须在入口夹住，不能透传给下游 ──
  SessionCore c;
  CHECK(c.upsert_source("phone",InputSourceKind::External,true));
  uint8_t seen=0;
  CHECK(c.register_control_sink("phone",[&](const ControlEvent& e){seen=e.point_count;}));
  ControlEvent e{};e.type=ControlEvent::Type::MultiTouch;
  e.point_count=static_cast<uint8_t>(kMaxMultiTouch);
  for(std::size_t i=0;i<kMaxMultiTouch;++i){
    e.points[i].x=static_cast<int16_t>(i*10);e.points[i].y=static_cast<int16_t>(i*20);e.points[i].id=static_cast<uint8_t>(i);
  }
  CHECK(c.route_control(e));
  CHECK(seen==kMaxMultiTouch);                 // 满 10 点原样通过
  e.point_count=11;                           // 生产者越界填数（数组只有 10 个）
  CHECK(c.route_control(e));
  CHECK(seen==kMaxMultiTouch);                 // 必须在入口被夹到 10
  e.point_count=255;
  CHECK(c.route_control(e));
  CHECK(seen==kMaxMultiTouch);
  e.point_count=0;
  CHECK(c.route_control(e)&&seen==0);          // 0 点也要能过（抬指结束）
  CHECK(c.snapshot().control_dropped==0);
}
{ // ── AudioChunk：默认值 + codec/channels/bits/role 往返 + 容量边界 ──
  SessionCore c;
  CHECK(c.upsert_source("phone",InputSourceKind::External,true));
  AudioChunk a{};
  CHECK(a.codec==AudioCodec::Pcm16&&a.role==AudioRole::Media);   // 默认 PCM16 / 媒体
  CHECK(a.sample_rate==8000&&a.channels==2&&a.bits==16);
  CHECK(a.samples.size()==kMaxAudioBytes/sizeof(int16_t));
  a.codec=AudioCodec::AacEld;a.role=AudioRole::Navigation;
  a.sample_rate=48000;a.channels=6;a.bits=24;a.sequence=7;
  a.sample_count=a.samples.size();
  CHECK(c.submit_audio("phone",a));
  AudioChunk got{};
  CHECK(c.latest_audio(got));
  CHECK(got.codec==AudioCodec::AacEld&&got.role==AudioRole::Navigation);
  CHECK(got.sample_rate==48000&&got.channels==6&&got.bits==24&&got.sequence==7);
  CHECK(got.sample_count==got.samples.size());
  AudioChunk over{};over.sequence=99;
  over.sample_count=over.samples.size()+1;     // 越界 → 必须拒绝，绝不能读越界
  CHECK(!c.submit_audio("phone",over));
  CHECK(c.latest_audio(got)&&got.sequence==7); // 上一块未被污染
  AudioChunk valid{};valid.sequence=42;valid.sample_count=16;
  CHECK(!c.submit_audio("other",valid));       // 合法块但非活跃源 → 拒绝
  CHECK(c.latest_audio(got)&&got.sequence==7); // 仍未污染
}
{ // ── artwork()/lyrics() 的 revision 语义与空参保护 ──
  SessionCore c;
  const char* p=nullptr;std::size_t n=0;uint32_t r=0;
  CHECK(!c.artwork(nullptr,&n,&r));            // 空参 → 安全返回 false，不崩
  CHECK(!c.artwork(&p,nullptr,&r));
  CHECK(!c.artwork(&p,&n,nullptr));
  CHECK(!c.artwork(&p,&n,&r));                 // 尚无封面
  CHECK(c.set_artwork(5,"AAAA",4));
  CHECK(c.artwork(&p,&n,&r));
  CHECK(n==4&&r==5&&p&&p[0]=='A');
  CHECK(!c.set_artwork(5,"BBBB",4));           // 同 revision → 不改
  CHECK(c.artwork(&p,&n,&r)&&n==4&&p[0]=='A');   // 仍是旧图
  CHECK(c.set_artwork(6,"BBBB",4));           // 新 revision → 替换
  CHECK(c.artwork(&p,&n,&r)&&n==4&&p[0]=='B'&&r==6);
  CHECK(c.set_artwork(7,nullptr,5));            // 清空（size 被忽略）
  CHECK(!c.artwork(&p,&n,&r)&&n==0&&r==7);
  std::string_view out;
  CHECK(!c.lyrics(nullptr,&r));                 // 空参 → false
  CHECK(!c.lyrics(&out,nullptr));
  uint32_t lr=0;
  CHECK(!c.lyrics(&out,&lr));                   // 尚无歌词
  CHECK(c.set_lyrics(9,"[00:01.00]abc"));
  CHECK(c.lyrics(&out,&lr)&&lr==9&&out=="[00:01.00]abc");
  CHECK(!c.set_lyrics(9,"[00:02.00]xxx"));      // 同 revision → 不改
  CHECK(c.lyrics(&out,&lr)&&out=="[00:01.00]abc");
  CHECK(c.set_lyrics(10,"[00:02.00]def"));
  CHECK(c.lyrics(&out,&lr)&&out=="[00:02.00]def"&&lr==10);
  CHECK(c.set_lyrics(11,""));                  // 清空
  CHECK(!c.lyrics(&out,&lr)&&out.empty()&&lr==11);
}
{ // ── 并发：setter 与 snapshot()/artwork()/lyrics() 交错不得崩溃或死锁 ──
  // 不强求一致视图（快照本来就是某个瞬间），只要求不 UB、不越界、不锁死。
  SessionCore c;
  CHECK(c.upsert_source("phone",InputSourceKind::External,true));
  const int rounds=4000;
  std::atomic<bool> bad{false};
  std::thread writers([&]{
    for(int i=0;i<rounds;++i){
      MediaInfo m{};m.position_ms=static_cast<uint32_t>(i);m.playing=static_cast<uint8_t>(i&1);
      c.set_media_info(m);
      DisplayConfig d{};d.safe_top=static_cast<uint16_t>(i);d.target_fps=60;c.set_display_config(d);
      AudioState a{};a.channels=static_cast<uint8_t>(i%8);c.set_audio_state(a);
      VehicleState v{};v.speed_kph=i;c.set_vehicle_state(v);
      LinkState l{};l.file_transfer_bytes=static_cast<uint32_t>(i);c.set_link_state(l);
      TelephonyState t{};t.signal_bars=static_cast<uint8_t>(i%6);c.set_telephony_state(t);
      InteractionState in{};in.multi_touch_used=static_cast<uint8_t>(i%11);c.set_input_state(in);
      c.set_ducking((i&1)!=0,1000000-i,1000000);
      c.upsert_session("s",true);c.set_active_session("s");
      const std::string art(1024,'x');
      c.set_artwork(static_cast<uint32_t>(i)+1,art.data(),art.size());
      c.set_lyrics(static_cast<uint32_t>(i)+1,"[00:01.00]词");
      c.note_hard_key(static_cast<uint32_t>(i));
    }
  });
  std::thread readers([&]{
    for(int i=0;i<rounds;++i){
      auto s=c.snapshot();
      if(s.media.position_ms>static_cast<uint32_t>(rounds))bad=true;
      if(s.display.safe_top>static_cast<uint16_t>(rounds))bad=true;
      if(s.input.multi_touch_used>10)bad=true;
      const char* d=nullptr;std::size_t n=0;uint32_t r=0;
      if(c.artwork(&d,&n,&r)&&(!d||n>kMaxArtwork))bad=true;
      std::string_view out;uint32_t lr=0;
      if(c.lyrics(&out,&lr)&&out.size()>kMaxLyrics)bad=true;
    }
  });
  writers.join();readers.join();
  CHECK(!bad.load());
  CHECK(c.snapshot().media.position_ms<static_cast<uint32_t>(rounds));
  CHECK(c.snapshot().link.session_count==1);   // 同 id 反复 upsert 只占一个槽
}
CHECK(checks>20);std::cout<<"core checks executed: "<<checks<<'\n';return 0;}
