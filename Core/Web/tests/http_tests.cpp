#include "wirelesscarplay/real_telemetry.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace mvp;
int checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK(" #x ") failed\n"; return 1; } } while (false)
int base_port=25000+(getpid()%10000);
int connect_to(int port){for(int i=0;i<30;i++){int s=socket(AF_INET,SOCK_STREAM,0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(uint16_t(port));inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);if(connect(s,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0)return s;close(s);std::this_thread::sleep_for(std::chrono::milliseconds(30));}return -1;}
std::string ask(int port,const std::vector<std::string>&parts){int s=connect_to(port);if(s<0)return{};for(std::size_t i=0;i<parts.size();i++){if(send(s,parts[i].data(),parts[i].size(),0)!=ssize_t(parts[i].size())){close(s);return{};}if(i+1<parts.size())std::this_thread::sleep_for(std::chrono::milliseconds(40));}shutdown(s,SHUT_WR);std::string r;char b[2048];int n;while((n=recv(s,b,sizeof b,0))>0)r.append(b,n);close(s);return r;}
std::string ask(int p,const std::string&r){return ask(p,std::vector<std::string>{r});}
pid_t start(const char*server,int port,const char*bind=nullptr,const char*token=nullptr){pid_t pid=fork();if(pid<0)return -1;if(pid==0){std::freopen("/dev/null","w",stdout);std::freopen("/dev/null","w",stderr);std::string ps=std::to_string(port);if(bind&&token)execl(server,server,"--port",ps.c_str(),"--bind",bind,"--token",token,nullptr);else{chdir("/");execl(server,server,"--port",ps.c_str(),nullptr);}_exit(127);}return pid;}
bool stop(pid_t p){if(p<0||kill(p,SIGTERM)!=0)return false;int s=0;return waitpid(p,&s,0)==p&&WIFEXITED(s)&&WEXITSTATUS(s)==0;}
int sidecar_listener(const std::string&path){int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return -1;unlink(path.c_str());sockaddr_un addr{};addr.sun_family=AF_UNIX;std::memcpy(addr.sun_path,path.c_str(),path.size()+1);if(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))||listen(fd,1)){close(fd);return -1;}return fd;}
std::string telemetry(bool connected=false,const char*main="not-negotiated",const char*media="not-negotiated"){std::string features="{\"bluetooth\":\"unsupported\",\"wifi_bonjour\":\"not-negotiated\",\"iap2\":\"not-negotiated\",\"mfi\":\"not-negotiated\",\"hap\":\"not-negotiated\",\"rtsp\":\"not-negotiated\",\"wired_usb\":\"not-negotiated\",\"main_screen\":\""+std::string(main)+"\",\"second_screen\":\"not-offered\",\"instrument_screen\":\"not-offered\",\"media_audio\":\""+std::string(media)+"\",\"call_audio\":\"not-offered\",\"siri\":\"not-offered\",\"prompt_audio\":\"not-offered\",\"microphone\":\"not-offered\",\"hid\":\"not-negotiated\",\"now_playing\":\"not-offered\",\"lyrics\":\"not-offered\",\"gps\":\"not-offered\",\"vehicle\":\"not-offered\",\"navigation\":\"not-offered\",\"phone\":\"not-offered\"}";return "{\"id\":1,\"ok\":true,\"payload\":{\"schema_version\":1,\"session_id\":7,\"lifecycle\":\"initial\",\"wireless_listener_ready\":false,\"wired_endpoint_ready\":false,\"iphone_connected\":false,\"vehicle_connected\":false,\"bridge_active\":false,\"connected\":"+std::string(connected?"true":"false")+",\"features\":"+features+",\"last_error\":\"\"}}";}
int main(int argc,char**argv){std::signal(SIGPIPE,SIG_IGN);int sentinel=0;CHECK(++sentinel==1);CHECK(argc==2);CpNativeTelemetryStatus parsed;std::string defaults=telemetry();CHECK(parse_cp_native_telemetry_line(defaults,parsed));CHECK(parsed.available&&!parsed.connected&&!parsed.negotiated&&!parsed.ready&&parsed.second_screen==CpOfferState::NotOffered);CHECK(parse_cp_native_telemetry_line(telemetry(true,"negotiating"),parsed));CHECK(parsed.connected&&parsed.negotiated&&!parsed.ready);CHECK(parse_cp_native_telemetry_line(telemetry(true,"active","active"),parsed));CHECK(parsed.ready);CHECK(parse_cp_native_telemetry_line(defaults,parsed));CHECK(!parsed.connected&&!parsed.negotiated&&!parsed.ready);auto replaced=[](std::string text,const std::string&from,const std::string&to){auto pos=text.find(from);if(pos!=std::string::npos)text.replace(pos,from.size(),to);return text;};CHECK(!parse_cp_native_telemetry_line(replaced(telemetry(),"\"id\":1","\"id\":2"),parsed));CHECK(!parse_cp_native_telemetry_line(replaced(telemetry(),"\"schema_version\":1","\"schema_version\":2"),parsed));CHECK(!parse_cp_native_telemetry_line(replaced(telemetry(),"\"second_screen\":\"not-offered\"","\"second_screen\":\"bogus\""),parsed));CHECK(!parse_cp_native_telemetry_line(telemetry()+"{}",parsed));std::string truncated=telemetry();truncated.pop_back();CHECK(!parse_cp_native_telemetry_line(truncated,parsed));auto malformed_number=[](std::string number){auto text=telemetry();text.insert(text.size()-1,",\"extra\":"+number);return text;};CHECK(!parse_cp_native_telemetry_line(malformed_number("- 1"),parsed));CHECK(!parse_cp_native_telemetry_line(malformed_number("01"),parsed));CHECK(!parse_cp_native_telemetry_line(malformed_number("1."),parsed));CHECK(!parse_cp_native_telemetry_line(malformed_number("1e+ 2"),parsed));CHECK(!parse_cp_native_telemetry_line(std::string(kCpNativeTelemetryReplyCap+1,'x'),parsed));pid_t reject=fork();CHECK(reject>=0);if(reject==0){execl(argv[1],argv[1],"--bind","0.0.0.0",nullptr);_exit(127);}int rs=0;CHECK(waitpid(reject,&rs,0)==reject&&WIFEXITED(rs)&&WEXITSTATUS(rs)==2);pid_t invalid=fork();CHECK(invalid>=0);if(invalid==0){execl(argv[1],argv[1],"--port","0",nullptr);_exit(127);}CHECK(waitpid(invalid,&rs,0)==invalid&&WIFEXITED(rs)&&WEXITSTATUS(rs)==2);std::string sidecar_path="/tmp/cp-telemetry-"+std::to_string(getpid());int sidecar=sidecar_listener(sidecar_path);CHECK(sidecar>=0);std::thread responder([&]{int client=accept(sidecar,nullptr,nullptr);if(client>=0){char request[128];(void)recv(client,request,sizeof(request),0);auto reply=telemetry(true,"active","active")+char(10);(void)send(client,reply.data(),reply.size(),0);close(client);}close(sidecar);});setenv("CP_CORE_SOCKET",sidecar_path.c_str(),1);pid_t live=start(argv[1],base_port+20);CHECK(live>0);auto live_request=std::string("GET /api/state HTTP/1.1")+char(13)+char(10)+"Host: x"+char(13)+char(10)+char(13)+char(10);auto live_state=ask(base_port+20,live_request);CHECK(live_state.find("\"realCarPlayTelemetryAvailable\":true")!=std::string::npos);CHECK(live_state.find("\"status\":\"ready\"")!=std::string::npos);CHECK(live_state.find("\"secondScreen\":\"not-offered\"")!=std::string::npos);CHECK(live_state.find("\"synthetic\":true")!=std::string::npos);auto live_body=live_state.substr(live_state.find("\r\n\r\n")+4);detail::JsonReader live_json(live_body);CHECK(live_json.skip()&&live_json.done());CHECK(stop(live));responder.join();unsetenv("CP_CORE_SOCKET");unlink(sidecar_path.c_str()); int port=base_port;std::string display_path="/tmp/http-display-config-"+std::to_string(getpid());unlink(display_path.c_str());setenv("CP_DISPLAY_CONFIG",display_path.c_str(),1);pid_t p=start(argv[1],port);CHECK(p>0);auto get=[&](const char*path){return ask(port,std::string("GET ")+path+" HTTP/1.1\r\nHost: x\r\n\r\n");};CHECK(get("/health").find("200 OK")!=std::string::npos);auto page=get("/");CHECK(page.find("WirelessCarPlay real media")!=std::string::npos);CHECK(page.find("Synthetic/local SVG and WAV test media is separate")!=std::string::npos);CHECK(page.find("Live video unsupported: this browser/context does not expose WebCodecs.")!=std::string::npos);CHECK(page.find("videos={main:")!=std::string::npos);CHECK(page.find("n.onended=")!=std::string::npos);CHECK(page.find("X-Media-After")!=std::string::npos);CHECK(page.find("m.screens||")!=std::string::npos);CHECK(page.find("q.generation!==a.generation")!=std::string::npos);CHECK(page.find("Next CarPlay session display offer")!=std::string::npos);CHECK(page.find("AltScreen")!=std::string::npos);CHECK(page.find("canvas.onpointerdown")!=std::string::npos);CHECK(page.find("mainStats")!=std::string::npos);auto config_default=get("/api/display-config");CHECK(config_default.find("200 OK")!=std::string::npos&&config_default.find("\"width\":1920")!=std::string::npos&&config_default.find("\"enabled\":false")!=std::string::npos);std::string display_body="main_width=1280&main_height=720&main_fps=50&alt_enabled=1&alt_width=800&alt_height=480&alt_fps=25";CHECK(ask(port,"POST /api/display-config HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(display_body.size())+"\r\n\r\n"+display_body).find("200 OK")!=std::string::npos);CHECK(get("/api/display-config").find("\"fps\":25")!=std::string::npos);std::string bad_display="main_width=1279&main_height=720&main_fps=50&alt_enabled=1&alt_width=800&alt_height=480&alt_fps=25";CHECK(ask(port,"POST /api/display-config HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(bad_display.size())+"\r\n\r\n"+bad_display).find("400 Bad Request")!=std::string::npos);CHECK(get("/api/state").find("\"active\":\"synthetic-wireless\"")!=std::string::npos);CHECK(get("/api/state").find("\"realCarPlayStatus\":{\"engine\":\"unavailable\"")!=std::string::npos);CHECK(get("/api/state").find("\"realCarPlayTelemetry\":null")!=std::string::npos);CHECK(get("/media/frame.svg").find("<svg")!=std::string::npos);CHECK(get("/media/audio.wav").find("RIFF")!=std::string::npos);
 std::string manual="mode=manual&source=synthetic-wireless";CHECK(ask(port,"POST /api/select HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(manual.size())+"\r\n\r\n"+manual).find("200 OK")!=std::string::npos);std::string off="enabled=0";CHECK(ask(port,{"POST /api/test-mode HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(off.size())+"\r\n\r\nen", "abled=0"}).find("200 OK")!=std::string::npos);// 关掉 synthetic 后应回落到【Core 自己的桌面】。历史上这里断言的是 "local-desktop"，
// 但 main.cpp 后来有意不再调用 local.start()：LocalDesktopBackend 会以 "local-desktop"
// 再注册一个内置桌面，与 "core-desktop" 重复（管理页会冒出两个），且它只产出 41 字节空帧。
// 实测（默认 ulimit -s 8192、服务启动后 POST /api/test-mode enabled=0）：
//   active=core-desktop；sources 只有 synthetic-wireless(False) + core-desktop(True)；
//   local-desktop 不存在；/media/frame.svg 是 DesktopRenderer 的真实 SVG（>500 字节，含 <text>）。
// 断言改为“回落后的桌面确实渲染出了内容”—— 这才是原断言的意图（防止回落到空壳），
// 同时不再钉死具体文案，避免下次改文案又误报。
CHECK(get("/api/state").find("\"active\":\"core-desktop\"")!=std::string::npos);
// 等待说明：DesktopRenderer 以 1 Hz 周期刷新（DesktopRenderer::refresh()），且只有“刚成为
// 活跃面”时才无条件提交一帧，所以切换活动源后要给它一个刷新周期。
// 实测证据（默认 ulimit -s 8192，POST select→test-mode 后立刻 GET）：
//   bytes=41   <svg xmlns='http://www.w3.org/2000/svg'/>   （空帧）
//   ≤ 3s 后    bytes=1874 含 <text>                        （真内容）
// 这里只容忍【刷新时机】；**内容断言没有放宽** —— 仍要求真实 SVG（含 <text>、>500 字节）。
// 已另报给 DesktopRenderer 的负责方：成为活跃面时应同步提交一帧，而不是等下一个周期；
// 在那之前不建议从 HTTP worker 线程直接调 refresh()，因为它与 1 Hz tick 线程并发写
// 非原子的 published_in_epoch_/last_signature_，会引入数据竞争。
std::string fallback;for(int attempt=0;attempt<12;++attempt){fallback=get("/media/frame.svg");if(fallback.find("<text")!=std::string::npos)break;usleep(250000);}CHECK(fallback.find("<svg xmlns=")!=std::string::npos&&fallback.find("<text")!=std::string::npos&&fallback.size()>500);std::string on="enabled=1";CHECK(ask(port,"POST /api/test-mode HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(on.size())+"\r\n\r\n"+on).find("200 OK")!=std::string::npos);CHECK(get("/api/state").find("\"active\":\"synthetic-wireless\"")!=std::string::npos);CHECK(get("/api/state").find("\"realCarPlayStatus\":{\"engine\":\"unavailable\"")!=std::string::npos);CHECK(get("/api/state").find("\"realCarPlayTelemetry\":null")!=std::string::npos);
// Exercise the HTTP -> strict parser -> Core routing boundary for all nine types.
// 200 here means routed to the selected synthetic test source, not phone execution.
for (const std::string body : {
    "type=touch&phase=up&x=10&y=20",
    "type=key&key=next",
    "type=multitouch&phase=down&points=0,10,20,down;1,30,40,move",
    "type=knob&direction=left&steps=3",
    "type=gesture&gesture=back",
    "type=proximity&state=1",
    "type=voice&action=up",
    "type=telephony&action=dtmf&dtmf=12%2A%23",
    "type=vehicle&control=night_mode&value=1"}) {
  CHECK(ask(port,"POST /api/control HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body).find("200 OK")!=std::string::npos);
}
for (const std::string body : {
    "type=voice&action=up&type=key",
    "type=voice&action=down&typo=1",
    "type=key&key=%00",
    "type=knob&direction=left&steps=9999999999999999",
    "type=telephony&action=dtmf&dtmf=12x",
    "type=multitouch&phase=down&points=0,1,2,down;0,3,4,down"}) {
  CHECK(ask(port,"POST /api/control HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body).find("400 Bad Request")!=std::string::npos);
}
 std::string touch="type=touch&phase=down&x=10&y=20";CHECK(ask(port,"POST /api/control HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(touch.size())+"\r\n\r\n"+touch).find("200 OK")!=std::string::npos);std::string bad_touch="type=touch&phase=drag&x=10&y=20";CHECK(ask(port,"POST /api/control HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(bad_touch.size())+"\r\n\r\n"+bad_touch).find("400 Bad Request")!=std::string::npos);CHECK(ask(port,"POST /api/control HTTP/1.1\r\nHost: x\r\nContent-Length: 9\r\nContent-Length: 9\r\n\r\ntype=key").find("400 Bad Request")!=std::string::npos);CHECK(ask(port,"POST /api/control HTTP/1.1\r\nHost: x\r\nContent-Length: 9\r\n\r\ntype=key").find("400 Bad Request")!=std::string::npos);CHECK(stop(p));unlink(display_path.c_str());unsetenv("CP_DISPLAY_CONFIG");
 int token_port=port+1;pid_t secure=start(argv[1],token_port,"0.0.0.0","secret");CHECK(secure>0);std::string health="GET /health HTTP/1.1\r\nHost: x\r\n\r\n";CHECK(ask(token_port,"GET / HTTP/1.1\r\nHost: x\r\n\r\n").find("200 OK")!=std::string::npos);CHECK(ask(token_port,health).find("401 Unauthorized")!=std::string::npos);CHECK(ask(token_port,"GET /health HTTP/1.1\r\nHost: x\r\nX-Auth-Token: wrong\r\n\r\n").find("401 Unauthorized")!=std::string::npos);std::string injected="type=key&key=X-Auth-Token: secret";CHECK(ask(token_port,"POST /api/control HTTP/1.1\r\nHost: x\r\nContent-Length: "+std::to_string(injected.size())+"\r\n\r\n"+injected).find("401 Unauthorized")!=std::string::npos);CHECK(ask(token_port,"GET /health HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer secret\r\n\r\n").find("200 OK")!=std::string::npos);CHECK(ask(token_port,"GET /media/video HTTP/1.1\r\nHost: x\r\n\r\n").find("401 Unauthorized")!=std::string::npos);CHECK(ask(token_port,"GET /media/video HTTP/1.1\r\nHost: x\r\nX-Auth-Token: secret\r\n\r\n").find("409 Conflict")!=std::string::npos);CHECK(ask(token_port,"GET /media/video/alt HTTP/1.1\r\nHost: x\r\nX-Auth-Token: secret\r\n\r\n").find("409 Conflict")!=std::string::npos);// 逃生阀 WEB_OPEN_API=1：显式关闭令牌校验（仅测试部署）。
// 两条路径都必须有覆盖 —— 否则安全语义会退化成“只靠一行日志提醒”：
//   ・不设 WEB_OPEN_API 且传了 --token → 必须 401（上面 secure 实例那几条）
//   ・设了 WEB_OPEN_API=1 且传了 --token → 免令牌可访问（下面这个实例）
// 这样“忽略校验”是一个被测试钉住的显式行为，而不是不可见的环境差异。
setenv("WEB_OPEN_API","1",1);
int open_port=token_port+1;pid_t open_srv=start(argv[1],open_port,"0.0.0.0","secret");CHECK(open_srv>0);
CHECK(ask(open_port,"GET /health HTTP/1.1\r\nHost: x\r\n\r\n").find("200 OK")!=std::string::npos);
CHECK(ask(open_port,"GET /api/state HTTP/1.1\r\nHost: x\r\n\r\n").find("200 OK")!=std::string::npos);
CHECK(ask(open_port,"GET /api/state HTTP/1.1\r\nHost: x\r\nX-Auth-Token: wrong\r\n\r\n").find("200 OK")!=std::string::npos);
CHECK(stop(open_srv));unsetenv("WEB_OPEN_API");
CHECK(stop(secure));CHECK(checks>20);std::cout<<"http checks executed: "<<checks<<'\n';return 0;}
