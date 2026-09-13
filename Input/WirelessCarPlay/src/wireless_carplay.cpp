#include "wirelesscarplay/wireless_carplay.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
namespace mvp {
LocalDesktopBackend::LocalDesktopBackend(SessionCore& c):core_(c){} LocalDesktopBackend::~LocalDesktopBackend(){stop();}
void LocalDesktopBackend::start(){if(started_)return;started_=true;core_.upsert_source(id(),kind(),true);core_.register_control_sink(id(),[this](const ControlEvent&e){on_control(e);});refresh();}void LocalDesktopBackend::refresh(){
  if(!started_)return;
  // 【栈帧】VideoFrame 的 256 KiB 载荷不能放栈上：refresh() 会被 HTTP 工作线程调用，
  // 线程栈大小不是本模块能假设的（本轮已有 main() 击穿 8 MiB 栈 ⇒ 启动即崩 /health=000 的先例）。
  // 改用堆分配：refresh() 只在选中变化时被调用，开销可忽略。
  auto f=std::make_unique<VideoFrame>();
  f->encoding=VideoEncoding::Svg;f->width=480;f->height=270;f->sequence=1;
  int n=std::snprintf(f->payload.data(),f->payload.size(),"<svg xmlns='http://www.w3.org/2000/svg' width='480' height='270'><rect width='100%%' height='100%%' fill='#25313a'/><rect x='20' y='20' width='440' height='210' rx='8' fill='#36454f'/><text x='42' y='80' fill='#dce9ef' font-size='28'>Local Desktop</text><text x='42' y='120' fill='#9fb3c1' font-size='18'>Fallback input active</text><text x='42' y='160' fill='#9fb3c1' font-size='16'>Synthetic wireless test mode is off</text></svg>");
  f->size=n>0?static_cast<std::size_t>(n):0;
  core_.submit_video(id(),*f);
  AudioChunk a;a.sequence=1;a.sample_rate=8000;a.sample_count=800;core_.submit_audio(id(),a);
}
void LocalDesktopBackend::stop(){if(!started_)return;started_=false;core_.unregister_control_sink(id());core_.upsert_source(id(),kind(),false);}void LocalDesktopBackend::on_control(const ControlEvent&){++controls_;}
SyntheticWirelessCarPlayBackend::SyntheticWirelessCarPlayBackend(SessionCore&c):core_(c){}SyntheticWirelessCarPlayBackend::~SyntheticWirelessCarPlayBackend(){stop();}
void SyntheticWirelessCarPlayBackend::start(){bool expected=false;if(!running_.compare_exchange_strong(expected,true))return;core_.upsert_source(id(),kind(),true);core_.register_control_sink(id(),[this](const ControlEvent&e){on_control(e);});thread_=std::thread(&SyntheticWirelessCarPlayBackend::run,this);}void SyntheticWirelessCarPlayBackend::stop(){bool expected=true;if(!running_.compare_exchange_strong(expected,false))return;if(thread_.joinable())thread_.join();core_.unregister_control_sink(id());core_.upsert_source(id(),kind(),false);}void SyntheticWirelessCarPlayBackend::on_control(const ControlEvent&){++controls_;}
void SyntheticWirelessCarPlayBackend::run(){
  uint64_t n=0;constexpr double pi=3.14159265358979323846;
  // 【栈帧】这是线程入口，线程栈大小完全不可假设（默认 8 MiB，但调用方可以设得更小）。
  // 帧缓冲提到循环外一次分配、每轮复用：既离开栈，也没有每轮的分配开销。
  // submit_video() 按值拷走内容，所以复用同一个缓冲是安全的。
  auto f=std::make_unique<VideoFrame>();
  while(running_){
    f->encoding=VideoEncoding::Svg;f->width=480;f->height=270;f->pts=n*100;f->sequence=++n;
    int w=std::snprintf(f->payload.data(),f->payload.size(),"<svg xmlns='http://www.w3.org/2000/svg' width='480' height='270'><rect width='100%%' height='100%%' fill='#102030'/><text x='24' y='80' fill='#72e0ff' font-size='30'>Synthetic WirelessCarPlay</text><text x='24' y='125' fill='white' font-size='20'>Test input only - no iPhone connected</text><circle cx='%d' cy='190' r='18' fill='#ffb000'/><text x='24' y='245' fill='#cbd5e1' font-size='16'>frame %llu</text></svg>",40+int((n*7)%400),static_cast<unsigned long long>(n));
    f->size=w>0?static_cast<std::size_t>(w):0;
    core_.submit_video(id(),*f);
    AudioChunk a;a.sequence=n;a.sample_rate=8000;a.sample_count=800;
    for(std::size_t i=0;i<a.sample_count;i++)a.samples[i]=static_cast<int16_t>(9000*std::sin(2*pi*440*(n*800+i)/8000.0));
    core_.submit_audio(id(),a);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
} // namespace mvp
