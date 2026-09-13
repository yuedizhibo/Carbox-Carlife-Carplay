#include "wirelesscarplay/catplay_media_protocol.hpp"
#include "wirelesscarplay/real_media_store.hpp"
#include "core/session_core.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
using namespace mvp;
using namespace mvp::catplay_media;
int checks=0;
#define CHECK(x) do { ++checks; if(!(x)){std::cerr<<__LINE__<<": " #x "\n";return 1;} }while(false)
std::array<uint8_t,kHeaderBytes> wire(const Header& h){std::array<uint8_t,kHeaderBytes> b{};b[0]='C';b[1]='P';b[2]='M';b[3]='F';b[4]=1;put_be16(b.data()+6,kHeaderBytes);put_be16(b.data()+8,uint16_t(h.type));put_be16(b.data()+10,h.flags);put_be32(b.data()+12,h.stream_id);put_be64(b.data()+16,h.session_id);put_be64(b.data()+24,h.sequence);put_be64(b.data()+32,h.pts);put_be32(b.data()+40,h.payload_bytes);put_be32(b.data()+44,h.p0);put_be32(b.data()+48,h.p1);put_be32(b.data()+52,h.p2);put_be32(b.data()+56,h.p3);return b;}
bool count(void* p,const Header&,std::span<const uint8_t>){++*static_cast<int*>(p);return true;}
bool put(RealMediaStore& s,Header h,std::vector<uint8_t> p={}){h.payload_bytes=uint32_t(p.size());return s.ingest(h,p);}std::size_t packet_type_count(const std::string& p,uint16_t type){std::size_t count=0,pos=0;while(pos+kHeaderBytes<=p.size()){auto raw=reinterpret_cast<const uint8_t*>(p.data()+pos);if(be16(raw+8)==type)++count;pos+=kHeaderBytes+be32(raw+40);}return pos==p.size()?count:0;}
// MediaRelay 的探针：统计回调次数，并验证正文真的递到了下游。
struct RelayProbe{
  int info=0,art=0,lyr=0;uint32_t art_rev=0,lyr_rev=0;std::size_t art_size=0,lyr_size=0;
  std::array<char,kMaxTextLong> title{};bool fail=false;
  static bool info_cb(void*p,const MediaInfo&m){auto*s=static_cast<RelayProbe*>(p);++s->info;std::memcpy(s->title.data(),m.title.data(),m.title.size());return !s->fail;}
  static bool art_cb(void*p,uint32_t r,const char*d,std::size_t n){(void)d;auto*s=static_cast<RelayProbe*>(p);++s->art;s->art_rev=r;s->art_size=n;return !s->fail;}
  static bool lyr_cb(void*p,uint32_t r,const char*d,std::size_t n){(void)d;auto*s=static_cast<RelayProbe*>(p);++s->lyr;s->lyr_rev=r;s->lyr_size=n;return !s->fail;}
  MediaRelaySink sink(){return MediaRelaySink{this,info_cb,art_cb,lyr_cb};}
};
int main(){
  Header hello{Type::ServerHello,0,0,0,0,0,0,kMaxWirePayloadBytes,1000,7,1};auto golden=wire(hello);CHECK(golden[0]=='C'&&golden[3]=='F'&&golden[7]==64);Parser parser;int records=0;CHECK(parser.push(golden.data(),17,count,&records)==ParseResult::NeedMore&&records==0);CHECK(parser.push(golden.data()+17,golden.size()-17,count,&records)==ParseResult::NeedMore&&records==1);auto bad=golden;bad[4]=2;parser.reset();CHECK(parser.push(bad.data(),bad.size(),count,&records)==ParseResult::Invalid);bad=golden;put_be32(bad.data()+40,kMaxWirePayloadBytes+1);parser.reset();CHECK(parser.push(bad.data(),bad.size(),count,&records)==ParseResult::Invalid);Header max_frame{Type::VideoFrame,0,1,1,1,1,kMaxWirePayloadBytes,1,640,360,1};auto max_header=wire(max_frame);std::vector<uint8_t> coalesced(kHeaderBytes+kMaxWirePayloadBytes+kHeaderBytes);std::memcpy(coalesced.data(),max_header.data(),kHeaderBytes);coalesced[kHeaderBytes+2]=1;coalesced[kHeaderBytes+3]=0x41;std::memcpy(coalesced.data()+kHeaderBytes+kMaxWirePayloadBytes,golden.data(),kHeaderBytes);parser.reset();records=0;CHECK(parser.push(coalesced.data(),coalesced.size(),count,&records)==ParseResult::NeedMore&&records==2);
  RealMediaStore store;store.transport_connected();CHECK(put(store,hello));CHECK(put(store,{Type::Heartbeat,0,0,0,0,1,0,0,0,0,0}));CHECK(!put(store,{Type::Heartbeat,0,0,0,7,1,0,0,0,0,0}));CHECK(put(store,{Type::Heartbeat,0,0,0,7,2,0,0,0,0,0}));CHECK(put(store,{Type::SessionBegin,0,0,9,0,0,0,0,0,0,0}));CHECK(put(store,{Type::VideoStart,0,4,9,0,0,0,0,0,0,0}));std::vector<uint8_t> config{0,0,0,1,0x67,0x42,0,0x1e,0,0,0,1,0x68,0xce,6,0xe2};CHECK(put(store,{Type::VideoConfig,0,4,9,0,0,0,1,640,360,11},config));uint64_t sid{};uint32_t stream{};CHECK(store.take_keyframe_request(sid,stream)&&sid==9&&stream==4);std::vector<uint8_t> idr{0,0,0,1,0x65,0x88};CHECK(put(store,{Type::VideoFrame,kFlagKeyframe,4,9,1,10,0,1,640,360,11},idr));CHECK(store.snapshot().video==RealMediaSnapshot::Video::Active);std::vector<uint8_t> delta{0,0,1,0x41,0x9a};CHECK(put(store,{Type::VideoFrame,0,4,9,3,30,0,1,640,360,11},delta));CHECK(store.snapshot().video==RealMediaSnapshot::Video::WaitingKeyframe);CHECK(!store.take_keyframe_request(sid,stream));CHECK(put(store,{Type::VideoFrame,kFlagKeyframe,4,9,4,40,0,1,640,360,11},idr));store.set_selected(false);std::string packet;CHECK(!store.video_packet(0,packet)&&store.snapshot().video==RealMediaSnapshot::Video::Inactive);store.set_selected(true);CHECK(store.snapshot().video==RealMediaSnapshot::Video::WaitingKeyframe);std::this_thread::sleep_for(std::chrono::milliseconds(510));CHECK(store.take_keyframe_request(sid,stream)&&sid==9&&stream==4);CHECK(put(store,{Type::VideoFrame,kFlagKeyframe,4,9,5,50,0,1,640,360,11},idr));CHECK(put(store,{Type::VideoFrame,0,4,9,6,60,0,1,640,360,11},delta));CHECK(store.video_packet(0,packet));CHECK(packet_type_count(packet,uint16_t(Type::VideoFrame))==2);CHECK(store.video_packet(5,packet));CHECK(packet_type_count(packet,uint16_t(Type::VideoFrame))==1);CHECK(!store.video_packet(6,packet));CHECK(put(store,{Type::VideoConfig,0,4,9,0,0,0,1,640,360,12},config));CHECK(!store.take_keyframe_request(sid,stream));CHECK(put(store,{Type::VideoFrame,kFlagKeyframe,4,9,7,70,0,1,640,360,12},idr));CHECK(store.video_packet(6,packet));CHECK(be16(reinterpret_cast<const uint8_t*>(packet.data())+8)==uint16_t(Type::VideoConfig)&&store.snapshot().video_config_generation==1);CHECK(put(store,{Type::VideoEnd,0,4,9,0,0,0,0,0,0,0}));CHECK(put(store,{Type::VideoStart,0,4,9,0,0,0,0,0,0,0}));CHECK(put(store,{Type::VideoConfig,0,4,9,0,0,0,1,640,360,12},config));CHECK(put(store,{Type::VideoFrame,kFlagKeyframe,4,9,1,80,0,1,640,360,12},idr));CHECK(!store.video_packet(7,packet));CHECK(store.video_packet(0,packet)&&be16(reinterpret_cast<const uint8_t*>(packet.data())+8)==uint16_t(Type::VideoConfig)&&store.snapshot().video_config_generation==2);
  // 同 role 换新 stream id = 【重置而非拒绍】（见 real_media_store.cpp 的 VideoStart 分支）。
  // 依据：旧实现直接 return false，会让新流不被登记 —— 表现为画面在某一刻永久卡住；
  // 而“会话重建 / 分辨率变化 / 手机重连”都会走到这条路径，所以必须重置。
  // 因此这里不再断言“被拒”，而是断言“重置真的发生了”：slot 换成新 id 且退回 WaitingConfig。
  CHECK(put(store,{Type::VideoStart,0,5,9,0,0,0,1,0,0,0}));
  CHECK(put(store,{Type::VideoStart,0,6,9,0,0,0,1,0,0,0}));
  CHECK(store.snapshot().screens[1].stream_id==6&&store.snapshot().screens[1].state==RealMediaSnapshot::Video::WaitingConfig);
  CHECK(put(store,{Type::VideoConfig,0,6,9,0,0,0,1,800,480,21},config));CHECK(store.take_keyframe_request(sid,stream)&&stream==6);CHECK(put(store,{Type::VideoFrame,kFlagKeyframe,6,9,1,90,0,1,800,480,21},idr));CHECK(store.snapshot().screens[0].state==RealMediaSnapshot::Video::Active&&store.snapshot().screens[1].state==RealMediaSnapshot::Video::Active);CHECK(store.snapshot().screens[1].stream_id==6);CHECK(store.video_packet(1,0,packet)&&be32(reinterpret_cast<const uint8_t*>(packet.data())+12)==6);CHECK(put(store,{Type::VideoEnd,0,4,9,0,0,0,0,0,0,0}));CHECK(store.snapshot().screens[0].state==RealMediaSnapshot::Video::Inactive&&store.snapshot().screens[1].state==RealMediaSnapshot::Video::Active);
  RealMediaStore::AudioCursor cursor;CHECK(put(store,{Type::AudioGain,0,0,9,0,0,0,500,100000,0,1}));CHECK(store.audio_packet(cursor,packet)&&packet_type_count(packet,uint16_t(Type::AudioGain))==1);CHECK(!store.audio_packet(cursor,packet));CHECK(!put(store,{Type::AudioGain,0,0,9,0,0,0,500,100001,0,1}));
  std::vector<uint8_t> descriptor(16);descriptor[0]=96;descriptor[1]=1;CHECK(put(store,{Type::AudioStart,0,7,9,0,0,0,2,48000,2,1},descriptor));std::vector<uint8_t> pcm(3840);CHECK(put(store,{Type::AudioChunk,0,7,9,1,50,0,2,48000,2,960},pcm));CHECK(store.snapshot().audio_count==1&&store.snapshot().audio[0].state==RealMediaSnapshot::Audio::State::Active);CHECK(put(store,{Type::AudioChunk,kFlagDiscontinuity,7,9,2,60,0,2,48000,2,960},pcm));CHECK(store.snapshot().audio[0].state==RealMediaSnapshot::Audio::State::Discontinuous);CHECK(!put(store,{Type::AudioChunk,0,7,9,3,70,0,2,48000,3,960},pcm));std::vector<uint8_t> mono(1920);CHECK(put(store,{Type::AudioStart,0,8,9,0,0,0,1,48000,1,1},descriptor));CHECK(put(store,{Type::AudioChunk,0,8,9,1,80,0,1,48000,1,960},mono));CHECK(store.audio_packet(cursor,packet)&&packet_type_count(packet,uint16_t(Type::AudioChunk))==3);CHECK(!store.audio_packet(cursor,packet));CHECK(put(store,{Type::AudioChunk,0,8,9,2,90,0,1,48000,1,960},mono));CHECK(store.audio_packet(cursor,packet)&&packet_type_count(packet,uint16_t(Type::AudioChunk))==1);CHECK(store.audio_packet(packet)&&packet_type_count(packet,uint16_t(Type::AudioChunk))==4&&store.snapshot().audio_count==2);auto audio_generation=store.snapshot().audio[0].generation;CHECK(put(store,{Type::AudioEnd,0,7,9,0,0,0,0,0,0,0}));CHECK(put(store,{Type::AudioStart,0,7,9,0,0,0,2,48000,2,1},descriptor));bool restarted=false;for(std::size_t i=0;i<store.snapshot().audio_count;++i)if(store.snapshot().audio[i].stream_id==7&&store.snapshot().audio[i].generation>audio_generation)restarted=true;CHECK(restarted);CHECK(put(store,{Type::AudioEnd,0,8,9,0,0,0,0,0,0,0}));for(uint64_t i=1;i<=20;++i)CHECK(put(store,{Type::AudioChunk,0,7,9,i,100+i,0,2,48000,2,960},pcm));RealMediaStore::AudioCursor backlog;CHECK(store.audio_packet(backlog,packet));
// 音频 ring 早已从 16 槽扩到 128 槽（kAudioChunks=128；MAPPING §0 的“16 块”是旧描述）。
// 所以 20 条全部保留、一条不丢：原断言的 16/4 两个数字正是“20 条撞 16 槽”的结果。
CHECK(packet_type_count(packet,uint16_t(Type::AudioChunk))==20);CHECK(store.snapshot().audio_dropped==0);
// 保留原断言的意图（溢出时丢最旧并计数）：推到 132 条，128 槽下应恰丢 4 条。
for(uint64_t i=21;i<=132;++i)CHECK(put(store,{Type::AudioChunk,0,7,9,i,100+i,0,2,48000,2,960},pcm));CHECK(store.snapshot().audio_dropped==4);store.transport_closed();
// transport_closed 是【传输级】关闭：故意保留会话与最后一帧（见 real_media_store.cpp:249-269）：
// 客户端停在最后一帧而不是黑屏；重连时引擎会重发 ServerHello，若此刻把媒体面清掉
// 反而会断流并迫使客户端重等关键帧。所以“会话仍在”是**有意**的语义。
// 注意：snapshot().video 只镜像 role 0（sync_screen_locked 里的 if(role==0)），
// 而 role 0 已在上一段被 VideoEnd 结束 —— 要验“最后一帧被保留”得看 role 1 那条流。
CHECK(store.snapshot().session==RealMediaSnapshot::Session::Active);
CHECK(store.snapshot().screens[1].state==RealMediaSnapshot::Video::Active&&store.snapshot().screens[1].stream_id==6);
// 真正结束会话的是 end_session()（会话级）：它调 clear_media_locked()，
// 把 session / 两条屏 / snapshot().video 一起置回 Inactive。
// 保留原断言的意图（会话结束后视频确实被清），只是换成会真正结束会话的那个 API。
CHECK(store.end_session(9));
CHECK(store.snapshot().session==RealMediaSnapshot::Session::Inactive);
CHECK(store.snapshot().video==RealMediaSnapshot::Video::Inactive);
CHECK(store.snapshot().screens[0].state==RealMediaSnapshot::Video::Inactive&&store.snapshot().screens[1].state==RealMediaSnapshot::Video::Inactive);
  SessionCore core;CHECK(core.upsert_source("local",InputSourceKind::LocalDesktop,true));CHECK(core.upsert_source("synthetic",InputSourceKind::Synthetic,true));CHECK(core.upsert_source("real",InputSourceKind::External,true));CHECK(std::string(core.snapshot().active.data())=="real");CHECK(core.upsert_source("real",InputSourceKind::External,false));CHECK(std::string(core.snapshot().active.data())=="synthetic");  // ══════════ L2 新增：多声道 / 多格式 / 闪避 / 帧率 / 元数据转发 ══════════
  {
    RealMediaStore s2;s2.transport_ready();CHECK(s2.begin_session(77));
    // 1) 多声道被接受且如实入队（不做任何声道变换）
    std::vector<uint8_t> d6(16);d6[0]=96;d6[1]=1;d6[4]=2;d6[5]=16;d6[6]=16;d6[7]=1;put_be32(d6.data()+8,2|4|16);put_be32(d6.data()+12,12);
    CHECK(valid_audio_format(d6.data(),d6.size(),6));
    CHECK(put(s2,{Type::AudioStart,0,20,77,0,0,0,2,48000,6,1},d6));
    CHECK(s2.snapshot().audio_count==1&&s2.snapshot().audio[0].channels==6);
    CHECK(s2.snapshot().audio[0].bytes_per_frame==12&&s2.snapshot().audio[0].bits==16);
    CHECK(s2.snapshot().audio[0].codec==2&&s2.snapshot().audio[0].role==uint32_t(AudioRole::Media));
    std::vector<uint8_t> p6(3840);   // 320 帧 x 6ch x 2B = 3840
    CHECK(put(s2,{Type::AudioChunk,0,20,77,1,10,0,2,48000,6,320},p6));
    CHECK(s2.snapshot().audio[0].state==RealMediaSnapshot::Audio::State::Active);
    CHECK(!put(s2,{Type::AudioChunk,0,20,77,2,20,0,2,48000,6,960},p6)); // 960*6*2 与 payload 不符
    // 2) 声道数越界必须被拒
    CHECK(!put(s2,{Type::AudioStart,0,21,77,0,0,0,2,48000,0,1},d6));
    CHECK(!put(s2,{Type::AudioStart,0,21,77,0,0,0,2,48000,9,1},d6));
    // 3) 24 位容器
    std::vector<uint8_t> d24(16);d24[0]=96;d24[1]=1;d24[4]=3;d24[5]=24;d24[6]=24;d24[7]=2;put_be32(d24.data()+12,6);
    CHECK(valid_audio_format(d24.data(),d24.size(),2));
    CHECK(put(s2,{Type::AudioStart,0,22,77,0,0,0,1,48000,2,1},d24));
    std::vector<uint8_t> p24(3840);   // 640 帧 x 2ch x 3B = 3840
    CHECK(put(s2,{Type::AudioChunk,0,22,77,1,10,0,1,48000,2,640},p24));
    // 4) 格式描述符自洽性：非法值必须被拒
    auto bad=d6;bad[4]=9;CHECK(!valid_audio_format(bad.data(),bad.size(),6));
    bad=d6;bad[5]=24;bad[6]=16;CHECK(!valid_audio_format(bad.data(),bad.size(),6));
    bad=d6;put_be32(bad.data()+12,99);CHECK(!valid_audio_format(bad.data(),bad.size(),6));
    // 5) 向后兼容：旧生产者只写前 4 字节，新增字节全零 → 仍被接受，按 16bit 处理
    std::vector<uint8_t> legacy(16);legacy[0]=96;legacy[1]=1;
    CHECK(valid_audio_format(legacy.data(),legacy.size(),2));
    CHECK(put(s2,{Type::AudioStart,0,23,77,0,0,0,2,48000,2,1},legacy));
    CHECK(s2.snapshot().audio_count==3);
    CHECK(s2.snapshot().audio[2].codec==2&&s2.snapshot().audio[2].bits==16&&s2.snapshot().audio[2].bytes_per_frame==4);
    // 6) role：由 audio_type 推导（1 = alert/nav）+ 可显式覆盖
    CHECK(put(s2,{Type::AudioStart,0,24,77,0,0,0,1,48000,1,1},legacy));
    bool nav=false,tele=false;
    for(std::size_t i=0;i<s2.snapshot().audio_count;++i)if(s2.snapshot().audio[i].stream_id==24)nav=s2.snapshot().audio[i].role==uint32_t(AudioRole::Navigation);
    CHECK(nav);
    CHECK(s2.set_audio_role(24,uint32_t(AudioRole::Telephony)));
    for(std::size_t i=0;i<s2.snapshot().audio_count;++i)if(s2.snapshot().audio[i].stream_id==24)tele=s2.snapshot().audio[i].role==uint32_t(AudioRole::Telephony);
    CHECK(tele);
    CHECK(!s2.set_audio_role(24,9));
    // 7) 闪避：导航播报压低【媒体轨】，导航轨不动
    //    参考 AudioTrackManagerDualNormal.java:376-395：setVolume(maxVolume / mMusicAudioTrackVolumReduceRatio)，ratio=3
    CHECK(s2.set_duck_config(333333,300));
    CHECK(s2.set_ducking(true));
    CHECK(s2.ducking_active());
    CHECK(s2.snapshot().ducking.active&&s2.snapshot().ducking.media_ppm==333333);
    CHECK(s2.snapshot().ducking.nav_ppm==1000000&&s2.snapshot().ducking.ratio_ppm==333333);
    CHECK(!s2.set_ducking(true));   // 幂等：重复压低不产生新的渐变命令
    RealMediaStore::AudioCursor c2;std::string pk;
    CHECK(s2.audio_packet(c2,pk)&&packet_type_count(pk,uint16_t(Type::AudioGain))==1);
    {const auto* raw=reinterpret_cast<const uint8_t*>(pk.data());CHECK(be32(raw+44)==300&&be32(raw+48)==333333);} // p0=渐变ms p1=ppm
    CHECK(s2.set_ducking(false));
    CHECK(s2.snapshot().ducking.media_ppm==1000000);
    CHECK(!s2.set_ducking(false));  // 幂等
    CHECK(!s2.set_duck_config(0,300)&&!s2.set_duck_config(1000001,300));
    // 8) 帧率：目标值入状态 + 挂一个待下发请求 + 实测收帧计数
    CHECK(!s2.set_target_fps(0,0)&&!s2.set_target_fps(0,241)&&!s2.set_target_fps(2,30));
    CHECK(put(s2,{Type::VideoStart,0,30,77,0,0,0,0,0,0,0}));
    std::vector<uint8_t> vcfg{0,0,0,1,0x67,0x42,0,0x1e,0,0,0,1,0x68,0xce,6,0xe2};
    CHECK(put(s2,{Type::VideoConfig,0,30,77,0,0,0,1,640,360,1},vcfg));
    CHECK(s2.set_target_fps(0,60));
    CHECK(s2.snapshot().screens[0].target_fps==60);
    CHECK(!s2.set_target_fps(0,60));  // 幂等
    uint32_t rr=0,rf=0;CHECK(s2.take_rate_request(rr,rf)&&rr==0&&rf==60);
    CHECK(!s2.take_rate_request(rr,rf));  // 已取走
    std::vector<uint8_t> vidr{0,0,0,1,0x65,0x88};
    CHECK(put(s2,{Type::VideoFrame,kFlagKeyframe,30,77,1,10,0,1,640,360,1},vidr));
    CHECK(s2.snapshot().screens[0].frames_received==1);
    // 9) 元数据/封面：会话边界必须清空正文，不能泄漏到新会话
    std::vector<char> art(2048,0x41);
    CHECK(s2.media_relay().publish_artwork(1,art.data(),art.size()));
    CHECK(s2.snapshot().media.has_artwork&&s2.snapshot().media.artwork_bytes==2048);
    CHECK(put(s2,{Type::SessionEnd,0,0,77,0,0,0,0,0,0,0}));
    CHECK(!s2.snapshot().media.has_artwork&&s2.snapshot().media.artwork_bytes==0);
  }
  // 多声道 / 高采样率：只做【合理性边界】，不做能力裁剪
  //（用户硬要求：多声道与杜比靠“原样透传”支持，不能被我们自己的校验挡在门外）
  {
    RealMediaStore s3;s3.transport_ready();CHECK(s3.begin_session(88));
    // 8 声道（7.1）x 16bit = 16 B/帧
    std::vector<uint8_t> d8(16);d8[0]=96;d8[1]=1;d8[4]=2;d8[5]=16;d8[6]=16;d8[7]=1;put_be32(d8.data()+12,16);
    CHECK(valid_audio_format(d8.data(),d8.size(),8));
    CHECK(s3.start_audio(40,2,96000,8,decode_audio_format(d8.data(),d8.size(),8)));
    CHECK(s3.snapshot().audio_count==1&&s3.snapshot().audio[0].channels==8&&s3.snapshot().audio[0].sample_rate==96000);
    CHECK(s3.snapshot().audio[0].bytes_per_frame==16);
    std::vector<uint8_t> p8(3840,0x5a);   // 240 帧 x 8ch x 2B = 3840
    CHECK(s3.push_audio(40,1,10,p8));
    CHECK(s3.snapshot().audio[0].state==RealMediaSnapshot::Audio::State::Active);
    // 6 声道（5.1）x 24bit = 18 B/帧，192 kHz
    std::vector<uint8_t> d6b(16);d6b[0]=96;d6b[1]=1;d6b[4]=3;d6b[5]=24;d6b[6]=24;d6b[7]=1;put_be32(d6b.data()+12,18);
    CHECK(s3.start_audio(41,2,192000,6,decode_audio_format(d6b.data(),d6b.size(),6)));
    CHECK(s3.snapshot().audio_count==2&&s3.snapshot().audio[1].sample_rate==192000&&s3.snapshot().audio[1].bits==24);
    // 位深非 8/16/24/32 必须被拒
    std::vector<uint8_t> dbad(16);dbad[0]=96;dbad[1]=1;dbad[4]=2;dbad[5]=20;dbad[6]=20;
    CHECK(!valid_audio_format(dbad.data(),dbad.size(),2));
    // 255 声道：拒且不崩，也不产生任何新流
    CHECK(!s3.start_audio(42,2,48000,255,catplay_media::AudioFormat{}));
    CHECK(s3.snapshot().audio_count==2);
    // 采样率边界：8000..192000 内放行，外拒绝
    CHECK(!s3.start_audio(43,2,7999,2,catplay_media::AudioFormat{}));
    CHECK(!s3.start_audio(43,2,192001,2,catplay_media::AudioFormat{}));
    CHECK(s3.start_audio(43,2,8000,2,catplay_media::AudioFormat{}));
    CHECK(s3.end_audio(43));
    CHECK(valid_rate(8000)&&valid_rate(96000)&&valid_rate(192000)&&!valid_rate(7999)&&!valid_rate(192001));
    // CPMF 线上的同一条边界：8 声道可通过，9 声道被拒
    std::vector<uint8_t> wire8(16);wire8[0]=96;wire8[1]=1;wire8[4]=2;wire8[5]=16;wire8[6]=16;wire8[7]=1;put_be32(wire8.data()+12,16);
    CHECK(put(s3,{Type::AudioStart,0,60,88,0,0,0,2,96000,8,1},wire8));
    CHECK(!put(s3,{Type::AudioStart,0,61,88,0,0,0,2,96000,9,1},wire8));
    // 2 声道 @192000：这里必须用与 2 声道一致的描述符（bytes/frame = 2ch x 16bit = 4）。
    // 复用 wire8（bytes/frame=16，即 8ch x 16bit）会被实现的交叉校验拒掉 ——
    // 那是对的行为（描述符与实际声道数不符就拒），所以修测试而不是修实现。
    std::vector<uint8_t> wire2=wire8;put_be32(wire2.data()+12,4);
    CHECK(put(s3,{Type::AudioStart,0,62,88,0,0,0,2,192000,2,1},wire2));
    std::vector<uint8_t> p2ch(3840);
    CHECK(put(s3,{Type::AudioChunk,0,62,88,1,10,0,2,192000,2,960},p2ch));
  }
  // 压缩载荷（AAC-ELD）：字节原样往返，我们没动它一个字节
  {
    RealMediaStore s4;s4.transport_ready();CHECK(s4.begin_session(99));
    std::vector<uint8_t> d(16);d[0]=96;d[1]=1;d[4]=6;d[5]=0;d[6]=0;d[7]=1;  // codec=6=AacEld
    put_be32(d.data()+12,0);   // 压缩流没有 PCM 帧长，所以 bytes_per_frame 不声明
    CHECK(valid_audio_format(d.data(),d.size(),2));
    CHECK(put(s4,{Type::AudioStart,0,50,99,0,0,0,2,48000,2,1},d));
    CHECK(s4.snapshot().audio_count==1&&s4.snapshot().audio[0].opaque==1&&s4.snapshot().audio[0].codec==6);
    // 构造一段“不是任何 PCM 帧长整数倍”的载荷：证明校验没拿 PCM 等式去挡它
    std::vector<uint8_t> eld(777);
    for(std::size_t i=0;i<eld.size();++i)eld[i]=uint8_t(i*31+7);
    CHECK(eld.size()%4!=0);   // 2ch*2B=4 → 不是整数帧
    // p3=1024（AAC 典型包长）在 48k 下是 21.3ms > 20ms：PCM 的 p1/50 约束不得用在这里
    CHECK(put(s4,{Type::AudioChunk,kFlagOpaquePayload,50,99,1,10,0,2,48000,2,1024},eld));
    RealMediaStore::AudioCursor c4;std::string pk4;
    CHECK(s4.audio_packet(c4,pk4));
    std::size_t pos=0;const uint8_t* got=nullptr;uint32_t got_size=0,got_flags=0;
    while(pos+kHeaderBytes<=pk4.size()){
      const auto* raw=reinterpret_cast<const uint8_t*>(pk4.data()+pos);
      const uint32_t bytes=be32(raw+40);
      if(bytes>pk4.size()-pos-kHeaderBytes)break;
      if(be16(raw+8)==uint16_t(Type::AudioChunk)){got=raw+kHeaderBytes;got_size=bytes;got_flags=be16(raw+10);}
      pos+=kHeaderBytes+bytes;
    }
    CHECK(pos==pk4.size());
    CHECK(got&&got_size==eld.size()&&(got_flags&kFlagOpaquePayload));
    CHECK(std::memcmp(got,eld.data(),eld.size())==0);   // 一个字节都没动
    // 直灌 API 走同一条路
    CHECK(s4.push_audio_opaque(50,2,20,1024,eld));
    CHECK(!s4.push_audio_opaque(50,0,20,1024,eld));     // sequence 不能为 0
    CHECK(!s4.push_audio_opaque(50,3,20,0,eld));        // frames 不能为 0
    CHECK(!s4.push_audio_opaque(50,4,20,1024,{}));      // 空载荷
    // 压缩包数上界（防垃圾值，非能力裁剪）
    CHECK(!put(s4,{Type::AudioChunk,kFlagOpaquePayload,50,99,5,50,0,2,48000,2,kMaxOpaqueFramesPerPacket+1},eld));
  }
  // MediaRelay：可注册回调 + 定长缓冲 + revision 去重 + 迟到订阅者可补齐
  {
    MediaRelay relay;RelayProbe probe;
    CHECK(!relay.has_sink());
    relay.set_sink(probe.sink());
    CHECK(relay.has_sink());
    MediaInfo info;info.valid=true;
    const char* t="测试标题";std::memcpy(info.title.data(),t,std::strlen(t));
    CHECK(relay.publish_media_info(info));
    CHECK(probe.info==1&&std::string(probe.title.data())==t);
    CHECK(!relay.publish_media_info(info));   // 无变化 → 不重发
    info.position_ms=1234;CHECK(relay.publish_media_info(info)&&probe.info==2);
    CHECK(relay.media_info().position_ms==1234);
    std::vector<char> a(4096,0x7a);
    CHECK(relay.publish_artwork(7,a.data(),a.size()));
    CHECK(probe.art==1&&probe.art_rev==7&&probe.art_size==4096);
    CHECK(!relay.publish_artwork(7,a.data(),a.size()));           // 同 revision 同内容 → 不发
    a[0]=0x7b;CHECK(relay.publish_artwork(7,a.data(),a.size()));  // 内容不同 → 发
    CHECK(relay.publish_artwork(8,nullptr,0));                    // 清除封面
    const char* ap=nullptr;std::size_t as=0;uint32_t ar=0;
    CHECK(!relay.artwork(&ap,&as,&ar)&&as==0&&ar==8);
    std::vector<char> big(kMaxArtwork+1,0);
    CHECK(!relay.publish_artwork(9,big.data(),big.size()));       // 超限拒绝（不截断）
    CHECK(!relay.publish_artwork(9,nullptr,16));                  // 非空 size + 空指针
    std::string lrc="[00:01.00]测试\n[00:05.00]second\n";
    CHECK(relay.publish_lyrics(3,lrc));
    CHECK(probe.lyr==1&&probe.lyr_rev==3&&probe.lyr_size==lrc.size());
    CHECK(!relay.publish_lyrics(3,lrc));
    std::string_view got;uint32_t lr=0;CHECK(relay.lyrics(&got,&lr)&&lr==3&&got==lrc);
    std::string huge(kMaxLyrics+1,'x');
    CHECK(!relay.publish_lyrics(4,huge));                         // 超长拒绝：截断会显示成残缺的 LRC 行
    // 下游返回 false（链路断开）不回滚正文：订阅者恢复后可以按 revision 补齐
    probe.fail=true;info.position_ms=4321;
    CHECK(relay.publish_media_info(info));
    probe.fail=false;
    CHECK(relay.media_info().position_ms==4321);
    RelayProbe p2;relay.set_sink(p2.sink());
    CHECK(relay.media_info().position_ms==4321&&relay.lyrics(&got,&lr)&&got==lrc);
    relay.clear_sink();CHECK(!relay.has_sink());
    relay.reset();
    CHECK(!relay.media_info().valid&&relay.media_info_generation()==0);
    const char* ap2=nullptr;std::size_t as2=0;uint32_t ar2=0;CHECK(!relay.artwork(&ap2,&as2,&ar2));
  }
std::cout<<"media checks executed: "<<checks<<'\n';
}
