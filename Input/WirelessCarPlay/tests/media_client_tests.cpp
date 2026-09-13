#include "wirelesscarplay/catplay_media_client.hpp"
#include "wirelesscarplay/catplay_media_protocol.hpp"
#include "wirelesscarplay/real_media_store.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
using namespace mvp; using namespace mvp::catplay_media;
int checks=0;
#define CHECK(x) do { ++checks; if(!(x)){std::cerr<<__LINE__<<": " #x "\n";return 1;} }while(false)
// 任何 CHECK 失败都会从 main() 提前 return。此时若 std::thread 仍 joinable，
// 它的析构会调用 std::terminate（SIGABRT / exit 134），把真实的断言失败"掩盖成崩溃"。
// 本轮就为这个绕了很大一圈：看到 Subprocess aborted，实际只是某条 CHECK 失败。
struct ThreadJoiner { std::thread& t; ~ThreadJoiner(){ if(t.joinable()) t.join(); } };
// 声明顺序即析构顺序（后声明先析构）：ClientGuard 必须先停客户端，ThreadJoiner 随后 join；
// 反过来在客户端仍在跑时 join，可能阻塞到客户端自己超时才返回。
struct ClientGuard { CatPlayMediaClient& c; ~ClientGuard(){ c.stop(); } };
std::array<uint8_t,kHeaderBytes> raw(const Header& h){return encode_header(h);}bool write_record(int fd,Header h,std::span<const uint8_t> p={}){h.payload_bytes=uint32_t(p.size());auto b=raw(h);return send(fd,b.data(),b.size(),MSG_NOSIGNAL)==ssize_t(b.size())&&(p.empty()||send(fd,p.data(),p.size(),MSG_NOSIGNAL)==ssize_t(p.size()));}
bool wait_for(auto f,int ms=1200){for(int i=0;i<ms/5;++i){if(f())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return f();}
int main(){std::string path="/tmp/cpmf-client-"+std::to_string(getpid());int listener=socket(AF_UNIX,SOCK_STREAM,0);CHECK(listener>=0);unlink(path.c_str());sockaddr_un a{};a.sun_family=AF_UNIX;std::memcpy(a.sun_path,path.c_str(),path.size()+1);CHECK(bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0&&listen(listener,2)==0);// 【栈帧】这三个对象按值放栈会让 main() 的栈帧达到 ~4.86 MiB
// （SessionCore 406 KiB + RealMediaStore 3.25 MiB + CatPlayMediaClient 1.2 MiB），
// 与"大对象放栈 ⇒ main() 击穿 8 MiB 栈 ⇒ 服务启动即崩"是同一类风险。
// unique_ptr + 引用别名：既离开栈，又不需改任何调用点（core./store./client. 全部照旧）。
// 声明顺序即析构顺序的逆序 ⇒ client 先于 store/core 销毁，符合依赖方向。
auto core_owned=std::make_unique<SessionCore>();SessionCore& core=*core_owned;
auto store_owned=std::make_unique<RealMediaStore>();RealMediaStore& store=*store_owned;
auto client_owned=std::make_unique<CatPlayMediaClient>(core,store,path);CatPlayMediaClient& client=*client_owned;std::atomic<bool> reverse_ok{},second_ok{},touch_ok{};std::thread producer([&]{int c=accept(listener,nullptr,nullptr);if(c<0)return;Header hello{Type::ServerHello,0,0,0,0,0,0,kMaxWirePayloadBytes,1000,7,1};std::vector<uint8_t> cfg{0,0,0,1,0x67,0x42,0,0x1e,0,0,0,1,0x68,0xce,6,0xe2};(void)write_record(c,hello);(void)write_record(c,{Type::Heartbeat,0,0,0,0,1,0,0,0,0,0});(void)write_record(c,{Type::SessionBegin,0,0,42,0,0,0,0,0,0,0});(void)write_record(c,{Type::VideoStart,0,3,42,0,0,0,0,0,0,0});(void)write_record(c,{Type::VideoConfig,0,3,42,0,0,0,1,640,360,1},cfg);std::array<uint8_t,kHeaderBytes> request{};timeval tv{0,500000};setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));auto received=recv(c,request.data(),request.size(),0);reverse_ok=received==ssize_t(request.size())&&request[0]=='C'&&request[1]=='P'&&request[2]=='M'&&request[3]=='F'&&request[4]==1&&be16(request.data()+8)==uint16_t(Type::RequestKeyframe)&&be64(request.data()+16)==42&&be32(request.data()+12)==3;std::this_thread::sleep_for(std::chrono::milliseconds(250));close(c);fd_set set;FD_ZERO(&set);FD_SET(listener,&set);timeval reconnect_tv{0,700000};int second=select(listener+1,&set,nullptr,nullptr,&reconnect_tv)>0?accept(listener,nullptr,nullptr):-1;if(second>=0){bool ok=write_record(second,hello)&&write_record(second,{Type::Heartbeat,0,0,0,0,2,0,0,0,0,0})&&write_record(second,{Type::SessionBegin,0,0,43,0,0,0,0,0,0,0})&&write_record(second,{Type::VideoStart,0,3,43,0,0,0,0,0,0,0})&&write_record(second,{Type::VideoConfig,0,3,43,0,0,0,1,640,360,2},cfg);std::vector<uint8_t> idr{0,0,0,1,0x65,0x88};ok=ok&&write_record(second,{Type::VideoFrame,kFlagKeyframe,3,43,1,3,0,1,640,360,2},idr);second_ok=ok;timeval touch_tv{0,700000};setsockopt(second,SOL_SOCKET,SO_RCVTIMEO,&touch_tv,sizeof(touch_tv));for(int i=0;i<4&&!touch_ok;++i){std::array<uint8_t,kHeaderBytes> incoming{};auto n=recv(second,incoming.data(),incoming.size(),MSG_WAITALL);if(n!=ssize_t(incoming.size()))break;if(be16(incoming.data()+8)==uint16_t(Type::Touch))touch_ok=be64(incoming.data()+16)==43&&be64(incoming.data()+24)>0&&be32(incoming.data()+44)==uint32_t(ControlEvent::TouchPhase::Down)&&be32(incoming.data()+48)==320&&be32(incoming.data()+52)==180;}close(second);}});ThreadJoiner joiner{producer};ClientGuard client_guard{client};client.start();CHECK(wait_for([&]{return store.snapshot().session==RealMediaSnapshot::Session::Active;}));CHECK(wait_for([&]{return core.snapshot().source_count&&std::string(core.snapshot().active.data())=="catplay-real";}));CHECK(wait_for([&]{return store.snapshot().session_id==43&&store.snapshot().video==RealMediaSnapshot::Video::Active;}));ControlEvent touch;touch.type=ControlEvent::Type::Touch;touch.phase=ControlEvent::TouchPhase::Down;touch.x=320;touch.y=180;CHECK(core.route_control(touch));CHECK(wait_for([&]{return touch_ok.load();}));CHECK(reverse_ok&&second_ok);
// 【原断言已陈旧】这里原来是 session==Inactive。但 transport_closed()（real_media_store.cpp:249-267）
// 刻意【只重置握手面、保留媒体面】—— 注释原文："真正的会话边界是 SessionBegin（新会话才清）……
// 保留最后一帧与 SPS/PPS，客户端在最坏情况下停在最后一帧而不是黑屏"。
// 而本测试的断连是【瞬时】的（共 4 次，其中一次还成功重连），在瞬时断连处清会话本身就是错的。
// 正确期望：重连后仍是 Active 会话 43、ipc 重新 Connected、媒体面完好。
// 本测试里 producer 最后把两条连接都关了，所以收尾时握手面必然已是"已重置"（ipc=Unavailable）；
// 而媒体面（会话 43 / 最后一帧 / SPS+PPS）必须仍然完好 —— 这正是 transport_closed() 的契约。
// 用 wait_for 等断连被客户端处理到，避免与 EOF 探测的异步时序赛跑。
CHECK(wait_for([&]{auto s=store.snapshot();return s.session==RealMediaSnapshot::Session::Active&&s.session_id==43&&s.screens[0].state==RealMediaSnapshot::Video::Active;}));
CHECK(wait_for([&]{return store.snapshot().ipc==RealMediaSnapshot::Ipc::Unavailable;}));
CHECK(store.snapshot().screens[0].stream_id==3);
CHECK(store.snapshot().screens[0].config_bytes==16);
client.stop();producer.join();close(listener);unlink(path.c_str());
// ── 断连语义直调断言：不依赖并发时序，把契约本身钉死 ──
// （放在 stop/join 之后，因此与上面的 socket 驱动部分完全不交叉）
{
  std::array<uint8_t,16> cfg16{0,0,0,1,0x67,0x42,0,0x1e,0,0,0,1,0x68,0xce,6,0xe2};
  std::array<uint8_t,6> idr6{0,0,0,1,0x65,0x88};
  store.set_selected(true);
  CHECK(store.begin_session(7001));
  CHECK(store.set_video_stream(0,11,640,360,cfg16));
  CHECK(store.push_video_frame(0,1,1,true,idr6));
  auto live=store.snapshot();
  CHECK(live.session==RealMediaSnapshot::Session::Active&&live.session_id==7001);
  CHECK(live.screens[0].state==RealMediaSnapshot::Video::Active);
  CHECK(live.screens[0].stream_id==11&&live.screens[0].config_bytes==16&&live.screens[0].queued_frames>0);
  // 瞬时断连：握手面重置，媒体面（会话/最后一帧/SPS+PPS/配置代）逐项保留
  store.transport_closed(false);
  auto dropped=store.snapshot();
  CHECK(dropped.ipc==RealMediaSnapshot::Ipc::Unavailable);
  CHECK(dropped.session==RealMediaSnapshot::Session::Active&&dropped.session_id==7001);
  CHECK(dropped.screens[0].state==RealMediaSnapshot::Video::Active);
  CHECK(dropped.screens[0].stream_id==11&&dropped.screens[0].config_bytes==16);
  CHECK(dropped.screens[0].queued_frames==live.screens[0].queued_frames);
  CHECK(dropped.screens[0].config_generation==live.screens[0].config_generation);
  // 协议错误只是另一种 ipc 取值，同样不碰媒体面
  store.transport_closed(true);
  auto proto=store.snapshot();
  CHECK(proto.ipc==RealMediaSnapshot::Ipc::ProtocolError&&proto.protocol_dropped>=1);
  CHECK(proto.session==RealMediaSnapshot::Session::Active&&proto.session_id==7001);
  CHECK(proto.screens[0].config_bytes==16&&proto.screens[0].queued_frames>0);
  // 真正的会话边界：SessionBegin（新 id）才清媒体面与帧
  CHECK(store.begin_session(7002));
  auto fresh=store.snapshot();
  CHECK(fresh.session==RealMediaSnapshot::Session::Active&&fresh.session_id==7002);
  CHECK(fresh.screens[0].stream_id==0&&fresh.screens[0].config_bytes==0&&fresh.screens[0].queued_frames==0);
  CHECK(fresh.screens[0].state==RealMediaSnapshot::Video::Inactive);
  // 同一个 id 重复 begin 是幂等拒绝且不清媒体面；id=0 非法
  CHECK(!store.begin_session(7002));
  CHECK(!store.begin_session(0));
  // 复位成"等待握手"，避免影响后续条目（本块结束后不再有依赖）
  store.transport_closed(false);
  CHECK(store.snapshot().ipc==RealMediaSnapshot::Ipc::Unavailable);
}CHECK(sizeof(RealMediaStore)<=11U*1024U*1024U);std::cout<<"client checks executed: "<<checks<<'\n';}
