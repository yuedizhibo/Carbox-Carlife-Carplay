#include "wirelesscarplay/catplay_media_client.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace mvp {
// 测试访问面：所有注入都走 handle() 的**真实记录路径**（不再绕过会话/媒体面直调 handle_ext），
// 这样"只有被 RealMediaStore 接受过的会话才能改状态面"这条不变式在测试里也真实成立。
struct CatPlayApiTestAccess {
  static bool record(CatPlayMediaClient& client, const catplay_media::Header& h, std::string_view text) {
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    return client.handle(h, bytes);
  }
  // ServerHello 的 p0..p3 是 Core 固定校验值，必须照抄。
  static bool handshake(CatPlayMediaClient& client) {
    return client.handle(catplay_media::Header{catplay_media::Type::ServerHello, 0, 0, 0, 0, 0, 0,
                                               catplay_media::kMaxWirePayloadBytes, 1000, 7, 1}, {});
  }
  // 握手 + 开始一个会话。重复 begin 同一 id 会被媒体面幂等拒绝（那是同一会话的快照）。
  static bool open_session(CatPlayMediaClient& client, uint64_t session_id) {
    if (!client.hello_ && !handshake(client)) return false;
    return client.handle(catplay_media::Header{catplay_media::Type::SessionBegin, 0, 0, session_id, 0, 0, 0, 0, 0, 0, 0}, {});
  }
  static bool end_session(CatPlayMediaClient& client, uint64_t session_id) {
    return client.handle(catplay_media::Header{catplay_media::Type::SessionEnd, 0, 0, session_id, 0, 0, 0, 0, 0, 0, 0}, {});
  }
  static bool audio_start(CatPlayMediaClient& client, uint64_t session, uint32_t stream, uint32_t audio_type,
                          uint32_t rate, uint32_t channels, std::span<const uint8_t> descriptor) {
    return client.handle(catplay_media::Header{catplay_media::Type::AudioStart, 0, stream, session, 0, 0,
                                               uint32_t(descriptor.size()), audio_type, rate, channels, 1},
                         descriptor);
  }
  static bool audio_end(CatPlayMediaClient& client, uint64_t session, uint32_t stream) {
    return client.handle(catplay_media::Header{catplay_media::Type::AudioEnd, 0, stream, session, 0, 0, 0, 4, 0, 0, 0}, {});
  }
  static bool audio_gain(CatPlayMediaClient& client, uint64_t session, uint32_t duration_ms, uint32_t ppm, uint32_t generation) {
    return client.handle(catplay_media::Header{catplay_media::Type::AudioGain, 0, 0, session, 0, 0, 0, duration_ms, ppm, 0, generation}, {});
  }
  // 扩展（新）类型：先按线格式自校验，再走真实 handle()。
  static bool chunk(CatPlayMediaClient& client, uint64_t session, catplay_media::ext::DType type,
                    uint32_t index, uint32_t count, uint32_t total, uint32_t revision, std::string_view text) {
    catplay_media::Header h{};
    h.type = static_cast<catplay_media::Type>(type);
    h.session_id=session; h.p0=index; h.p1=count; h.p2=total; h.p3=revision;
    h.payload_bytes=uint32_t(text.size());
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(text.data()),text.size()};
    return catplay_media::ext::valid_declared_ext(h) && catplay_media::ext::valid_static_ext(h,bytes) &&
           client.handle(h,bytes);
  }
  static uint64_t session(const CatPlayMediaClient& client) { return client.session_id_; }
  // 本地镜像读取面：证明"未选中时数据仍然被收下"（不承诺跨输入回放）。
  static const MediaInfo& media(const CatPlayMediaClient& client) { return client.media_; }
  static const DisplayConfig& display(const CatPlayMediaClient& client) { return client.display_; }
  static const AudioState& audio(const CatPlayMediaClient& client) { return client.audio_; }
};
}

#define CHECK(condition) do { if(!(condition)) { std::cerr<<"FAIL "<<__LINE__<<": " #condition "\n"; return 1; } } while(false)
int main() {
  using namespace mvp;
  using D=catplay_media::ext::DType;
  using T=catplay_media::Type;

  // ── 1) 封面/歌词分片重组（会话 42）+ 陈旧/空会话记录一律跳过 ────────────────
  {
    auto core_owned=std::make_unique<SessionCore>(); SessionCore& core=*core_owned;
    auto store_owned=std::make_unique<RealMediaStore>(); RealMediaStore& store=*store_owned;
    auto client_owned=std::make_unique<CatPlayMediaClient>(core,store); CatPlayMediaClient& client=*client_owned;
    auto chunk_s=[&](uint64_t session,D d,uint32_t i,uint32_t n,uint32_t bytes,uint32_t rev,std::string_view s) {
      return CatPlayApiTestAccess::chunk(client,session,d,i,n,bytes,rev,s);
    };
    auto chunk=[&](D d,uint32_t i,uint32_t n,uint32_t bytes,uint32_t rev,std::string_view s) {
      return chunk_s(42,d,i,n,bytes,rev,s);
    };
    // 握手之后、会话之前：扩展记录带着 session_id=0，必须整条跳过（不落 Core、不拆会话）。
    CHECK(CatPlayApiTestAccess::handshake(client));
    CHECK(chunk_s(0,D::LyricsChunk,0,1,3,0,"abc"));
    CHECK(!core.snapshot().media.has_lyrics);
    CHECK(CatPlayApiTestAccess::open_session(client,42));
    CHECK(CatPlayApiTestAccess::session(client)==42);
    CHECK(store.snapshot().session==RealMediaSnapshot::Session::Active && store.snapshot().session_id==42);
    CHECK(std::string(core.snapshot().active.data())=="catplay-real");
    // A final chunk alone must not publish a partial lyric.
    CHECK(chunk(D::LyricsChunk,1,2,6,1,"abc"));
    CHECK(!core.snapshot().media.has_lyrics);
    CHECK(chunk(D::LyricsChunk,0,2,6,2,"abc"));
    CHECK(chunk(D::LyricsChunk,1,2,6,2,"def"));
    std::string_view lyric; uint32_t rev{};
    CHECK(core.lyrics(&lyric,&rev) && lyric=="abcdef" && rev==2);
    // Missing middle, short total, changing total/revision cannot replace committed data.
    CHECK(chunk(D::LyricsChunk,0,3,6,3,"ab"));
    CHECK(chunk(D::LyricsChunk,2,3,6,3,"cdef"));
    CHECK(core.lyrics(&lyric,&rev) && lyric=="abcdef" && rev==2);
    CHECK(chunk(D::LyricsChunk,0,1,6,4,"abc"));
    CHECK(core.lyrics(&lyric,&rev) && rev==2);
    CHECK(chunk(D::LyricsChunk,0,2,6,5,"abc"));
    CHECK(chunk(D::LyricsChunk,1,2,7,5,"defg"));
    CHECK(core.lyrics(&lyric,&rev) && rev==2);
    CHECK(chunk(D::ArtworkChunk,0,3,6,6,"ab"));
    CHECK(chunk(D::ArtworkChunk,2,3,6,6,"cdef"));
    CHECK(!core.snapshot().media.has_artwork);
    CHECK(chunk(D::ArtworkChunk,0,2,6,7,"abc"));
    CHECK(chunk(D::ArtworkChunk,1,2,6,8,"def"));
    CHECK(!core.snapshot().media.has_artwork);
    // Maximum legal chunk count is 256, not an 8-bit truncated 255.
    for(uint32_t i=0;i<256;++i) CHECK(chunk(D::ArtworkChunk,i,256,256,9,"x"));
    const char* art{}; std::size_t size{};
    CHECK(core.artwork(&art,&size,&rev) && size==256 && rev==9);
    CHECK(art[0]=='x' && art[255]=='x');
    // ── 陈旧 / 无会话的记录：整条跳过，核心状态一点不动 ──
    {
      const auto skipped_before=client.record_skipped();
      CHECK(chunk_s(7,D::LyricsChunk,0,1,3,60,"xyz"));                       // 陈旧会话 id
      CHECK(chunk_s(0,D::LyricsChunk,0,1,3,61,"xyz"));                       // 无会话 id
      CHECK(chunk_s(7,D::ArtworkChunk,0,1,256,61,"Q"));                      // 陈旧会话的封面分片
      std::string_view now; uint32_t now_rev{};
      CHECK(core.lyrics(&now,&now_rev) && now_rev==2 && now==lyric);          // 歌词没被改动
      const char* now_art{}; std::size_t now_size{}; uint32_t now_art_rev{};
      CHECK(core.artwork(&now_art,&now_size,&now_art_rev) && now_size==256 && now_art_rev==9 && now_art[0]=='x');
      const auto before_display=core.snapshot().display;
      CHECK(chunk_s(7,D::DayNight,1,0,0,0,""));                              // 陈旧会话的显示记录
      CHECK(core.snapshot().display==before_display);
      CHECK(client.record_skipped()>=skipped_before+4);
      CHECK(CatPlayApiTestAccess::session(client)==42);                      // 会话仍然活着
    }
  }

  // ── 2) 跨会话分片不得混拼；同一会话内仍然照常组装 ──────────────────────────
  {
    auto core_owned=std::make_unique<SessionCore>(); SessionCore& core=*core_owned;
    auto store_owned=std::make_unique<RealMediaStore>(); RealMediaStore& store=*store_owned;
    auto client_owned=std::make_unique<CatPlayMediaClient>(core,store); CatPlayMediaClient& client=*client_owned;
    CHECK(CatPlayApiTestAccess::open_session(client,1));
    CHECK(CatPlayApiTestAccess::chunk(client,1,D::LyricsChunk,0,2,6,101,"abc"));
    // 新会话（id=2）：上一会话的半份分片必须作废，第 1 片不能把两个会话拼在一起。
    CHECK(CatPlayApiTestAccess::open_session(client,2));
    CHECK(CatPlayApiTestAccess::session(client)==2);
    CHECK(CatPlayApiTestAccess::chunk(client,2,D::LyricsChunk,1,2,6,101,"def"));
    CHECK(!core.snapshot().media.has_lyrics);
    std::string_view lyric; uint32_t rev{};
    CHECK(!core.lyrics(&lyric,&rev));
    // 覆盖：同一会话里成对的两片照常组装（证明上面的拒绝不是分片机制坏了）。
    CHECK(CatPlayApiTestAccess::open_session(client,1));
    CHECK(CatPlayApiTestAccess::chunk(client,1,D::LyricsChunk,0,2,6,102,"abc"));
    CHECK(CatPlayApiTestAccess::chunk(client,1,D::LyricsChunk,1,2,6,102,"def"));
    CHECK(core.lyrics(&lyric,&rev) && rev==102 && lyric=="abcdef");
  }

  // ── 3) 音频：描述符 codec/role、重复快照不累加、AudioEnd 只摘匹配的那条 ────
  {
    auto core_owned=std::make_unique<SessionCore>(); SessionCore& core=*core_owned;
    auto store_owned=std::make_unique<RealMediaStore>(); RealMediaStore& store=*store_owned;
    auto client_owned=std::make_unique<CatPlayMediaClient>(core,store); CatPlayMediaClient& client=*client_owned;
    CHECK(CatPlayApiTestAccess::open_session(client,42));
    // 旧生产者：描述符 [4..15] 全零 → PCM16 + role_from_audio_type(2=Media)，声道/采样率如实保留。
    std::array<uint8_t,16> legacy{};
    legacy[0]=96; legacy[1]=1;
    CHECK(CatPlayApiTestAccess::audio_start(client,42,7,2,44100,2,legacy));
    { const auto a=core.snapshot().audio;
      CHECK(a.codec==AudioCodec::Pcm16 && a.active_role==AudioRole::Media);
      CHECK(a.channels==2 && a.sample_rate==44100 && a.simultaneous_streams==1); }
    // 同一条流的重复快照：镜像按最新值走，但计数不重复累加。
    CHECK(CatPlayApiTestAccess::audio_start(client,42,7,2,48000,2,legacy));
    { const auto a=core.snapshot().audio;
      CHECK(a.sample_rate==48000 && a.simultaneous_streams==1); }
    // 第二条流 → 2；结束一条没登记过的流不得让计数下溢。
    CHECK(CatPlayApiTestAccess::audio_start(client,42,8,3,48000,1,legacy));
    CHECK(core.snapshot().audio.simultaneous_streams==2);
    CHECK(CatPlayApiTestAccess::audio_end(client,42,99));
    CHECK(core.snapshot().audio.simultaneous_streams==2);
    CHECK(CatPlayApiTestAccess::audio_end(client,42,7));
    CHECK(core.snapshot().audio.simultaneous_streams==1);
    // 描述符声明 AAC-ELD（codec 6）+ role 3=Telephony：codec/role 都走描述符，不信 audio_type。
    std::array<uint8_t,16> aac{};
    aac[0]=96; aac[1]=1; aac[4]=6; aac[5]=16; aac[6]=16; aac[7]=3;
    catplay_media::put_be32(aac.data()+12, 2*16/8);      // bytes_per_frame = 2ch×16bit/8 = 4
    CHECK(CatPlayApiTestAccess::audio_start(client,42,9,0,48000,2,aac));
    { const auto a=core.snapshot().audio;
      CHECK(a.codec==AudioCodec::AacEld && a.active_role==AudioRole::Telephony);
      CHECK(a.channels==2 && a.sample_rate==48000 && a.simultaneous_streams==2); }
    // 描述符声明 PCM24（codec 3、24 位容器）+ role 2=Navigation。
    std::array<uint8_t,16> pcm24{};
    pcm24[0]=96; pcm24[1]=1; pcm24[4]=3; pcm24[5]=24; pcm24[6]=24; pcm24[7]=2;
    catplay_media::put_be32(pcm24.data()+12, 2*24/8);    // 6
    CHECK(CatPlayApiTestAccess::audio_start(client,42,10,4,24000,2,pcm24));
    { const auto a=core.snapshot().audio;
      CHECK(a.codec==AudioCodec::Pcm24 && a.active_role==AudioRole::Navigation);
      CHECK(a.channels==2 && a.sample_rate==24000 && a.simultaneous_streams==3); }
    // 畸形（声道 0）与陈旧会话的音频记录绝不落 Core：镜像逐字段不变。
    const auto before=core.snapshot().audio;
    CHECK(CatPlayApiTestAccess::audio_start(client,42,11,2,44100,0,legacy));     // 媒体面拒绝
    CHECK(CatPlayApiTestAccess::audio_start(client,999,12,2,44100,2,legacy));   // 陈旧会话
    CHECK(CatPlayApiTestAccess::audio_gain(client,999,100,500000,7));           // 陈旧会话
    CHECK(core.snapshot().audio==before);
    // 被接受的 AudioGain 照常落到当前活跃角色（此处是 Navigation）上。
    CHECK(CatPlayApiTestAccess::audio_gain(client,42,120,700000,1));
    CHECK(core.snapshot().audio.duck_transition_ms==120 && core.snapshot().audio.nav_volume_ppm==700000);
    // ── 会话结束：本地镜像与半份分片全部作废，旧会话 id 的记录不得再落地 ──
    CHECK(CatPlayApiTestAccess::end_session(client,42));
    CHECK(CatPlayApiTestAccess::session(client)==0);
    CHECK(!CatPlayApiTestAccess::media(client).has_lyrics && !CatPlayApiTestAccess::media(client).has_artwork);
    CHECK(CatPlayApiTestAccess::audio(client).simultaneous_streams==0);
    CHECK(std::string(core.snapshot().active.data())!="catplay-real");          // 源已离线
    const auto after_end=core.snapshot().audio;
    CHECK(CatPlayApiTestAccess::audio_start(client,42,13,2,44100,2,legacy));
    CHECK(CatPlayApiTestAccess::chunk(client,42,D::DayNight,1,0,0,0,""));
    CHECK(core.snapshot().audio==after_end);
  }

  // ── 4) 未选中的 catplay-real 不得覆盖别的输入已发布的状态 ──────────────────
  {
    auto core_owned=std::make_unique<SessionCore>(); SessionCore& core=*core_owned;
    auto store_owned=std::make_unique<RealMediaStore>(); RealMediaStore& store=*store_owned;
    auto client_owned=std::make_unique<CatPlayMediaClient>(core,store); CatPlayMediaClient& client=*client_owned;
    // 另一个外部输入（模拟 CarLife）先上线并被手动选中，再由它发布一份状态。
    CHECK(core.upsert_source("carlife-real",InputSourceKind::External,true));
    CHECK(core.set_selection(SelectionMode::Manual,"carlife-real"));
    MediaInfo car{};
    car.valid=true; car.playing=1;
    std::memcpy(car.title.data(),"CarLife Song",12);
    CHECK(core.set_media_info(car));
    DisplayConfig car_display{}; car_display.day_night=1; car_display.target_fps=30;
    CHECK(core.set_display_config(car_display));
    TelephonyState car_tel{}; car_tel.call_state=3;
    CHECK(core.set_telephony_state(car_tel));
    AudioState car_audio{}; car_audio.active_role=AudioRole::Navigation; car_audio.channels=1;
    CHECK(core.set_audio_state(car_audio));
    // CarPlay 会话开始：源会上线，但 active 仍是 carlife-real。
    CHECK(CatPlayApiTestAccess::open_session(client,42));
    CHECK(std::string(core.snapshot().active.data())=="carlife-real");
    // catplay-real 推元数据/显示/电话/音频：一个字节都不许落到 Core。
    const std::string blob=std::string("CarPlay Song")+'\0'+"Artist"+'\0'+"Album"+'\0'+"AA"+'\0'+"App"+'\0'+"Rock"+'\0';
    CHECK(CatPlayApiTestAccess::chunk(client,42,D::Metadata,0x3,210000,42000,(7u<<16)|12u,blob));
    CHECK(CatPlayApiTestAccess::chunk(client,42,D::DayNight,0,0,0,0,""));
    CHECK(CatPlayApiTestAccess::chunk(client,42,D::Ducking,1,333333,1000000,300,""));
    { const auto s=core.snapshot();
      CHECK(std::string(s.media.title.data())=="CarLife Song");
      CHECK(s.display.day_night==1 && s.display.target_fps==30);
      CHECK(s.telephony.call_state==3);
      CHECK(s.audio.active_role==AudioRole::Navigation && s.audio.channels==1 && s.audio.nav_active==0); }
    // 本地镜像照常收下最新数据（未选中也不丢）；但**不承诺**跨输入回放。
    CHECK(std::string(CatPlayApiTestAccess::media(client).title.data())=="CarPlay Song");
    CHECK(CatPlayApiTestAccess::display(client).day_night==0);
    CHECK(CatPlayApiTestAccess::audio(client).nav_active==1);
    // 重新选中本输入后，下一条记录自然把最新镜像带上去。
    CHECK(core.set_selection(SelectionMode::Manual,"catplay-real"));
    CHECK(std::string(core.snapshot().active.data())=="catplay-real");
    CHECK(CatPlayApiTestAccess::chunk(client,42,D::DayNight,1,0,0,0,""));
    CHECK(core.snapshot().display.day_night==1);
  }

  std::cout<<"CarPlay input API regressions passed\n";
}
