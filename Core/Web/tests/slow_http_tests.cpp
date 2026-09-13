#include "wirelesscarplay/catplay_media_protocol.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace mvp::catplay_media;

int checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<__LINE__<<": " #x "\n";return 1;}}while(false)

constexpr int kRequestedReceiveBuffer=1024;
constexpr int kMaximumEffectiveReceiveBuffer=64*1024;
constexpr auto kResponseDeadline=std::chrono::milliseconds(250);
constexpr auto kRecoveryBound=std::chrono::milliseconds(900);
constexpr auto kDrainDeadline=std::chrono::seconds(10);

std::array<uint8_t,kHeaderBytes> hdr(const Header& h){return encode_header(h);}
bool all(int fd,const void* p,size_t n){auto b=static_cast<const uint8_t*>(p);while(n){auto r=send(fd,b,n,MSG_NOSIGNAL);if(r<=0)return false;b+=r;n-=size_t(r);}return true;}
bool record(int fd,Header h,const std::vector<uint8_t>& p={}){h.payload_bytes=uint32_t(p.size());auto b=hdr(h);return all(fd,b.data(),b.size())&&(p.empty()||all(fd,p.data(),p.size()));}
int connect_to(int port){for(int i=0;i<50;++i){int s=socket(AF_INET,SOCK_STREAM,0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(uint16_t(port));inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);if(connect(s,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0)return s;close(s);std::this_thread::sleep_for(std::chrono::milliseconds(20));}return -1;}
std::string ask(int port,const std::string& q){int s=connect_to(port);if(s<0)return{};if(!all(s,q.data(),q.size())){close(s);return{};}std::string r;char b[1024];int n;while((n=recv(s,b,sizeof b,0))>0)r.append(b,n);close(s);return r;}
bool wait_for(auto f,int ms=1500){for(int i=0;i<ms/10;++i){if(f())return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return f();}

bool drain_to_eof(int fd,std::string& response,std::chrono::steady_clock::time_point deadline){
  char buffer[4096];
  for(;;){
    const auto now=std::chrono::steady_clock::now();
    if(now>=deadline)return false;
    const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();
    pollfd p{fd,short(POLLIN|POLLHUP),0};
    const int ready=poll(&p,1,std::max(1,int(remaining)));
    if(ready<0){if(errno==EINTR)continue;return false;}
    if(ready==0)continue;
    for(;;){
      const auto n=recv(fd,buffer,sizeof(buffer),MSG_DONTWAIT);
      if(n>0){response.append(buffer,std::size_t(n));continue;}
      if(n==0)return true;
      if(errno==EINTR)continue;
      if(errno==EAGAIN||errno==EWOULDBLOCK)break;
      return false;
    }
  }
}

bool content_length(const std::string& response,std::size_t header_end,std::size_t& value){
  if(response.compare(0,9,"HTTP/1.1 ")!=0)return false;
  std::size_t line_end=response.find("\r\n");
  if(line_end==std::string::npos||line_end>=header_end)return false;
  for(std::size_t pos=line_end+2;pos<header_end;){
    line_end=response.find("\r\n",pos);
    if(line_end==std::string::npos||line_end>header_end)return false;
    if(line_end==pos)break;
    constexpr std::string_view name="Content-Length: ";
    if(response.compare(pos,name.size(),name)==0){
      const char* first=response.data()+pos+name.size();
      const char* last=response.data()+line_end;
      const auto parsed=std::from_chars(first,last,value);
      return first!=last&&parsed.ec==std::errc{}&&parsed.ptr==last;
    }
    pos=line_end+2;
  }
  return false;
}

struct Cleanup {
  int& slow;
  int& listener;
  pid_t& server;
  std::atomic<bool>& done;
  std::thread& producer;
  const std::string& path;
  ~Cleanup(){
    if(slow>=0){close(slow);slow=-1;}
    done=true;
    if(listener>=0){shutdown(listener,SHUT_RDWR);close(listener);listener=-1;}
    if(server>0){(void)kill(server,SIGTERM);int status;while(waitpid(server,&status,0)<0&&errno==EINTR){}server=-1;}
    if(producer.joinable())producer.join();
    unlink(path.c_str());
    unsetenv("CP_CATPLAY_MEDIA_SOCKET");
  }
};

int main(int argc,char**argv){
  CHECK(argc==2);
  std::signal(SIGPIPE,SIG_IGN);
  int port=33000+(getpid()%1000);
  std::string path="/tmp/cpmf-slow-"+std::to_string(getpid());
  int listener=socket(AF_UNIX,SOCK_STREAM,0);
  int slow=-1;
  pid_t server=-1;
  CHECK(listener>=0);
  unlink(path.c_str());
  sockaddr_un a{};
  a.sun_family=AF_UNIX;
  std::memcpy(a.sun_path,path.c_str(),path.size()+1);
  CHECK(bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0&&listen(listener,1)==0);
  std::atomic<bool> done{},published{};
  std::thread producer([&]{
    int c=accept(listener,nullptr,nullptr);
    if(c<0)return;
    Header hello{Type::ServerHello,0,0,0,0,0,0,kMaxWirePayloadBytes,1000,7,1};
    std::vector<uint8_t> cfg{0,0,0,1,0x67,0x42,0,0x1e,0,0,0,1,0x68,0xce,6,0xe2};
    std::vector<uint8_t> frame(kMaxVideoFrameBytes-64);
    frame[2]=1;
    frame[3]=0x65;
    bool ok=record(c,hello)&&record(c,{Type::Heartbeat,0,0,0,0,1,0,0,0,0,0})&&record(c,{Type::SessionBegin,0,0,99,0,0,0,0,0,0,0})&&record(c,{Type::VideoStart,0,5,99,0,0,0,0,0,0,0})&&record(c,{Type::VideoConfig,0,5,99,0,0,0,1,640,360,1},cfg)&&record(c,{Type::VideoFrame,kFlagKeyframe,5,99,1,2,0,1,640,360,1},frame);
    published=ok;
    while(!done.load())std::this_thread::sleep_for(std::chrono::milliseconds(10));
    close(c);
  });
  setenv("CP_CATPLAY_MEDIA_SOCKET",path.c_str(),1);
  Cleanup cleanup{slow,listener,server,done,producer,path};
  server=fork();
  CHECK(server>=0);
  if(server==0){
    std::string p=std::to_string(port);
    execl(argv[1],argv[1],"--port",p.c_str(),"--bind","0.0.0.0","--token","slow-secret",nullptr);
    _exit(127);
  }
  auto auth=[](const char* path){return std::string("GET ")+path+" HTTP/1.1\r\nHost: x\r\nX-Auth-Token: slow-secret\r\nConnection: close\r\n\r\n";};
  CHECK(wait_for([&]{auto state=published.load()?ask(port,auth("/api/state")):std::string{};return state.find("\"active\":\"catplay-real\"")!=std::string::npos&&state.find("\"video\":\"active\"")!=std::string::npos;}));
  slow=connect_to(port);
  CHECK(slow>=0);
  CHECK(setsockopt(slow,SOL_SOCKET,SO_RCVBUF,&kRequestedReceiveBuffer,sizeof(kRequestedReceiveBuffer))==0);
  int effective_receive_buffer=0;
  socklen_t effective_receive_buffer_size=sizeof(effective_receive_buffer);
  CHECK(getsockopt(slow,SOL_SOCKET,SO_RCVBUF,&effective_receive_buffer,&effective_receive_buffer_size)==0);
  CHECK(effective_receive_buffer_size==sizeof(effective_receive_buffer));
  CHECK(effective_receive_buffer>=kRequestedReceiveBuffer&&effective_receive_buffer<=kMaximumEffectiveReceiveBuffer);
  auto request=auth("/media/video");
  const auto start=std::chrono::steady_clock::now();
  CHECK(all(slow,request.data(),request.size()));
  std::this_thread::sleep_for(kResponseDeadline+std::chrono::milliseconds(110));
  auto health=ask(port,auth("/health"));
  const auto elapsed=std::chrono::steady_clock::now()-start;
  CHECK(health.find("200 OK")!=std::string::npos);
  CHECK(elapsed<kRecoveryBound);
  const int drain_receive_buffer=kMaximumEffectiveReceiveBuffer;
  CHECK(setsockopt(slow,SOL_SOCKET,SO_RCVBUF,&drain_receive_buffer,sizeof(drain_receive_buffer))==0);
  std::string response;
  CHECK(drain_to_eof(slow,response,std::chrono::steady_clock::now()+kDrainDeadline));
  const auto header_end=response.find("\r\n\r\n");
  CHECK(header_end!=std::string::npos);
  std::size_t advertised_body_bytes=0;
  CHECK(content_length(response,header_end+4,advertised_body_bytes));
  const auto delivered_body_bytes=response.size()-(header_end+4);
  // Loopback stacks may buffer the complete body even when the peer does not
  // read. The portable invariant is bounded handler recovery; if bytes remain,
  // they must still be a valid prefix of the advertised response.
  CHECK(advertised_body_bytes>std::size_t(effective_receive_buffer));
  CHECK(delivered_body_bytes<=advertised_body_bytes);
  close(slow);
  slow=-1;
  done=true;
  CHECK(kill(server,SIGTERM)==0);
  int status=0;
  const auto waited=waitpid(server,&status,0);
  if(waited==server)server=-1;
  CHECK(waited>0&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  producer.join();
  close(listener);
  listener=-1;
  unlink(path.c_str());
  unsetenv("CP_CATPLAY_MEDIA_SOCKET");
  std::cout<<"slow http checks: "<<checks<<'\n';
}
