#include "wirelesscarplay/wireless_carplay.hpp"
#include "wirelesscarplay/real_telemetry.hpp"
#include "wirelesscarplay/catplay_media_client.hpp"
#include "core/desktop.hpp"
#include "wirelesscarplay/display_config.hpp"
#include "wirelesscarplay/control_request.hpp"
#include "wirelesscarplay/real_media_store.hpp"
#ifdef ZERO2W_WITH_CARLIFE
#include "carlife/carlife_input.h"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <execinfo.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
const char kPage[] = R"HTML(<!doctype html><meta charset="utf-8"><title>WirelessCarPlay real media</title><style>body{font:16px system-ui;background:#101820;color:#e5edf5;max-width:1100px;margin:2rem auto}canvas{width:100%;background:#000;touch-action:none;cursor:pointer}.screens{display:grid;grid-template-columns:repeat(auto-fit,minmax(360px,1fr));gap:1rem}fieldset{border:1px solid #456;padding:1rem;margin:1rem 0}label{display:inline-block;margin:.25rem}button,input{margin:.3rem;padding:.5rem}pre,#status{background:#182530;padding:1rem;white-space:pre-wrap}small{color:#fbbf24}</style><h1>WirelessCarPlay real media</h1><small>Synthetic/local SVG and WAV test media is separate and is never presented as iPhone media.</small><fieldset><legend>Next CarPlay session display offer</legend><strong>Main</strong><label>width <input id="mw" type="number" min="320" max="2560" step="2"></label><label>height <input id="mh" type="number" min="240" max="1440" step="2"></label><label>fps <input id="mf" type="number" min="1" max="60"></label><br><strong>AltScreen</strong><label><input id="ae" type="checkbox"> enabled</label><label>width <input id="aw" type="number" min="320" max="2560" step="2"></label><label>height <input id="ah" type="number" min="240" max="1440" step="2"></label><label>fps <input id="af" type="number" min="1" max="60"></label><button id="saveDisplays">Save for next session</button><div id="configStatus">Authenticate to load display settings.</div></fieldset><p>Token: <input id="token" type="password"><button id="use">Use token</button><button id="audio">Enable live audio</button></p><div id="status">Live media unavailable</div><div class="screens"><section><h2>Main screen <small id="mainStats">0 fps</small></h2><canvas id="video" width="800" height="480"></canvas><div>Touch/click and drag controls the negotiated main display.</div></section><section><h2>AltScreen <small id="altStats">0 fps</small></h2><canvas id="altVideo" width="800" height="480"></canvas><div>Waiting for a genuinely negotiated AltScreen stream.</div></section></div><pre id="state"></pre><script>let token='',audio,astream={},agen={},audioStreaming=false,audioAbort=null,audioAvailable=false,audioWire={pending:new Uint8Array(0)},audioDuckGain=1,msid=0;const status=document.querySelector('#status'),canvas=document.querySelector('#video'),ctx=canvas.getContext('2d'),altCanvas=document.querySelector('#altVideo'),altCtx=altCanvas.getContext('2d'),state=document.querySelector('#state'),configStatus=document.querySelector('#configStatus'),mw=document.querySelector('#mw'),mh=document.querySelector('#mh'),mf=document.querySelector('#mf'),ae=document.querySelector('#ae'),aw=document.querySelector('#aw'),ah=document.querySelector('#ah'),af=document.querySelector('#af'),videos={main:{decoder:null,codec:'',busy:false,seq:0,generation:0,stateGeneration:0,stream:0,keyframeAt:0,timer:null,streaming:false,abort:null,pending:new Uint8Array(0),wireConfig:null,needKey:true,rendered:0,fpsAt:performance.now(),stat:document.querySelector('#mainStats'),canvas,ctx},alt:{decoder:null,codec:'',busy:false,seq:0,generation:0,stateGeneration:0,stream:0,keyframeAt:0,timer:null,streaming:false,abort:null,pending:new Uint8Array(0),wireConfig:null,needKey:true,rendered:0,fpsAt:performance.now(),stat:document.querySelector('#altStats'),canvas:altCanvas,ctx:altCtx}};const headers=()=>token?{'X-Auth-Token':token}:{};function u16(d,o){return d.getUint16(o)}function u32(d,o){return d.getUint32(o)}function u64(d,o){return Number(d.getBigUint64(o))}function records(b){let a=[],d=new DataView(b),o=0;while(o+64<=b.byteLength){if(d.getUint32(o)!==0x43504d46||d.getUint8(o+4)!==1||d.getUint16(o+6)!==64)throw Error('bad media record');let n=u32(d,o+40);if(n>1048576||o+64+n>b.byteLength)throw Error('truncated media record');a.push({t:u16(d,o+8),flags:u16(d,o+10),stream:u32(d,o+12),seq:u64(d,o+24),pts:u64(d,o+32),p0:u32(d,o+44),p1:u32(d,o+48),p2:u32(d,o+52),p3:u32(d,o+56),data:new Uint8Array(b,o+64,n)});o+=64+n}if(o!==b.byteLength)throw Error('trailing media bytes');return a}function avc(config){for(let i=0;i+7<config.length;i++)if(config[i]===0&&config[i+1]===0&&(config[i+2]===1||config[i+2]===0&&config[i+3]===1)){let j=config[i+2]===1?i+3:i+4;if((config[j]&31)===7&&j+3<config.length)return 'avc1.'+[config[j+1],config[j+2],config[j+3]].map(x=>x.toString(16).padStart(2,'0')).join('').toUpperCase()}throw Error('missing SPS')}function setup(role,c,w,h,g,stream){let v=videos[role],name=avc(c);let changed=!v.decoder||v.codec!==name||v.codedWidth!==w||v.codedHeight!==h;v.generation=g;if(changed){if(v.decoder)v.decoder.close();v.codec=name;v.codedWidth=w;v.codedHeight=h;v.decoder=new VideoDecoder({output:f=>{v.canvas.width=f.codedWidth;v.canvas.height=f.codedHeight;v.ctx.drawImage(f,0,0);f.close();v.rendered++;let now=performance.now();if(now-v.fpsAt>=1000){v.stat.textContent=(v.rendered*1000/(now-v.fpsAt)).toFixed(1)+' fps';v.rendered=0;v.fpsAt=now}},error:e=>{if(v.decoder)v.decoder.close();v.decoder=null;v.seq=0;v.generation=0;status.textContent=role+' decoder error: '+e.message;fetch('/api/media/keyframe',{method:'POST',headers:{...headers(),'Content-Type':'application/x-www-form-urlencoded'},body:'stream_id='+stream}).catch(()=>{})}});try{v.decoder.configure({codec:name,codedWidth:w,codedHeight:h,optimizeForLatency:true})}catch(e){v.decoder.close();v.decoder=null;status.textContent=role+' video unsupported: '+e.message}}}function requestKey(v,stream){if(stream&&Date.now()-v.keyframeAt>=500){v.keyframeAt=Date.now();fetch('/api/media/keyframe',{method:'POST',headers:{...headers(),'Content-Type':'application/x-www-form-urlencoded'},body:'stream_id='+stream}).catch(()=>{})}}function consumeVideo(role,rs){let v=videos[role];for(let r of rs)if(r.t===17){v.wireConfig=r;setup(role,r.data,r.p1,r.p2,r.p3,r.stream)}for(let f of rs)if(f.t===18){let key=!!(f.flags&1);if(!key&&v.seq&&f.seq!==v.seq+1){v.needKey=true;requestKey(v,f.stream)}if(v.needKey&&!key)continue;if(key)v.needKey=false;if(f.seq<=v.seq)continue;let c=v.wireConfig,data=key&&c?new Uint8Array(c.data.length+f.data.length):f.data;if(key&&c){data.set(c.data);data.set(f.data,c.data.length)}if(v.decoder&&v.decoder.state==='configured'){if(v.decoder.decodeQueueSize>24){v.decoder.close();v.decoder=null;v.seq=0;v.needKey=true;requestKey(v,f.stream);return}v.decoder.decode(new EncodedVideoChunk({type:key?'key':'delta',timestamp:f.pts,data}));v.seq=f.seq}}}function streamRecords(v,chunk){let all=new Uint8Array(v.pending.length+chunk.length);all.set(v.pending);all.set(chunk,v.pending.length);if(all.length>2200000)throw Error('stream buffer overflow');let rs=[],d=new DataView(all.buffer),o=0;while(o+64<=all.length){if(d.getUint32(o)!==0x43504d46||d.getUint8(o+4)!==1||d.getUint16(o+6)!==64)throw Error('bad stream record');let n=d.getUint32(o+40);if(n>1048576)throw Error('oversize stream record');if(o+64+n>all.length)break;rs.push({t:d.getUint16(o+8),flags:d.getUint16(o+10),stream:d.getUint32(o+12),seq:Number(d.getBigUint64(o+24)),pts:Number(d.getBigUint64(o+32)),p0:d.getUint32(o+44),p1:d.getUint32(o+48),p2:d.getUint32(o+52),p3:d.getUint32(o+56),data:new Uint8Array(all.buffer,o+64,n)});o+=64+n}v.pending=all.slice(o);return rs}async function startVideoStream(role){let v=videos[role];if(v.streaming||!v.stream)return;v.streaming=true;v.abort=new AbortController();v.pending=new Uint8Array(0);v.needKey=true;try{let r=await fetch('/media/video/'+role+'/stream',{headers:headers(),signal:v.abort.signal});if(!r.ok)throw Error(r.status);let reader=r.body.getReader();for(;;){let part=await reader.read();if(part.done)break;consumeVideo(role,streamRecords(v,part.value))}}catch(e){if(e.name!=='AbortError')status.textContent=role+' stream unavailable: '+e.message}finally{v.streaming=false;v.abort=null;if(v.stream)setTimeout(()=>startVideoStream(role),250)}}function scheduleVideo(role,delay=25){let v=videos[role];if(v.timer)return;v.timer=setTimeout(()=>{v.timer=null;pollVideo(role)},delay)}async function pollVideo(role){let v=videos[role];if(v.busy)return;v.busy=true;try{let r=await fetch('/media/video/'+role,{headers:{...headers(),'X-Media-After':String(v.seq)}});if(r.status===204){if(!v.seq&&v.stream&&Date.now()-v.keyframeAt>=500){v.keyframeAt=Date.now();fetch('/api/media/keyframe',{method:'POST',headers:{...headers(),'Content-Type':'application/x-www-form-urlencoded'},body:'stream_id='+v.stream}).catch(()=>{})}return}if(!r.ok)throw Error(r.status);let rs=records(await r.arrayBuffer()),c=rs.find(x=>x.t===17),fs=rs.filter(x=>x.t===18);if(fs.length){if(c)setup(role,c.data,c.p1,c.p2,c.p3,fs[0].stream);for(let f of fs){if(!(f.flags&1)&&v.seq&&f.seq!==v.seq+1){v.seq=0;fetch('/api/media/keyframe',{method:'POST',headers:{...headers(),'Content-Type':'application/x-www-form-urlencoded'},body:'stream_id='+f.stream}).catch(()=>{});return}if(f.seq<=v.seq)continue;let data=c&&(f.flags&1)?new Uint8Array(c.data.length+f.data.length):f.data;if(c&&(f.flags&1)){data.set(c.data);data.set(f.data,c.data.length)}if(v.decoder&&v.decoder.state==='configured'&&v.decoder.decodeQueueSize<=2){v.decoder.decode(new EncodedVideoChunk({type:f.flags&1?'key':'delta',timestamp:f.pts,data}));v.seq=f.seq}}}}catch(e){status.textContent=role+' video unavailable: '+e.message}finally{v.busy=false;if(v.stream)scheduleVideo(role)}}function applyDuck(gain,duration){audioDuckGain=Math.max(0,Math.min(1,gain));let now=audio.currentTime,end=now+Math.max(0,Math.min(60,duration/1000));for(let q of Object.values(astream))if(q.audioType===2&&q.gain){let p=q.gain.gain;p.cancelScheduledValues(now);p.setValueAtTime(p.value,now);p.linearRampToValueAtTime(audioDuckGain,end)}}function consumeAudio(rs){for(let c of rs.filter(x=>x.t===35))applyDuck(c.p1/1000000,c.p0);for(let p of rs.filter(x=>x.t===33)){let s=rs.find(x=>x.t===32&&x.stream===p.stream),q=astream[p.stream];if(!s)continue;if(!q){let gain=audio.createGain();gain.gain.value=s.p0===2?audioDuckGain:1;gain.connect(audio.destination);q={seq:0,next:audio.currentTime+.12,nodes:[],generation:agen[p.stream]||0,gain,audioType:s.p0}}if(p.seq<=q.seq)continue;if((p.flags&2)||(q.seq&&p.seq!==q.seq+1)||q.next<audio.currentTime||q.next>audio.currentTime+.45||q.nodes.length>=64){for(let old of q.nodes)old.stop();q.nodes=[];q.next=audio.currentTime+.12}let b=audio.createBuffer(s.p2,p.p3,s.p1),view=new DataView(p.data.buffer,p.data.byteOffset,p.data.byteLength);for(let ch=0;ch<s.p2;ch++){let out=b.getChannelData(ch);for(let i=0;i<p.p3;i++)out[i]=view.getInt16((i*s.p2+ch)*2,true)/32768}let n=audio.createBufferSource();n.buffer=b;n.connect(q.gain);n.onended=()=>{let i=q.nodes.indexOf(n);if(i>=0)q.nodes.splice(i,1)};n.start(q.next);q.nodes.push(n);q.next+=b.duration;q.seq=p.seq;q.audioType=s.p0;q.generation=agen[p.stream]||q.generation;astream[p.stream]=q}}async function startAudioStream(){if(audioStreaming||!audio||!audioAvailable)return;audioStreaming=true;audioAbort=new AbortController();audioWire.pending=new Uint8Array(0);try{let r=await fetch('/media/audio/stream',{headers:headers(),signal:audioAbort.signal});if(!r.ok)throw Error(r.status);let reader=r.body.getReader();for(;;){let part=await reader.read();if(part.done)break;consumeAudio(streamRecords(audioWire,part.value))}}catch(e){if(e.name!=='AbortError')status.textContent='Live audio stream unavailable: '+e.message}finally{audioStreaming=false;audioAbort=null;if(audioAvailable)setTimeout(startAudioStream,100)}}let touchPointer=null,touchMove=null,touchRaf=0,touchBusy=false,touchQueue=[];function touchPoint(e){let r=canvas.getBoundingClientRect();return{x:Math.max(0,Math.min(canvas.width-1,Math.floor((e.clientX-r.left)*canvas.width/r.width))),y:Math.max(0,Math.min(canvas.height-1,Math.floor((e.clientY-r.top)*canvas.height/r.height)))}}async function pumpTouch(){if(touchBusy)return;touchBusy=true;try{while(touchQueue.length){let t=touchQueue.shift(),body=new URLSearchParams({type:'touch',phase:t.phase,x:String(t.x),y:String(t.y)}),r=await fetch('/api/control',{method:'POST',headers:{...headers(),'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)throw Error('touch '+r.status)}}catch(e){touchQueue=[];status.textContent='Touch unavailable: '+e.message}finally{touchBusy=false}}function sendTouch(phase,e){if(!videos.main.stream)return;let p=touchPoint(e),t={phase,x:p.x,y:p.y},last=touchQueue[touchQueue.length-1];if(phase==='move'&&last&&last.phase==='move')touchQueue[touchQueue.length-1]=t;else if(touchQueue.length<8)touchQueue.push(t);else if(phase!=='move')touchQueue=[t];pumpTouch()}canvas.onpointerdown=e=>{if(touchPointer!==null)return;touchPointer=e.pointerId;canvas.setPointerCapture(e.pointerId);sendTouch('down',e);e.preventDefault()};canvas.onpointermove=e=>{if(e.pointerId!==touchPointer)return;touchMove=e;if(!touchRaf)touchRaf=requestAnimationFrame(()=>{touchRaf=0;if(touchMove){sendTouch('move',touchMove);touchMove=null}});e.preventDefault()};function endTouch(e){if(e.pointerId!==touchPointer)return;touchMove=null;sendTouch('up',e);touchPointer=null;e.preventDefault()}canvas.onpointerup=endTouch;canvas.onpointercancel=endTouch;canvas.oncontextmenu=e=>e.preventDefault();async function refresh(){try{let r=await fetch('/api/state',{headers:headers()}),j=await r.json();state.textContent=JSON.stringify(j,null,2);let m=j.realCarPlayMedia,screens=m.screens||[{role:'main',streamId:m.videoStreamId,configGeneration:m.videoConfigGeneration,state:m.video},{role:'alt',streamId:0,configGeneration:0,state:'inactive'}];if(msid&&msid!==m.sessionId){for(let v of Object.values(videos)){if(v.decoder)v.decoder.close();if(v.timer)clearTimeout(v.timer);if(v.abort)v.abort.abort();v.decoder=null;v.timer=null;v.seq=0;v.generation=0;v.stateGeneration=0;v.pending=new Uint8Array(0);v.wireConfig=null;v.needKey=true;v.rendered=0;v.fpsAt=performance.now();v.stat.textContent='0 fps'}if(audioAbort)audioAbort.abort();audioWire.pending=new Uint8Array(0);for(let q of Object.values(astream)){for(let n of q.nodes)n.stop();if(q.gain)q.gain.disconnect()}astream={};audioDuckGain=1}msid=m.sessionId;for(let role of ['main','alt']){let v=videos[role],screen=screens.find(x=>x.role===role)||{streamId:0,configGeneration:0,state:'inactive'};v.stream=screen.streamId;if(v.stateGeneration&&v.stateGeneration!==screen.configGeneration){if(v.decoder)v.decoder.close();if(v.abort)v.abort.abort();v.decoder=null;v.seq=0;v.generation=0;v.pending=new Uint8Array(0);v.wireConfig=null;v.needKey=true;if(screen.streamId)fetch('/api/media/keyframe',{method:'POST',headers:{...headers(),'Content-Type':'application/x-www-form-urlencoded'},body:'stream_id='+screen.streamId}).catch(()=>{})}v.stateGeneration=screen.configGeneration}for(let a of m.audio){agen[a.streamId]=a.generation;let q=astream[a.streamId];if(q&&q.generation!==a.generation){for(let n of q.nodes)n.stop();if(q.gain)q.gain.disconnect();delete astream[a.streamId]}}if(j.active!=='catplay-real'){status.textContent='Live CarPlay unavailable: catplay-real is not selected';return}if(!isSecureContext||!window.VideoDecoder||!window.EncodedVideoChunk){status.textContent='Live video unsupported: this browser/context does not expose WebCodecs.';return}let active=screens.filter(x=>x.state==='active').map(x=>x.role);if(!active.length){status.textContent='Live video waiting: main='+(screens[0]?.state||'inactive')+', alt='+(screens[1]?.state||'inactive');return}status.textContent=(audio?'Live video/audio receiving: ':'Live video receiving: ')+active.join(', ')+(audio?'':' (enable audio from a user gesture)');for(let role of active)startVideoStream(role);audioAvailable=m.audio.length>0;if(audioAvailable)startAudioStream();else if(audioAbort)audioAbort.abort()}catch(e){status.textContent='Live media unavailable: '+e.message}}async function loadDisplays(){try{let r=await fetch('/api/display-config',{headers:headers()});if(!r.ok)throw Error(r.status);let c=await r.json();mw.value=c.main.width;mh.value=c.main.height;mf.value=c.main.fps;ae.checked=c.alt.enabled;aw.value=c.alt.width;ah.value=c.alt.height;af.value=c.alt.fps;configStatus.textContent='Loaded. Changes apply to the next CarPlay session.'}catch(e){configStatus.textContent='Display config unavailable: '+e.message}}document.querySelector('#saveDisplays').onclick=async()=>{let body=new URLSearchParams({main_width:mw.value,main_height:mh.value,main_fps:mf.value,alt_enabled:ae.checked?'1':'0',alt_width:aw.value,alt_height:ah.value,alt_fps:af.value});try{let r=await fetch('/api/display-config',{method:'POST',headers:{...headers(),'Content-Type':'application/x-www-form-urlencoded'},body});let j=await r.json();if(!r.ok)throw Error(j.error||r.status);configStatus.textContent='Saved. Reconnect CarPlay to advertise this offer.'}catch(e){configStatus.textContent='Display config rejected: '+e.message}};document.querySelector('#use').onclick=()=>{token=document.querySelector('#token').value;loadDisplays();refresh()};document.querySelector('#audio').onclick=async()=>{if(!window.AudioContext){status.textContent='Live audio unsupported: Web Audio is unavailable';return}audio??=new AudioContext();await audio.resume();refresh()};setInterval(refresh,500);refresh();</script>)HTML";
using namespace mvp;
namespace {
// CarLife 输入的媒体面与适配器。与 catplay-real 并列，各自一个独立的有界媒体面，
// 由 SessionCore 决定哪一个处于活跃态。
//
// TODO(Core/Web 拆分): 这两个全局指针是单文件装配层的权宜之计，
// 拆分路由/页面模块时应改成显式依赖注入。
#ifdef ZERO2W_WITH_CARLIFE
const carlife::CarLifeInputAdapter* g_carlife_adapter = nullptr;
RealMediaStore* g_carlife_media = nullptr;
#endif
constexpr std::size_t kMaxHeader=4096,kMaxBody=1024,kMaxRequest=kMaxHeader+kMaxBody;
constexpr int kAcceptedClientSendBuffer=64*1024;
volatile sig_atomic_t alive=1; void stop_signal(int){alive=0;}
std::string text32(const std::array<char,32>&a){std::size_t n=0;while(n<a.size()&&a[n])++n;return {a.data(),n};}
std::string esc(std::string_view s){std::string o;for(char c:s){if(c=='"'||c=='\\')o+='\\';if(static_cast<unsigned char>(c)>=32)o+=c;}return o;}
const char* lifecycle_status(const CpNativeTelemetryStatus& s){if(!s.available)return "unavailable";if(!s.connected)return "not-connected";if(s.ready)return "ready";if(s.negotiated)return s.main_screen==CpOfferState::Negotiating||s.media_audio==CpOfferState::Negotiating?"negotiating":"not-ready";if(s.main_screen==CpOfferState::Unsupported&&s.media_audio==CpOfferState::Unsupported)return "unsupported";return "not-negotiated";}
std::string status_json(const CpNativeTelemetryStatus&s){auto t=[&s](CpOfferState x){return s.available?cp_offer_text(x):std::string_view("unavailable");};std::ostringstream o;o<<"{\"engine\":\""<<(s.available?"telemetry-available":"unavailable")<<"\",\"status\":\""<<lifecycle_status(s)<<"\",\"schemaVersion\":"<<s.schema_version<<",\"sessionId\":"<<s.session_id<<",\"lastUpdatedMs\":"<<s.last_updated_ms<<",\"connected\":"<<(s.connected?"true":"false")<<",\"negotiated\":"<<(s.negotiated?"true":"false")<<",\"ready\":"<<(s.ready?"true":"false")<<",\"lifecycle\":\""<<(s.available?esc(s.lifecycle.data()):"unavailable")<<"\",\"wireless\":{\"listenerReady\":"<<(s.wireless_listener_ready?"true":"false")<<",\"offer\":\""<<t(s.wifi_bonjour)<<"\"},\"wired\":{\"endpointReady\":"<<(s.wired_endpoint_ready?"true":"false")<<",\"offer\":\""<<t(s.wired_usb)<<"\"},\"iphone\":{\"connected\":"<<(s.iphone_connected?"true":"false")<<",\"iap2\":\""<<t(s.iap2)<<"\",\"mfi\":\""<<t(s.mfi)<<"\"},\"vehicle\":{\"connected\":"<<(s.vehicle_connected?"true":"false")<<",\"offer\":\""<<t(s.vehicle)<<"\"},\"bridge\":{\"active\":"<<(s.bridge_active?"true":"false")<<",\"hap\":\""<<t(s.hap)<<"\",\"rtsp\":\""<<t(s.rtsp)<<"\"},\"features\":{\"bluetooth\":\""<<t(s.bluetooth)<<"\",\"mainScreen\":\""<<t(s.main_screen)<<"\",\"secondScreen\":\""<<t(s.second_screen)<<"\",\"instrumentScreen\":\""<<t(s.instrument_screen)<<"\",\"mediaAudio\":\""<<t(s.media_audio)<<"\",\"microphone\":\""<<t(s.microphone)<<"\",\"hid\":\""<<t(s.hid)<<"\"}}";return o.str();}
const char* media_ipc(RealMediaSnapshot::Ipc v){return v==RealMediaSnapshot::Ipc::Connected?"connected":v==RealMediaSnapshot::Ipc::ProtocolError?"protocol-error":"unavailable";}const char* media_session(RealMediaSnapshot::Session v){return v==RealMediaSnapshot::Session::Active?"active":"inactive";}const char* media_video(RealMediaSnapshot::Video v){switch(v){case RealMediaSnapshot::Video::Inactive:return "inactive";case RealMediaSnapshot::Video::WaitingConfig:return "waiting-config";case RealMediaSnapshot::Video::WaitingKeyframe:return "waiting-keyframe";case RealMediaSnapshot::Video::Active:return "active";case RealMediaSnapshot::Video::Unsupported:return "unsupported";case RealMediaSnapshot::Video::Failed:return "failed";}return "failed";}const char* media_audio(RealMediaSnapshot::Audio::State v){return v==RealMediaSnapshot::Audio::State::Active?"active":v==RealMediaSnapshot::Audio::State::Discontinuous?"discontinuous":"started-no-data";}const char* media_audio_type(uint32_t v){static const char* names[]={"default","alert","media","telephony","speech-recognition","compatibility"};return v<6?names[v]:"unknown";}
#ifdef ZERO2W_WITH_CARLIFE
std::string carlife_status_json(const carlife::CarLifeInputAdapter& adapter) {
  const auto c = adapter.status();
  std::ostringstream o;
  o << "{\"sourceId\":\"carlife-hu\",\"running\":" << (c.running ? "true" : "false")
    << ",\"connected\":" << (c.connected ? "true" : "false")
    << ",\"state\":\"" << c.state_name.data() << "\""
    << ",\"detail\":\"" << esc(c.detail.data()) << "\""
    << ",\"phoneIp\":\"" << esc(c.phone_ip.data()) << "\""
    << ",\"display\":{\"width\":" << c.video_width << ",\"height\":" << c.video_height
    << ",\"fps\":" << c.video_rate << "}"
    << ",\"video\":{\"framesReceived\":" << c.frames_received << ",\"framesForwarded\":" << c.frames_forwarded
    << ",\"keyframes\":" << c.keyframes << ",\"configs\":" << c.video_configs << ",\"bytes\":" << c.video_bytes << "}"
    << ",\"audio\":{\"mediaBytes\":" << c.media_bytes << ",\"ttsBytes\":" << c.tts_bytes
    << ",\"chunks\":" << c.audio_frames << "}"
    << ",\"control\":{\"sent\":" << c.controls_sent << ",\"dropped\":" << c.controls_dropped << "}"
    << ",\"microphone\":{\"requests\":" << c.mic_requests << ",\"frames\":" << c.mic_frames << "}"
    // A2：手机上报的播放进度（0..100）；A1：媒体音是否正被导航压低。
    << ",\"mediaProgress\":" << c.media_progress
    << ",\"mediaDucked\":" << (c.media_ducked ? "true" : "false")
    << ",\"bt\":{\"phase\":\"" << c.bt_phase.data() << "\",\"note\":\"" << esc(c.bt_note.data()) << "\"}}";
  return o.str();
}
#endif
// 无条件的包装：输出 JSON 片段，或 "null"。放在单行 state_json 里调用，
// 避免在表达式中间出现预处理指令。
std::string sources_json(SessionCore&);
// L3：新契约的状态片段（定义见文件后部「新契约的 Web 面」段）。
// 必须在这里先声明：它被 state_json 调用，而 state_json 定义在该段之前。
std::string new_state_json(SessionCore&,RealMediaStore&);

// 管理页面资源：启动时从磁盘读一次并做占位符替换。
// 资源与二进制放在同一个部署目录，便于整目录回滚。
std::string g_page;
// 浏览器音频转发默认关闭。理由：引擎自己就有原生音频播放器
// （引擎日志里的 `rtp::play::rtp_consumer` / `Audio player XRUN` 就是它），
// 我们再把同一份音频走 CPMF→HTTP→浏览器，等于重复消费一份数据。
// 实测后果：放音乐时引擎音频 XRUN（copied=0、buffer=0）、视频帧迟到 2 秒，
// 最终引擎丢掉整个 CarPlay 会话 —— 在 986 MB 的板子上被压垮。
// 需要远程听音调试时置 CP_MEDIA_AUDIO=1 强制打开。
bool g_media_audio_enabled = false;
bool read_whole_file(const std::string& path, std::string& out) {
  FILE* file = fopen(path.c_str(), "rb");
  if (!file) return false;
  char buf[4096];
  std::size_t n;
  while ((n = fread(buf, 1, sizeof buf, file)) > 0) out.append(buf, n);
  fclose(file);
  return true;
}
std::string build_page(const std::string& dir, std::string& error) {
  std::string html, css, js;
  if (!read_whole_file(dir + "/index.html", html)) { error = "missing index.html in " + dir; return {}; }
  if (!read_whole_file(dir + "/app.css", css)) { error = "missing app.css in " + dir; return {}; }
  if (!read_whole_file(dir + "/app.js", js)) { error = "missing app.js in " + dir; return {}; }
  if (html.size() + css.size() + js.size() > 512U * 1024U) { error = "page assets exceed 512 KiB"; return {}; }
  const auto substitute = [](std::string& text, const char* key, const std::string& value) {
    const auto at = text.find(key);
    if (at != std::string::npos) text.replace(at, std::strlen(key), value);
  };
  substitute(html, "{{STYLE}}", css);
  substitute(html, "{{SCRIPT}}", js);
  return html;
}
// "/media/<source>/<rest>"；返回 false 表示不是按来源限定的媒体路径。
bool split_media_path(const std::string& path, std::string& source, std::string& rest) {
  const std::string prefix = "/media/";
  if (path.rfind(prefix, 0) != 0) return false;
  const auto slash = path.find('/', prefix.size());
  if (slash == std::string::npos) return false;
  source = path.substr(prefix.size(), slash - prefix.size());
  rest = path.substr(slash);
  return !source.empty() && source.find("..") == std::string::npos;
}
// 每个输入有独立的媒体面，按来源选择。
RealMediaStore& store_for(const std::string& source, RealMediaStore& catplay) {
#ifdef ZERO2W_WITH_CARLIFE
  if (g_carlife_media && source == "carlife-hu") return *g_carlife_media;
#endif
  return catplay;
}

// 廉价的活跃视频描述（不复制 256 KiB 帧体）：页面据此决定走 SVG 渲染还是 H.264 解码。
// 自带前导逗号，这样在 state_json 那种密集单行里只需插入标识符，不必再写引号字面量。
std::string active_video_json(SessionCore& c) {
  std::ostringstream o;
  o << ",\"activeVideo\":{\"source\":\"" << text32(c.snapshot().active) << "\",\"encoding\":\""
    << (c.has_video() ? (c.active_video_encoding() == VideoEncoding::Svg ? "svg" : "h264") : "none")
    << "\"}";
  return o.str();
}

std::string carlife_state_json() {
#ifdef ZERO2W_WITH_CARLIFE
  return g_carlife_adapter ? carlife_status_json(*g_carlife_adapter) : std::string("null");
#else
  return "null";
#endif
}
std::string state_json(SessionCore&c,RealMediaStore& media,bool test){auto s=c.snapshot();auto m=media.snapshot();const auto telemetry=read_cp_native_telemetry();const auto&status=telemetry.status;std::ostringstream o;o<<"{\"synthetic\":"<<(test?"true":"false")<<",\"realCarPlayTelemetryAvailable\":"<<(status.available?"true":"false")<<",\"message\":\"synthetic/local media backend; telemetry is observation only\",\"mode\":\""<<(s.mode==SelectionMode::Automatic?"automatic":"manual")<<"\",\"active\":\""<<text32(s.active)<<"\",\"sources\":"<<sources_json(c)<<active_video_json(c)<<",\"drops\":{\"video\":"<<s.video_dropped<<",\"audio\":"<<s.audio_dropped<<",\"control\":"<<s.control_dropped<<"},\"controls\":[";for(std::size_t i=0;i<s.control_count;i++){if(i)o<<',';o<<'\"'<<esc({s.controls[i].detail.data(),strnlen(s.controls[i].detail.data(),s.controls[i].detail.size())})<<'\"';}o<<"],\"realCarPlayMedia\":{\"ipc\":\""<<media_ipc(m.ipc)<<"\",\"session\":\""<<media_session(m.session)<<"\",\"sessionId\":"<<m.session_id<<",\"videoConfigGeneration\":"<<m.video_config_generation<<",\"videoStreamId\":"<<m.video_stream<<",\"videoDropped\":"<<m.video_dropped<<",\"video\":\""<<media_video(m.video)<<"\",\"screens\":[";for(std::size_t i=0;i<m.screens.size();++i){if(i)o<<',';const auto&screen=m.screens[i];o<<"{\"role\":\""<<(screen.role==0?"main":"alt")<<"\",\"streamId\":"<<screen.stream_id<<",\"configGeneration\":"<<screen.config_generation<<",\"configBytes\":"<<screen.config_bytes<<",\"queuedFrames\":"<<screen.queued_frames<<",\"framesSent\":"<<screen.frames_sent<<",\"framesDropped\":"<<screen.frames_dropped<<",\"selected\":"<<(screen.selected?"true":"false")<<",\"state\":\""<<media_video(screen.state)<<"\"}";}o<<"],\"audio\":[";for(std::size_t i=0;i<m.audio_count;++i){if(i)o<<',';o<<"{\"streamId\":"<<m.audio[i].stream_id<<",\"audioType\":\""<<media_audio_type(m.audio[i].audio_type)<<"\",\"generation\":"<<m.audio[i].generation<<",\"state\":\""<<media_audio(m.audio[i].state)<<"\"}";}o<<"]},\"realCarPlayStatus\":"<<status_json(status)<<",\"realCarPlayTelemetry\":"<<(status.available?telemetry.json:"null")<<",\"carlife\":"<<carlife_state_json()<<new_state_json(c,media)<<"}";return o.str();}
std::string sources_json(SessionCore&c){auto s=c.snapshot();std::ostringstream o;o<<'[';for(std::size_t i=0;i<s.source_count;i++){if(i)o<<',';auto k=s.sources[i].kind==InputSourceKind::External?"external":s.sources[i].kind==InputSourceKind::LocalDesktop?"local-desktop":"synthetic";o<<"{\"id\":\""<<text32(s.sources[i].id)<<"\",\"kind\":\""<<k<<"\",\"connected\":"<<(s.sources[i].connected?"true":"false")<<'}';}o<<']';return o.str();}
void u16(std::string&s,uint16_t v){s.push_back(char(v));s.push_back(char(v>>8));}void u32(std::string&s,uint32_t v){u16(s,uint16_t(v));u16(s,uint16_t(v>>16));}
std::string wav(SessionCore&c){AudioChunk a;if(!c.latest_audio(a))return{};std::string o;o.reserve(44+a.sample_count*2);o+="RIFF";u32(o,uint32_t(36+a.sample_count*2));o+="WAVEfmt ";u32(o,16);u16(o,1);u16(o,1);u32(o,a.sample_rate);u32(o,a.sample_rate*2);u16(o,2);u16(o,16);o+="data";u32(o,uint32_t(a.sample_count*2));for(std::size_t i=0;i<a.sample_count;i++)u16(o,uint16_t(a.samples[i]));return o;}
bool send_all(int fd,const char*data,std::size_t n,std::chrono::steady_clock::time_point deadline){while(n){const auto now=std::chrono::steady_clock::now();if(now>=deadline)return false;const auto ms=std::max(1,int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count()));pollfd p{fd,POLLOUT,0};auto ready=poll(&p,1,ms);if(ready<=0)continue;auto r=send(fd,data,std::min<std::size_t>(n,16*1024),MSG_DONTWAIT|MSG_NOSIGNAL);if(r>0){data+=r;n-=std::size_t(r);continue;}if(r<0&&errno==EINTR)continue;if(r<0&&(errno==EAGAIN||errno==EWOULDBLOCK))continue;return false;}return true;}
void reply(int fd,int code,std::string_view type,const std::string&body){const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);const char*reason=code==200?"OK":code==204?"No Content":code==400?"Bad Request":code==401?"Unauthorized":code==404?"Not Found":code==409?"Conflict":code==415?"Unsupported Media Type":code==503?"Service Unavailable":"Payload Too Large";std::string h="HTTP/1.1 "+std::to_string(code)+" "+reason+"\r\nContent-Type: "+std::string(type)+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nCross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\n\r\n";if(!send_all(fd,h.data(),h.size(),deadline))return;if(!body.empty())(void)send_all(fd,body.data(),body.size(),deadline);}
bool peer_closed(int fd){pollfd p{fd,POLLIN|POLLHUP|POLLERR,0};const auto n=::poll(&p,1,0);if(n<=0)return false;if(p.revents&(POLLHUP|POLLERR|POLLNVAL))return true;if(p.revents&POLLIN){char byte;const auto got=::recv(fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);return got==0;}return false;}
void stream_audio(int fd,RealMediaStore&media){
  if(!g_media_audio_enabled){reply(fd,503,"application/json","{\"error\":\"browser audio forwarding is disabled; the CarPlay engine plays audio natively. Set CP_MEDIA_AUDIO=1 to force it.\"}");return;}
  auto snapshot=media.snapshot();if(snapshot.session!=RealMediaSnapshot::Session::Active||snapshot.audio_count==0){reply(fd,204,"application/json","");return;}const auto session=snapshot.session_id;std::string header="HTTP/1.1 200 OK\r\nContent-Type: application/vnd.zero2w.carplay-media.v1\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";if(!send_all(fd,header.data(),header.size(),std::chrono::steady_clock::now()+std::chrono::milliseconds(250)))return;RealMediaStore::AudioCursor cursor;while(alive){if(peer_closed(fd))break;snapshot=media.snapshot();if(snapshot.session_id!=session||snapshot.session!=RealMediaSnapshot::Session::Active||snapshot.audio_count==0)break;std::string packet;if(!media.audio_packet(cursor,packet)){std::this_thread::sleep_for(std::chrono::milliseconds(4));continue;}if(!send_all(fd,packet.data(),packet.size(),std::chrono::steady_clock::now()+std::chrono::milliseconds(250)))return;}}
void stream_video(int fd,RealMediaStore&media,uint32_t role){auto snapshot=media.snapshot();if(role>=snapshot.screens.size()||snapshot.session!=RealMediaSnapshot::Session::Active||snapshot.screens[role].stream_id==0||snapshot.screens[role].state==RealMediaSnapshot::Video::Inactive||snapshot.screens[role].state==RealMediaSnapshot::Video::Unsupported||snapshot.screens[role].state==RealMediaSnapshot::Video::Failed){reply(fd,204,"application/json","");return;}const auto session=snapshot.session_id;const auto stream=snapshot.screens[role].stream_id;std::string header="HTTP/1.1 200 OK\r\nContent-Type: application/vnd.zero2w.carplay-media.v1\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";if(!send_all(fd,header.data(),header.size(),std::chrono::steady_clock::now()+std::chrono::milliseconds(250)))return;uint64_t after=0;media.request_keyframe(stream);while(alive){if(peer_closed(fd))break;snapshot=media.snapshot();if(snapshot.session_id!=session||snapshot.session!=RealMediaSnapshot::Session::Active||snapshot.screens[role].stream_id!=stream)break;std::string packet;if(!media.video_packet(role,after,packet)){std::this_thread::sleep_for(std::chrono::milliseconds(4));continue;}uint64_t latest=after;for(std::size_t at=0;at+catplay_media::kHeaderBytes<=packet.size();){const auto*raw=reinterpret_cast<const uint8_t*>(packet.data()+at);const auto bytes=catplay_media::be32(raw+40);if(at+catplay_media::kHeaderBytes+bytes>packet.size())return;if(catplay_media::be16(raw+8)==uint16_t(catplay_media::Type::VideoFrame))latest=std::max(latest,catplay_media::be64(raw+24));at+=catplay_media::kHeaderBytes+bytes;}if(latest==after)return;if(!send_all(fd,packet.data(),packet.size(),std::chrono::steady_clock::now()+std::chrono::milliseconds(250)))return;after=latest;}}
std::string lower(std::string_view v){std::string o(v);for(char&c:o)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return o;}std::string_view trim(std::string_view v){while(!v.empty()&&(v.front()==' '||v.front()=='\t'))v.remove_prefix(1);while(!v.empty()&&(v.back()==' '||v.back()=='\t'))v.remove_suffix(1);return v;}
struct Request{std::string method,path,body;std::size_t body_len{},media_after{};bool has_length{},authorized{},has_media_after{},close_after{};};
bool parse_headers(const std::string&raw,std::size_t end,const std::string&token,Request&r){auto first=raw.find("\r\n");if(first==std::string::npos||first>512)return false;auto line=std::string_view(raw.data(),first);auto a=line.find(' '),z=line.rfind(' ');if(a==std::string_view::npos||z<=a||line.substr(z+1)!="HTTP/1.1")return false;r.method=std::string(line.substr(0,a));r.path=std::string(line.substr(a+1,z-a-1));if(r.path.empty()||r.path[0]!='/'||r.path.find("..")!=std::string::npos||r.path.find('?')!=std::string::npos)return false;std::size_t p=first+2;while(p<end){auto q=raw.find("\r\n",p);if(q==std::string::npos||q>end)return false;if(q==p)break;auto h=std::string_view(raw.data()+p,q-p);auto colon=h.find(':');if(colon==std::string_view::npos||colon==0)return false;auto name=lower(h.substr(0,colon));auto val=trim(h.substr(colon+1));if(name=="content-length"){if(r.has_length||val.empty())return false;r.has_length=true;std::size_t x=0;for(char c:val){if(c<'0'||c>'9'||x>(kMaxBody-(c-'0'))/10)return false;x=x*10+std::size_t(c-'0');}r.body_len=x;}else if(name=="x-media-after"){if(r.has_media_after||val.empty())return false;std::size_t x=0;for(char c:val){if(c<'0'||c>'9'||x>(SIZE_MAX-(c-'0'))/10)return false;x=x*10+std::size_t(c-'0');}r.media_after=x;r.has_media_after=true;}else if(name=="connection"&&lower(val)=="close")r.close_after=true;else if(!token.empty()&&((name=="x-auth-token"&&val==token)||(name=="authorization"&&val==std::string("Bearer ")+token)))r.authorized=true;p=q+2;}if(r.method=="POST"&&!r.has_length)return false;if(r.method!="POST"&&r.has_length&&r.body_len!=0)return false;if(token.empty())r.authorized=true;return true;}
bool read_body(int fd,std::string&raw,std::size_t header_end,const Request&r){const auto total=header_end+r.body_len;if(raw.size()>total)return false;std::array<char,512>b{};while(raw.size()<total){auto want=std::min(b.size(),total-raw.size());auto n=recv(fd,b.data(),want,0);if(n<=0)return false;raw.append(b.data(),std::size_t(n));}return true;}
std::string field(const std::string&b,std::string_view key){auto p=b.find(key);if(p==std::string::npos)return{};p+=key.size();auto e=b.find('&',p);return b.substr(p,e==std::string::npos?std::string::npos:e-p);}
bool decimal_field(const std::string& body,std::string_view key,uint32_t& out){auto value=field(body,key);if(value.empty())return false;uint64_t n=0;for(char c:value){if(c<'0'||c>'9'||n>(UINT32_MAX-uint32_t(c-'0'))/10)return false;n=n*10+uint32_t(c-'0');}out=uint32_t(n);return true;}
bool display_config_body(const std::string& body,ScreenOutputConfig& config,std::string& error){uint32_t enabled{};if(!decimal_field(body,"main_width=",config.main.width)||!decimal_field(body,"main_height=",config.main.height)||!decimal_field(body,"main_fps=",config.main.fps)||!decimal_field(body,"alt_enabled=",enabled)||!decimal_field(body,"alt_width=",config.alt.width)||!decimal_field(body,"alt_height=",config.alt.height)||!decimal_field(body,"alt_fps=",config.alt.fps)){error="all display fields must be unsigned integers";return false;}if(enabled>1){error="alt_enabled must be 0 or 1";return false;}config.main.enabled=true;config.alt.enabled=enabled==1;return valid_display_config(config,&error);}
void sync_media_selection(SessionCore& core,RealMediaStore& media){const auto active=text32(core.snapshot().active);media.set_selected(active=="catplay-real");
#ifdef ZERO2W_WITH_CARLIFE
  if(g_carlife_media)g_carlife_media->set_selected(active=="carlife-hu");
#endif
}
// ═══════════════════════════════════════════════════════════════════════════
// L3：新契约的 Web 面。契约冻结在 Core/MainMenu/include/core/session_core.hpp。
//
// 本段只做三件事：
//   1) 把 SessionSnapshot 的 7 块新字段（media/display/input/audio/vehicle/
//      telephony/link）序列化成**合法** JSON；
//   2) 只读端点：封面、歌词、通讯录、通话记录；
//   3) 写端点：交互注入（多点/旋钮/手势/接近/语音/电话键+DTMF/媒体键/车控）、
//      播放控制、显示设置（校准/昼夜/safe area/副屏平面/帧率）、多设备会话。
//
// 为什么单独一段：client() 与 state_json() 都是单行巨行，直接在里面对接极容易改错。
// 这里只对它们做两处最小插入：state_json 尾部插一个标识符，client() 里插一行转发。
//
// 两条硬要求（都是踩过的坑）：
//   · 字符串必须按 UTF-8 边界截断 —— 劈开汉字会让整份 /api/state 非法；
//   · 浮点必须先判 isfinite —— NaN/Inf 会写出非法 JSON。
// ═══════════════════════════════════════════════════════════════════════════
// <cmath> / <mutex> 已在文件头统一引入（这里是匿名命名空间内部，
// 在此处 #include 标准库头会掉进命名空间，造成 __is_integer 一类怪错）。

// 只保留完整的 UTF-8 字符；末尾若是不完整或非法的多字节序列就丢弃。
std::string utf8_safe(std::string_view s) {
  std::size_t i = 0, last_good = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const std::size_t len = c < 0x80 ? 1
                          : (c >= 0xC2 && c <= 0xDF) ? 2
                          : (c >= 0xE0 && c <= 0xEF) ? 3
                          : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
    if (len == 0 || i + len > s.size()) break;          // 非法首字节 / 被截断
    bool ok = true;
    for (std::size_t k = 1; k < len; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { ok = false; break; }
    if (!ok) break;
    i += len; last_good = i;
  }
  return std::string(s.substr(0, last_good));
}

// 定长 char 数组 → 去 NUL → UTF-8 安全字符串。
template <std::size_t N>
std::string textN(const std::array<char, N>& a) {
  std::size_t n = 0;
  while (n < a.size() && a[n]) ++n;
  return utf8_safe(std::string_view(a.data(), n));
}

// 任意字符串 → 带引号的 JSON 字符串字面量。
std::string json_str(std::string_view raw) {
  const std::string s = utf8_safe(raw);
  std::string o; o.reserve(s.size() + 2); o += '"';
  for (char ch : s) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
    else if (ch == '\n') o += "\\n";
    else if (ch == '\r') o += "\\r";
    else if (ch == '\t') o += "\\t";
    else if (u < 0x20) o += ' ';                        // 其余控制字符非法，替成空格
    else o += ch;
  }
  o += '"'; return o;
}
// 浮点：非有限值输出 null（JSON 不允许 NaN/Infinity）。
std::string json_num(double v) { if (!std::isfinite(v)) return "null"; std::ostringstream o; o << v; return o.str(); }
const char* json_bool(bool v) { return v ? "true" : "false"; }

// 表单值必须做 percent 解码。
// 浏览器的 `new URLSearchParams(obj).toString()` 会把 `,` `;` 和中文都转义成 %XX，
// 不解码就会把 "%2C" 当成字面量。既有代码只用到无反斜杠的安全值（如 key=volume_up），
// 所以一直没暴露；而多点触控的 `pts=x,y;x,y` 和中文姓名/号码必定会被转义。
std::string url_decode(std::string_view s) {
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string o; o.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') { o += ' '; continue; }
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) { o += char(hi * 16 + lo); i += 2; continue; }
    }
    o += s[i];
  }
  return o;
}
// 取字段并解码。纯数字的字段仍可用 decimal_field（数字不会被转义）。
std::string field_dec(const std::string& body, std::string_view key) { return url_decode(field(body, key)); }

// ── 通讯录 / 通话记录（Web 侧定长缓存）────────────────────────────────────
// 契约的 TelephonyState 只有条目数量、没有条目数组（kMaxContacts 已声明但未落存储），
// 所以这里按上限自备两块定长缓存，由 Input 侧通过 POST /api/telephony/* 推入。
// 全部走固定数组，无堆分配，与仓库既有风格一致。
struct WebContact { std::array<char, 64> name{}; std::array<char, 32> number{}; };
struct WebCallLogEntry { std::array<char, 64> name{}; std::array<char, 32> number{}; uint8_t type{}; uint32_t when{}; };
std::mutex g_web_telephony_mutex;
std::array<WebContact, mvp::kMaxContacts> g_contacts{}; std::size_t g_contact_count = 0;
std::array<WebCallLogEntry, mvp::kMaxContacts> g_calllog{}; std::size_t g_calllog_count = 0;

// ── 各块的 JSON（都不带前导逗号，便于单独复用与单测）──────────────────
std::string media_json(const mvp::SessionSnapshot& s) {
  const std::string src = text32(s.active);
  const bool art = s.media.has_artwork, lyr = s.media.has_lyrics;
  std::ostringstream o;
  o << "{\"valid\":" << json_bool(s.media.valid)
    << ",\"title\":" << json_str(textN(s.media.title))
    << ",\"artist\":" << json_str(textN(s.media.artist))
    << ",\"album\":" << json_str(textN(s.media.album))
    << ",\"albumArtist\":" << json_str(textN(s.media.album_artist))
    << ",\"app\":" << json_str(textN(s.media.app))
    << ",\"genre\":" << json_str(textN(s.media.genre))
    << ",\"trackNumber\":" << s.media.track_number
    << ",\"trackCount\":" << s.media.track_count
    << ",\"durationMs\":" << s.media.duration_ms
    << ",\"positionMs\":" << s.media.position_ms
    << ",\"playing\":" << json_bool(s.media.playing != 0)
    << ",\"repeat\":" << unsigned(s.media.repeat_mode)
    << ",\"shuffle\":" << json_bool(s.media.shuffle != 0)
    << ",\"hasArtwork\":" << json_bool(art)
    << ",\"artworkRevision\":" << s.media.artwork_revision
    << ",\"artworkBytes\":" << s.media.artwork_bytes
    // URL 里带 revision：换曲换封面时 URL 变了，浏览器缓存自然失效，不必额外做 304。
    << ",\"artUrl\":" << (art ? json_str("/media/" + src + "/artwork/" + std::to_string(s.media.artwork_revision)) : std::string("null"))
    << ",\"hasLyrics\":" << json_bool(lyr)
    << ",\"lyricsRevision\":" << s.media.lyrics_revision
    << ",\"lyricsBytes\":" << s.media.lyrics_bytes
    << ",\"lyricsUrl\":" << (lyr ? json_str("/media/" + src + "/lyrics/" + std::to_string(s.media.lyrics_revision)) : std::string("null"))
    << ",\"mediaLibraryRevision\":" << s.media.media_library_revision
    << ",\"contactsUrl\":" << json_str("/media/" + src + "/contacts")
    << ",\"callLogUrl\":" << json_str("/media/" + src + "/calllog")
    << '}';
  return o.str();
}

std::string display_json(const mvp::DisplayConfig& d) {
  std::ostringstream o;
  o << "{\"safeTop\":" << d.safe_top << ",\"safeBottom\":" << d.safe_bottom
    << ",\"safeLeft\":" << d.safe_left << ",\"safeRight\":" << d.safe_right
    << ",\"width\":" << d.width << ",\"height\":" << d.height
    << ",\"dayNight\":" << unsigned(d.day_night)
    << ",\"gamma\":" << unsigned(d.gamma_pct)
    << ",\"contrast\":" << unsigned(d.contrast_pct)
    << ",\"saturation\":" << unsigned(d.saturation_pct)
    << ",\"auxEnabled\":" << json_bool(d.aux_enabled != 0)
    << ",\"auxX\":" << d.aux_x << ",\"auxY\":" << d.aux_y
    << ",\"auxW\":" << d.aux_w << ",\"auxH\":" << d.aux_h
    << ",\"primaryPlane\":" << unsigned(d.primary_plane)
    << ",\"targetFps\":" << d.target_fps << ",\"actualFps\":" << d.actual_fps << '}';
  return o.str();
}

std::string input_json(const mvp::InteractionState& i) {
  std::ostringstream o;
  o << "{\"multiTouchPoints\":" << unsigned(i.multi_touch_points)
    << ",\"multiTouchUsed\":" << unsigned(i.multi_touch_used)
    << ",\"touchpad\":" << unsigned(i.touchpad) << ",\"knob\":" << unsigned(i.knob)
    << ",\"proximity\":" << unsigned(i.proximity) << ",\"hidMode\":" << unsigned(i.hid_mode)
    << ",\"voiceover\":" << unsigned(i.voiceover) << ",\"assistiveTouch\":" << unsigned(i.assistive_touch)
    << ",\"lastKeyCode\":" << i.last_key_code << '}';
  return o.str();
}

std::string audio_json(const mvp::AudioState& a) {
  std::ostringstream o;
  o << "{\"activeRole\":" << unsigned(a.active_role)
    << ",\"navActive\":" << json_bool(a.nav_active != 0)
    << ",\"mediaVolumePpm\":" << a.media_volume_ppm << ",\"navVolumePpm\":" << a.nav_volume_ppm
    << ",\"duckRatioPpm\":" << a.duck_ratio_ppm << ",\"duckTransitionMs\":" << a.duck_transition_ms
    << ",\"channels\":" << unsigned(a.channels) << ",\"sampleRate\":" << a.sample_rate
    << ",\"codec\":" << unsigned(a.codec)
    << ",\"simultaneousStreams\":" << unsigned(a.simultaneous_streams) << '}';
  return o.str();
}

std::string vehicle_json(const mvp::VehicleState& v) {
  std::ostringstream o;
  o << "{\"valid\":" << json_bool(v.valid) << ",\"gear\":" << unsigned(v.gear)
    << ",\"speedKph\":" << v.speed_kph << ",\"rpm\":" << v.rpm
    << ",\"latitude\":" << json_num(v.latitude) << ",\"longitude\":" << json_num(v.longitude)
    << ",\"headingDeg\":" << json_num(double(v.heading_deg))
    << ",\"odometerKm\":" << v.odometer_km
    << ",\"fuelPct\":" << v.fuel_pct << ",\"rangeKm\":" << v.range_km
    << ",\"outsideTempC\":" << v.outside_temp_c << ",\"nightMode\":" << unsigned(v.night_mode)
    << ",\"doors\":" << v.doors << ",\"lights\":" << unsigned(v.lights)
    << ",\"parkingBrake\":" << json_bool(v.parking_brake != 0)
    << ",\"vin\":" << json_str(textN(v.vin)) << '}';
  return o.str();
}

// 导航逐向。\"maneuverCode\" 是**原始值**：CarLife 的 action 码表由百度导航 App 产生，
// 参考树里没有对应映射表，我们也没有任何权威依据去解它，所以只能当数字原样透传。
// \"maneuver\" 是我们能确定的那部分（取值顺序与契约里的 Maneuver 枚举一致），不确定时为 0。
// 页面不得把 maneuverCode 自行展开成转向语义——那等于编一张没有依据的映射表。
std::string nav_json(const mvp::NavigationState& n) {
  std::ostringstream o;
  o << "{\"valid\":" << json_bool(n.valid)
    << ",\"active\":" << json_bool(n.active != 0)
    << ",\"maneuver\":" << unsigned(n.maneuver)
    << ",\"maneuverCode\":" << n.maneuver_code
    << ",\"roadName\":" << json_str(textN(n.road_name))
    << ",\"nextRoadName\":" << json_str(textN(n.next_road_name))
    << ",\"icon\":" << json_str(textN(n.icon))
    << ",\"destination\":" << json_str(textN(n.destination))
    << ",\"distanceToManeuverM\":" << n.distance_to_maneuver_m
    << ",\"distanceRemainingM\":" << n.distance_remaining_m
    << ",\"timeRemainingS\":" << n.time_remaining_s
    << ",\"etaEpochS\":" << n.eta_epoch_s
    << ",\"destinationReached\":" << json_bool(n.destination_reached != 0)
    << ",\"laneBitmap\":" << n.lane_bitmap << '}';
  return o.str();
}

std::string telephony_json(const mvp::TelephonyState& t) {
  std::ostringstream o;
  o << "{\"callState\":" << unsigned(t.call_state)
    << ",\"caller\":" << json_str(textN(t.caller))
    << ",\"callerNumber\":" << json_str(textN(t.caller_number))
    << ",\"callDurationS\":" << t.call_duration_s
    << ",\"signalBars\":" << unsigned(t.signal_bars) << ",\"batteryPct\":" << unsigned(t.battery_pct)
    << ",\"contactsReady\":" << json_bool(t.contacts_ready != 0)
    << ",\"contactCount\":" << t.contact_count
    << ",\"callLogReady\":" << json_bool(t.call_log_ready != 0)
    << ",\"callLogCount\":" << t.call_log_count
    << ",\"dtmfSupported\":" << json_bool(t.dtmf_supported != 0) << '}';
  return o.str();
}

std::string link_json(const mvp::LinkState& l) {
  std::ostringstream o;
  o << "{\"activationState\":" << unsigned(l.activation_state)
    << ",\"contentEncryption\":" << unsigned(l.content_encryption)
    << ",\"fileTransferActive\":" << json_bool(l.file_transfer_active != 0)
    << ",\"fileTransferBytes\":" << l.file_transfer_bytes
    << ",\"fileTransferTotal\":" << l.file_transfer_total
    << ",\"otaState\":" << unsigned(l.ota_state)
    << ",\"sessionCount\":" << unsigned(l.session_count)
    << ",\"activeSession\":" << json_str(textN(l.active_session))
    << ",\"sessions\":[";
  std::size_t n = 0;
  for (std::size_t i = 0; i < l.sessions.size() && n < std::size_t(l.session_count) && i < mvp::kMaxSessionList; ++i, ++n) {
    if (i) o << ',';
    o << "{\"id\":" << json_str(textN(l.sessions[i]))
      << ",\"active\":" << json_bool(l.session_active[i] != 0) << '}';
  }
  o << "]}";
  return o.str();
}

// 新字段总片段。**带前导逗号**，这样在 state_json 尾部只需插一个标识符。
std::string new_state_json(SessionCore& c, RealMediaStore& from) {
  (void)from;
  const auto s = c.snapshot();
  std::ostringstream o;
  o << ",\"media\":" << media_json(s)
    << ",\"display\":" << display_json(s.display)
    << ",\"input\":" << input_json(s.input)
    << ",\"audio\":" << audio_json(s.audio)
    << ",\"vehicle\":" << vehicle_json(s.vehicle)
    << ",\"nav\":" << nav_json(s.nav)
    << ",\"telephony\":" << telephony_json(s.telephony)
    << ",\"link\":" << link_json(s.link);
  return o.str();
}

// 解析 /media/{source}/{action}[/{arg}]。自己解析而不复用 split_media_path，
// 因为契约里 /media/{source}/... 要再多一级（artwork/{rev}）。
bool parse_media_route(const std::string& path, std::string& source, std::string& action, std::string& arg) {
  const std::string prefix = "/media/";
  if (path.rfind(prefix, 0) != 0) return false;
  const std::size_t a = prefix.size();
  const std::size_t b = path.find('/', a);
  if (b == std::string::npos) return false;
  source = path.substr(a, b - a);
  const std::size_t c = path.find('/', b + 1);
  if (c == std::string::npos) { action = path.substr(b + 1); arg.clear(); }
  else { action = path.substr(b + 1, c - b - 1); arg = path.substr(c + 1); }
  return !source.empty() && !action.empty();
}

// 带 ETag 的响应。既有 reply() 不支持额外响应头，这里单独写一份，
// 保留同样的安全头与 250ms 发送截止时间，避免与既有行为分叉。
void reply_blob(int fd, int code, std::string_view type, const std::string& body, const std::string& etag) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  const char* reason = code == 200 ? "OK" : code == 304 ? "Not Modified" : code == 404 ? "Not Found" : "Bad Request";
  std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + std::string(type) +
                  "\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nETag: " + etag +
                  "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                  "Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\n\r\n";
  if (!send_all(fd, h.data(), h.size(), deadline)) return;
  if (!body.empty()) (void)send_all(fd, body.data(), body.size(), deadline);
}

// 封面/歌词都是从 SessionCore 的固定缓冲里带锁取指针再释放锁，读期间理论上
// 可能被生产者整块覆盖。做法：读一遍 → 复核 revision 未变才采用，最多重试 3 次。
bool stable_artwork(SessionCore& c, std::string& out, uint32_t& revision) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    const char* data = nullptr; std::size_t size = 0; uint32_t rev = 0;
    if (!c.artwork(&data, &size, &rev) || !data || size == 0 || size > mvp::kMaxArtwork) return false;
    out.assign(data, size);
    const char* again = nullptr; std::size_t size2 = 0; uint32_t rev2 = 0;
    if (!c.artwork(&again, &size2, &rev2) || rev2 == rev) { revision = rev; return true; }
    out.clear();
  }
  return false;
}
bool stable_lyrics(SessionCore& c, std::string& out, uint32_t& revision) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::string_view view{}; uint32_t rev = 0;
    if (!c.lyrics(&view, &rev) || view.empty() || view.size() > mvp::kMaxLyrics) return false;
    out.assign(view.data(), view.size());
    std::string_view again{}; uint32_t rev2 = 0;
    if (!c.lyrics(&again, &rev2) || rev2 == rev) { revision = rev; return true; }
    out.clear();
  }
  return false;
}

// ── 交互注入：请求体 → ControlEvent ────────────────────────────────────
// 不认识的类型一律返回 false（调用方回 400），**绝不因为注入而拆会话**。
bool interact_from_body(const std::string& body, mvp::ControlEvent& e, std::string& error) {
  const std::string kind = field_dec(body, "kind=");
  if (kind == "multitouch") {
    // pts=x,y,id,phase;x,y,id,phase;...  一次提交整组触点。
    const std::string pts = field_dec(body, "pts=");
    if (pts.empty()) { error = "multitouch needs pts="; return false; }
    e.type = mvp::ControlEvent::Type::MultiTouch;
    std::size_t start = 0;
    while (start <= pts.size() && e.point_count < mvp::kMaxMultiTouch) {
      const std::size_t semi = pts.find(';', start);
      const std::string item = pts.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
      if (!item.empty()) {
        long long v[4] = {0, 0, 0, 0}; int n = 0; std::size_t p = 0;
        while (p <= item.size() && n < 4) {
          const std::size_t comma = item.find(',', p);
          const std::string tok = item.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
          if (tok.empty()) { n = 0; break; }
          char* end = nullptr;
          const long long parsed = std::strtoll(tok.c_str(), &end, 10);
          if (!end || *end != 0 || parsed < -32768 || parsed > 65535) { n = 0; break; }
          v[n++] = parsed;
          if (comma == std::string::npos) break;
          p = comma + 1;
        }
        if (n == 4 && v[3] >= 0 && v[3] <= 2) {
          auto& pt = e.points[e.point_count];
          pt.x = int16_t(v[0]); pt.y = int16_t(v[1]); pt.id = uint8_t(v[2]); pt.phase = uint8_t(v[3]);
          ++e.point_count;
        } else { error = "each multitouch point must be x,y,id,phase"; return false; }
      }
      if (semi == std::string::npos) break;
      start = semi + 1;
    }
    if (e.point_count == 0) { error = "multitouch needs at least one point"; return false; }
    return true;
  }
  if (kind == "knob") {
    const std::string dir = field_dec(body, "dir=");
    e.type = mvp::ControlEvent::Type::Knob;
    if (dir == "left") e.knob_dir = mvp::ControlEvent::KnobDir::Left;
    else if (dir == "right") e.knob_dir = mvp::ControlEvent::KnobDir::Right;
    else if (dir == "up") e.knob_dir = mvp::ControlEvent::KnobDir::Up;
    else if (dir == "down") e.knob_dir = mvp::ControlEvent::KnobDir::Down;
    else if (dir == "press") e.knob_dir = mvp::ControlEvent::KnobDir::Press;
    else { error = "knob dir must be left|right|up|down|press"; return false; }
    const std::string steps = field_dec(body, "steps=");
    if (!steps.empty()) {
      char* end = nullptr;
      const long parsed = std::strtol(steps.c_str(), &end, 10);
      if (!end || *end != 0 || parsed < -32768 || parsed > 32767) { error = "steps out of range"; return false; }
      e.knob_steps = int16_t(parsed);
    }
    return true;
  }
  if (kind == "gesture") {
    const std::string name = field_dec(body, "name=");
    if (name.empty() || name.size() >= e.gesture.size()) { error = "gesture name required (<=31 bytes)"; return false; }
    e.type = mvp::ControlEvent::Type::Gesture;
    std::memcpy(e.gesture.data(), name.data(), name.size());
    return true;
  }
  if (kind == "proximity") {
    uint32_t near = 0;
    if (!decimal_field(body, "near=", near) || near > 1) { error = "near must be 0 or 1"; return false; }
    e.type = mvp::ControlEvent::Type::Proximity;
    e.x = int(near);
    return true;
  }
  if (kind == "voice") { e.type = mvp::ControlEvent::Type::Voice; return true; }
  if (kind == "telephony" || kind == "dtmf") {
    // action=answer|hangup|dial|dtmf|redial；dtmf 走 dtmf=0..9,*,# 串
    const std::string action = kind == "dtmf" ? std::string("dtmf") : field_dec(body, "action=");
    e.type = mvp::ControlEvent::Type::Telephony;
    if (action == "dtmf") {
      const std::string digits = field_dec(body, "dtmf=");
      if (digits.empty() || digits.size() >= e.dtmf.size()) { error = "dtmf digits required (<=15)"; return false; }
      for (char ch : digits)
        if (!((ch >= '0' && ch <= '9') || ch == '*' || ch == '#')) { error = "dtmf allows 0-9 * # only"; return false; }
      std::memcpy(e.dtmf.data(), digits.data(), digits.size());
      std::memcpy(e.key.data(), "dtmf", 4);
      return true;
    }
    if (action.empty() || action.size() >= e.key.size()) { error = "telephony action required"; return false; }
    std::memcpy(e.key.data(), action.data(), action.size());
    return true;
  }
  if (kind == "mediakey") {
    const std::string key = field_dec(body, "key=");
    if (key.empty() || key.size() >= e.key.size()) { error = "media key required"; return false; }
    e.type = mvp::ControlEvent::Type::Key;
    std::memcpy(e.key.data(), key.data(), key.size());
    if (key == "seek") {
      // 契约的 ControlEvent 没有 seek 位置字段（见 DESIGN.md「已知契约缺口」）。
      // 约定：key="seek" 时用 x 承载目标毫秒数，Input 侧按此约定读取。
      uint32_t pos = 0;
      if (!decimal_field(body, "positionMs=", pos)) { error = "seek needs positionMs="; return false; }
      e.x = int(pos);
    }
    return true;
  }
  if (kind == "vehicle") {
    const std::string ctrl = field_dec(body, "ctrl=");
    if (ctrl.empty() || ctrl.size() >= e.ctrl.size()) { error = "vehicle ctrl name required (<=31 bytes)"; return false; }
    e.type = mvp::ControlEvent::Type::VehicleCtrl;
    std::memcpy(e.ctrl.data(), ctrl.data(), ctrl.size());
    return true;
  }
  error = "unknown kind";
  return false;
}

// 通讯录/通话记录推入。体长受 kMaxBody 限制，所以每次推 <=8 条。
bool telephony_push(const std::string& body, bool call_log, std::string& error) {
  uint32_t count = 0;
  if (!decimal_field(body, "count=", count) || count > 8) { error = "count must be 0..8"; return false; }
  std::lock_guard<std::mutex> lock(g_web_telephony_mutex);
  if (call_log) g_calllog_count = 0; else g_contact_count = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const std::string suffix = std::to_string(i);
    const std::string name = field_dec(body, "name" + suffix + "=");
    const std::string number = field_dec(body, "number" + suffix + "=");
    if (name.size() >= 64 || number.size() >= 32) { error = "name<=63 or number<=31 bytes"; return false; }
    if (call_log) {
      uint32_t type = 0, when = 0;
      if (!decimal_field(body, "type" + suffix + "=", type) || type > 3) { error = "type must be 0..3"; return false; }
      if (!decimal_field(body, "when" + suffix + "=", when)) { error = "when (unix seconds) required"; return false; }
      auto& e = g_calllog[g_calllog_count];
      std::memcpy(e.name.data(), name.data(), name.size());
      std::memcpy(e.number.data(), number.data(), number.size());
      e.type = uint8_t(type); e.when = when;
      ++g_calllog_count;
    } else {
      auto& e = g_contacts[g_contact_count];
      std::memcpy(e.name.data(), name.data(), name.size());
      std::memcpy(e.number.data(), number.data(), number.size());
      ++g_contact_count;
    }
  }
  return true;
}

std::string contacts_json() {
  std::lock_guard<std::mutex> lock(g_web_telephony_mutex);
  std::ostringstream o; o << "{\"count\":" << g_contact_count << ",\"contacts\":[";
  for (std::size_t i = 0; i < g_contact_count; ++i) {
    if (i) o << ',';
    o << "{\"name\":" << json_str(textN(g_contacts[i].name))
      << ",\"number\":" << json_str(textN(g_contacts[i].number)) << '}';
  }
  o << "]}";
  return o.str();
}

std::string calllog_json() {
  std::lock_guard<std::mutex> lock(g_web_telephony_mutex);
  std::ostringstream o; o << "{\"count\":" << g_calllog_count << ",\"entries\":[";
  for (std::size_t i = 0; i < g_calllog_count; ++i) {
    if (i) o << ',';
    o << "{\"name\":" << json_str(textN(g_calllog[i].name))
      << ",\"number\":" << json_str(textN(g_calllog[i].number))
      << ",\"type\":" << unsigned(g_calllog[i].type)
      << ",\"when\":" << g_calllog[i].when << '}';
  }
  o << "]}";
  return o.str();
}

// ── 显示设置：读-改-写 ────────────────────────────────────────────────
// 只覆盖请求里出现的项，其余保持原值 —— 因为 DisplayConfig 同时是 Input 侧
// 上报的状态（分辨率/实际帧率/主平面），整块覆盖会把上报值抹掉。
bool apply_display_settings(const std::string& body, mvp::DisplayConfig& cfg, std::string& error) {
  uint32_t v = 0;
  if (field(body, "gamma=").size()) { if (!decimal_field(body, "gamma=", v) || v < 10 || v > 200) { error = "gamma must be 10..200"; return false; } cfg.gamma_pct = uint8_t(v); }
  if (field(body, "contrast=").size()) { if (!decimal_field(body, "contrast=", v) || v < 10 || v > 200) { error = "contrast must be 10..200"; return false; } cfg.contrast_pct = uint8_t(v); }
  if (field(body, "saturation=").size()) { if (!decimal_field(body, "saturation=", v) || v < 10 || v > 200) { error = "saturation must be 10..200"; return false; } cfg.saturation_pct = uint8_t(v); }
  if (field(body, "dayNight=").size()) { if (!decimal_field(body, "dayNight=", v) || v > 1) { error = "dayNight must be 0 or 1"; return false; } cfg.day_night = uint8_t(v); }
  if (field(body, "safeTop=").size()) { if (!decimal_field(body, "safeTop=", v) || v > 4096) { error = "safeTop out of range"; return false; } cfg.safe_top = uint16_t(v); }
  if (field(body, "safeBottom=").size()) { if (!decimal_field(body, "safeBottom=", v) || v > 4096) { error = "safeBottom out of range"; return false; } cfg.safe_bottom = uint16_t(v); }
  if (field(body, "safeLeft=").size()) { if (!decimal_field(body, "safeLeft=", v) || v > 4096) { error = "safeLeft out of range"; return false; } cfg.safe_left = uint16_t(v); }
  if (field(body, "safeRight=").size()) { if (!decimal_field(body, "safeRight=", v) || v > 4096) { error = "safeRight out of range"; return false; } cfg.safe_right = uint16_t(v); }
  if (field(body, "auxEnabled=").size()) { if (!decimal_field(body, "auxEnabled=", v) || v > 1) { error = "auxEnabled must be 0 or 1"; return false; } cfg.aux_enabled = uint8_t(v); }
  if (field(body, "auxX=").size()) { if (!decimal_field(body, "auxX=", v) || v > 8192) { error = "auxX out of range"; return false; } cfg.aux_x = uint16_t(v); }
  if (field(body, "auxY=").size()) { if (!decimal_field(body, "auxY=", v) || v > 8192) { error = "auxY out of range"; return false; } cfg.aux_y = uint16_t(v); }
  if (field(body, "auxW=").size()) { if (!decimal_field(body, "auxW=", v) || v > 8192) { error = "auxW out of range"; return false; } cfg.aux_w = uint16_t(v); }
  if (field(body, "auxH=").size()) { if (!decimal_field(body, "auxH=", v) || v > 8192) { error = "auxH out of range"; return false; } cfg.aux_h = uint16_t(v); }
  if (field(body, "targetFps=").size()) { if (!decimal_field(body, "targetFps=", v) || v < 1 || v > 120) { error = "targetFps must be 1..120"; return false; } cfg.target_fps = uint16_t(v); }
  return true;
}

// ── 新端点统一入口 ────────────────────────────────────────────────────
// 返回 true 表示已经响应完（调用方直接结束）。只处理自己认识的路由，
// 其余一律返回 false 交回既有分发链 —— 保证老端点行为完全不变。
bool web_api(int fd, const Request& r, SessionCore& core) {
  std::string source, action, arg;
  const bool media_route = parse_media_route(r.path, source, action, arg);

  if (r.method == "GET" && media_route) {
    if (action == "artwork") {
      std::string blob; uint32_t rev = 0;
      if (!stable_artwork(core, blob, rev)) { reply(fd, 404, "application/json", "{\"error\":\"no artwork\"}"); return true; }
      reply_blob(fd, 200, "image/jpeg", blob, "\"art-" + std::to_string(rev) + "\"");
      return true;
    }
    if (action == "lyrics") {
      std::string text; uint32_t rev = 0;
      if (!stable_lyrics(core, text, rev)) { reply(fd, 404, "application/json", "{\"error\":\"no lyrics\"}"); return true; }
      reply_blob(fd, 200, "text/plain; charset=utf-8", text, "\"lrc-" + std::to_string(rev) + "\"");
      return true;
    }
    if (action == "contacts") { reply_blob(fd, 200, "application/json", contacts_json(), "\"contacts\""); return true; }
    if (action == "calllog") { reply_blob(fd, 200, "application/json", calllog_json(), "\"calllog\""); return true; }
    return false;   // 其余 /media/{source}/... 交回既有路由
  }

  if (r.method == "POST" && r.path == "/api/interact") {
    mvp::ControlEvent e;
    std::string error;
    if (!interact_from_body(r.body, e, error)) { reply(fd, 400, "application/json", "{\"error\":" + json_str(error) + "}"); return true; }
    const bool accepted = core.route_control(e);
    reply(fd, accepted ? 200 : 400, "application/json",
          std::string("{\"accepted\":") + json_bool(accepted) + ",\"type\":" + std::to_string(unsigned(e.type)) + "}");
    return true;
  }

  if (r.method == "POST" && r.path == "/api/playback") {
    const std::string act = field_dec(r.body, "action=");
    mvp::ControlEvent e;
    e.type = mvp::ControlEvent::Type::Key;
    if (act == "resume" || act == "play") std::memcpy(e.key.data(), "play", 5);
    else if (act == "pause") std::memcpy(e.key.data(), "pause", 6);
    else if (act == "next") std::memcpy(e.key.data(), "next", 5);
    else if (act == "prev") std::memcpy(e.key.data(), "prev", 5);
    else if (act == "ff") std::memcpy(e.key.data(), "ff", 3);
    else if (act == "rew") std::memcpy(e.key.data(), "rew", 4);
    else if (act == "seek") {
      uint32_t pos = 0;
      if (!decimal_field(r.body, "positionMs=", pos)) { reply(fd, 400, "application/json", "{\"error\":\"seek needs positionMs=\"}"); return true; }
      std::memcpy(e.key.data(), "seek", 5);
      e.x = int(pos);       // 与 /api/interact?kind=mediakey&key=seek 同一约定
    } else { reply(fd, 400, "application/json", "{\"error\":\"action must be resume|pause|next|prev|ff|rew|seek\"}"); return true; }
    const bool accepted = core.route_control(e);
    reply(fd, accepted ? 200 : 400, "application/json", std::string("{\"accepted\":") + json_bool(accepted) + "}");
    return true;
  }

  if (r.method == "POST" && r.path == "/api/settings/display") {
    mvp::DisplayConfig cfg = core.snapshot().display;   // 读-改-写，保留 Input 侧上报值
    std::string error;
    if (!apply_display_settings(r.body, cfg, error)) { reply(fd, 400, "application/json", "{\"error\":" + json_str(error) + "}"); return true; }
    core.set_display_config(cfg);
    reply(fd, 200, "application/json", std::string("{\"accepted\":true,\"display\":") + display_json(cfg) + "}");
    return true;
  }

  if (r.method == "POST" && r.path == "/api/sessions") {
    const std::string act = field_dec(r.body, "action=");
    const std::string id = field_dec(r.body, "id=");
    if (act == "switch") {
      if (id.empty()) { reply(fd, 400, "application/json", "{\"error\":\"id required\"}"); return true; }
      const bool ok = core.set_active_session(id);
      reply(fd, ok ? 200 : 400, "application/json", std::string("{\"accepted\":") + json_bool(ok) + "}");
      return true;
    }
    if (act == "upsert") {
      uint32_t active = 0;
      if (id.empty()) { reply(fd, 400, "application/json", "{\"error\":\"id required\"}"); return true; }
      (void)decimal_field(r.body, "active=", active);
      const bool ok = core.upsert_session(id, active == 1);
      reply(fd, ok ? 200 : 400, "application/json", std::string("{\"accepted\":") + json_bool(ok) + "}");
      return true;
    }
    reply(fd, 400, "application/json", "{\"error\":\"action must be upsert|switch\"}");
    return true;
  }

  if (r.method == "POST" && r.path == "/api/telephony/contacts") {
    std::string error;
    if (!telephony_push(r.body, false, error)) { reply(fd, 400, "application/json", "{\"error\":" + json_str(error) + "}"); return true; }
    reply(fd, 200, "application/json", contacts_json());
    return true;
  }
  if (r.method == "POST" && r.path == "/api/telephony/calllog") {
    std::string error;
    if (!telephony_push(r.body, true, error)) { reply(fd, 400, "application/json", "{\"error\":" + json_str(error) + "}"); return true; }
    reply(fd, 200, "application/json", calllog_json());
    return true;
  }

  return false;
}

bool client(int fd,SessionCore&core,RealMediaStore&media,SyntheticWirelessCarPlayBackend&synthetic,LocalDesktopBackend&local,DesktopRenderer&desktop,const std::string&token,const std::string&display_config_path){timeval tv{2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));std::string raw;raw.reserve(kMaxRequest);std::array<char,512>b{};std::size_t end=std::string::npos;while(raw.size()<=kMaxHeader){auto n=recv(fd,b.data(),b.size(),0);if(n<=0)return false;raw.append(b.data(),std::size_t(n));end=raw.find("\r\n\r\n");if(end!=std::string::npos)break;}if(end==std::string::npos||end>kMaxHeader){reply(fd,400,"text/plain","bad header");return false;}Request r;if(!parse_headers(raw,end,token,r)){reply(fd,400,"text/plain","bad request");return false;}// 公开面**只有** /（页面外壳）。/health **不在**其中：
// ・http_tests 明确要求"设了 --token 时 /health 无令牌 → 401"；
// ・把 /health 当公开探针，等于未授权也能探测"服务在跑"。
// 不设 --token 时（或 WEB_OPEN_API=1）仍然全部放行 —— 与既有测试流程一致。
const bool public_shell=r.method=="GET"&&r.path=="/";if(!public_shell&&!r.authorized){reply(fd,401,"text/plain","token required");return false;}const auto header_end=end+4;if(!read_body(fd,raw,header_end,r)){reply(fd,400,"text/plain","bad body");return false;}r.body=raw.substr(header_end);bool test=synthetic.running();if(web_api(fd,r,core))return false;if(r.method=="GET"&&r.path=="/health")reply(fd,200,"application/json","{\"ok\":true,\"backend\":\"synthetic\"}");else if(r.method=="GET"&&r.path=="/api/state")reply(fd,200,"application/json",state_json(core,media,test));else if(r.method=="GET"&&r.path=="/api/display-config"){ScreenOutputConfig config;std::string error;if(load_display_config(display_config_path,config,&error))reply(fd,200,"application/json",display_config_json(config));else reply(fd,503,"application/json","{\"error\":\""+esc(error)+"\"}");}else if(r.method=="POST"&&r.path=="/api/display-config"){ScreenOutputConfig config;std::string error;if(!display_config_body(r.body,config,error)||!save_display_config(display_config_path,config,&error))reply(fd,400,"application/json","{\"error\":\""+esc(error)+"\"}");else reply(fd,200,"application/json",display_config_json(config));}else if(r.method=="GET"&&r.path=="/api/sources")reply(fd,200,"application/json",sources_json(core));else if(r.method=="GET"&&r.path=="/media/frame.svg"){// 堆分配：VideoFrame 内含 256 KiB 载荷数组，放栈上会让本函数栈帧达 263 KiB。
// （本函数跑在 4 个 HTTP worker 线程上，8 MiB 栈虽然撐得住，但每次请求白吃 257 KiB
// 是不必要的，也会掩盖真正的大栈帧问题。-fstack-usage 已实测过这 263216 字节。）
auto f=std::make_unique<VideoFrame>();if(!core.latest_video(*f))reply(fd,200,"image/svg+xml","<svg xmlns='http://www.w3.org/2000/svg'/>");else if(f->encoding==VideoEncoding::Svg)reply(fd,200,"image/svg+xml",std::string(f->payload.data(),f->size));else reply(fd,415,"text/plain","H264 Annex-B is a deferred backend seam; no browser endpoint is implemented");}else if(r.method=="GET"&&r.path=="/media/audio.wav")reply(fd,200,"audio/wav",wav(core));else if(r.method=="GET"&&r.path=="/media/audio/stream"){if(text32(core.snapshot().active)!="catplay-real")reply(fd,409,"application/json","{\"error\":\"catplay-real is not selected\"}");else stream_audio(fd,media);return false;}else if(r.method=="GET"&&(r.path=="/media/video/main/stream"||r.path=="/media/video/alt/stream")){if(text32(core.snapshot().active)!="catplay-real")reply(fd,409,"application/json","{\"error\":\"catplay-real is not selected\"}");else stream_video(fd,media,r.path=="/media/video/alt/stream"?1:0);return false;}else if(r.method=="GET"&&(r.path=="/media/video"||r.path=="/media/video/main"||r.path=="/media/video/alt"||r.path=="/media/audio")){if(text32(core.snapshot().active)!="catplay-real")reply(fd,409,"application/json","{\"error\":\"catplay-real is not selected\"}");else{std::string packet;const bool is_video=r.path!="/media/audio";const uint32_t role=r.path=="/media/video/alt"?1:0;bool ok=is_video?media.video_packet(role,r.media_after,packet):media.audio_packet(packet);if(!ok)reply(fd,204,"application/json","");else reply(fd,200,"application/vnd.zero2w.carplay-media.v1",packet);}}else if(r.method=="GET"&&r.path.rfind("/media/",0)==0){std::string source,rest;if(!split_media_path(r.path,source,rest)){reply(fd,400,"text/plain","bad media path");}else{RealMediaStore& store=store_for(source,media);if(rest=="/video/main/stream"||rest=="/video/alt/stream"){stream_video(fd,store,rest=="/video/alt/stream"?1:0);return false;}else if(rest=="/audio/stream"){stream_audio(fd,store);return false;}else if(rest=="/video"||rest=="/video/main"||rest=="/video/alt"||rest=="/audio"){std::string packet;const bool is_video=rest!="/audio";const uint32_t role=rest=="/video/alt"?1:0;const bool ok=is_video?store.video_packet(role,r.media_after,packet):store.audio_packet(packet);if(!ok)reply(fd,204,"application/json","");else reply(fd,200,"application/vnd.zero2w.carplay-media.v1",packet);}else reply(fd,404,"text/plain","not found");}}else if(r.method=="POST"&&r.path=="/api/media/keyframe"){auto v=field(r.body,"stream_id=");char* end=nullptr;auto id=std::strtoul(v.c_str(),&end,10);bool ok=end&&*end==0&&id<=UINT32_MAX&&media.request_keyframe(uint32_t(id));reply(fd,ok?200:400,"application/json",ok?"{\"accepted\":true}":"{\"error\":\"invalid stream\"}");}else if(r.method=="GET"&&r.path=="/")reply(fd,200,"text/html; charset=utf-8",g_page.empty()?std::string(kPage):g_page);else if(r.method=="POST"&&r.path=="/api/select"){auto mode=field(r.body,"mode=");auto source=field(r.body,"source=");bool ok=mode=="automatic"?core.set_selection(SelectionMode::Automatic):mode=="manual"?core.set_selection(SelectionMode::Manual,source):false;if(ok)sync_media_selection(core,media);if(ok)desktop.publish_now();reply(fd,ok?200:400,"application/json",ok?state_json(core,media,synthetic.running()):"{\"error\":\"invalid selection\"}");}else if(r.method=="POST"&&r.path=="/api/test-mode"){auto enabled=field(r.body,"enabled=");if(enabled=="1"||enabled=="on")synthetic.start();else if(enabled=="0"||enabled=="off"){synthetic.stop();local.refresh();}else{reply(fd,400,"application/json","{\"error\":\"enabled must be 0 or 1\"}");return false;}sync_media_selection(core,media);desktop.publish_now();reply(fd,200,"application/json",state_json(core,media,synthetic.running()));}else if(r.method=="POST"&&r.path=="/api/control"){ControlEvent e;std::string control_error;if(!parse_control_request(r.body,e,control_error)){reply(fd,400,"application/json","{\"error\":\""+esc(control_error)+"\"}");return false;}const bool accepted=core.route_control(e);reply(fd,accepted?200:400,"application/json",accepted?"{\"accepted\":true}":"{\"accepted\":false}");}else reply(fd,404,"text/plain","not found");return false;}
class HttpWorkers {
 public:
  HttpWorkers(SessionCore&core,RealMediaStore&media,SyntheticWirelessCarPlayBackend&synthetic,LocalDesktopBackend&local,DesktopRenderer&desktop,const std::string&token,const std::string&config):core_(core),media_(media),synthetic_(synthetic),local_(local),desktop_(desktop),token_(token),config_(config){for(auto&worker:workers_)worker=std::thread([this]{run();});}
  ~HttpWorkers(){{std::lock_guard lock(mutex_);stopping_=true;for(std::size_t i=0;i<count_;++i)close(queue_[(begin_+i)%queue_.size()]);count_=0;}ready_.notify_all();for(auto&worker:workers_)if(worker.joinable())worker.join();}
  bool submit(int fd){std::lock_guard lock(mutex_);if(stopping_||count_==queue_.size())return false;queue_[(begin_+count_)%queue_.size()]=fd;++count_;ready_.notify_one();return true;}
 private:
  void run(){for(;;){int fd=-1;{std::unique_lock lock(mutex_);ready_.wait(lock,[this]{return stopping_||count_;});if(stopping_&&!count_)return;fd=queue_[begin_];begin_=(begin_+1)%queue_.size();--count_;}if(setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&kAcceptedClientSendBuffer,sizeof(kAcceptedClientSendBuffer))==0)for(int request=0;request<128;++request)if(!client(fd,core_,media_,synthetic_,local_,desktop_,token_,config_))break;close(fd);}}
  // desktop_：只用于在【切换活跃源到桌面源】时调 publish_now() 让画面立刻出来（不等 1 Hz tick）。
  // publish_now() 由 DesktopRenderer 负责做成线程安全的（它与 1 Hz tick 线程并发）。
  SessionCore&core_;RealMediaStore&media_;SyntheticWirelessCarPlayBackend&synthetic_;LocalDesktopBackend&local_;DesktopRenderer&desktop_;std::string token_,config_;std::mutex mutex_;std::condition_variable ready_;std::array<std::thread,4>workers_{};std::array<int,8>queue_{};std::size_t begin_{},count_{};bool stopping_{};
};
// 退出路径上一旦发生 terminate（典型原因：joinable 状态的 std::thread 被析构），
// 默认只会得到一句 “terminate called without an active exception” 和 SIGABRT，
// 没有位置信息。这里把原因与调用栈直接打到 stderr，便于定位。
// 配合 -rdynamic 才有符号名。
void terminate_handler() {
  if (auto ex = std::current_exception()) {
    try {
      std::rethrow_exception(ex);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[fatal] uncaught exception: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "[fatal] uncaught non-std exception\n");
    }
  } else {
    std::fprintf(stderr, "[fatal] terminate: joinable thread destroyed?\n");
  }
  void* frames[32];
  const int count = ::backtrace(frames, 32);
  ::backtrace_symbols_fd(frames, count, 2);
  std::fflush(stderr);
  std::_Exit(3);
}

bool port_number(const std::string&s,int&out){if(s.empty())return false;unsigned long n=0;for(char c:s){if(c<'0'||c>'9'||n>6553)return false;n=n*10+unsigned(c-'0');}if(n==0||n>65535)return false;out=int(n);return true;}
}
// 读取本机蓝牙适配器 MAC。
// 为什么需要：HU_INFO.btAddress 与 HU_FEATURE_CONFIG_RESPONSE.huBtMac 会把车机蓝牙地址
// 告诉手机；若上报一个假地址（旧默认值 00:11:22:33:44:55），手机就会拿一个不存在的
// 地址去找车机。这里优先用 bluetoothctl，拿不到再退 hciconfig。
static std::string detectAdapterBtMac(){
  // 【必须有超时，否则会卡死整个启动流程】
  // 实测教训：本函数在主线程上调用，而 popen+fgets 会一直等到子进程结束。
  // 在“有 bluetoothctl 但没有 bluetoothd 在跑”的环境（如 WSL）里，
  // bluetoothctl 会去连系统 D-Bus 并【永久阻塞】—— 主线程卡在 pipe_read，
  // 于是 HTTP 监听永远建不起来（curl /api/state 全部超时）。
  // 板上因为 bluetoothd 在跑所以看不出来，但这是潜伏风险：
  // bluetoothd 一旦异常，mvp_server 就会卡在启动阶段。
  // 因此：每个命令都加 timeout，并用 </dev/null 避免任何交互式等待。
  //
  // 【补充教训·第二次踩同一个坑】只写 `timeout 3` 是不够的：GNU timeout 到点只发
  // SIGTERM，而 bluetoothctl 卡在 D-Bus 调用时**不理会 SIGTERM**；timeout 默认不会
  // 升级为 SIGKILL（需要 -k），于是它自己会一直等下去 —— 管道永不关闭，fgets 永久
  // 阻塞，现象与不加 timeout 完全一样（主线程 wchan=pipe_read）。
  // 实测：同一二进制在 D-Bus 状态不同时一次正常、一次永久挂住（"看运气"）。
  // 所以必须用 `timeout -k <宽限> <上限>` 让它在宽限期后 SIGKILL。
  const char* cmds[] = {
    "timeout -k 1 3 bluetoothctl show </dev/null 2>/dev/null | awk '/Controller/{print $2; exit}'",
    "timeout -k 1 3 hciconfig hci0 </dev/null 2>/dev/null | awk '/BD Address/{print $3; exit}'",
  };
  for(const char* cmd : cmds){
    FILE* p=popen(cmd,"r");
    if(!p) continue;
    char buf[160];
    std::string got;
    while(std::fgets(buf,sizeof buf,p)) got+=buf;
    pclose(p);
    while(!got.empty()&&(got.back()=='\n'||got.back()=='\r'||got.back()==' ')) got.pop_back();
    if(got.size()>=11 && got.find(':')!=std::string::npos) return got;
  }
  return std::string();
}

int main(int argc,char**argv){std::string address="127.0.0.1",token,display_config_path="/tmp/zero2w-carplay-display.conf",assets_dir;if(const char* env=std::getenv("CP_DISPLAY_CONFIG"))display_config_path=env;int port=8080;for(int i=1;i<argc;i++){std::string x=argv[i];if(x=="--bind"&&i+1<argc)address=argv[++i];else if(x=="--port"&&i+1<argc){if(!port_number(argv[++i],port)){std::cerr<<"invalid port\n";return 2;}}else if(x=="--token"&&i+1<argc)token=argv[++i];else if(x=="--assets"&&i+1<argc)assets_dir=argv[++i];else if(x=="--display-config"&&i+1<argc)display_config_path=argv[++i];else{std::cerr<<"usage: mvp_server [--bind IPv4] [--port 1..65535] [--token value] [--display-config path]\n";return 2;}}if(address=="::1"||address.find(':')!=std::string::npos){std::cerr<<"IPv6 is not supported; use an IPv4 address\n";return 2;}in_addr parsed{};if(inet_pton(AF_INET,address.c_str(),&parsed)!=1){std::cerr<<"invalid IPv4 bind address\n";return 2;}// 0.0.0.0 （监听所有网卡）必须带 --token：本版本不带令牌时所有 /api/* 都是
// 匿名可访问的，再绑到全部网卡就等于把车机控制面旁听给整个局域网。
// 带令牌的通路仍然放行（Core/Web/tests/http_tests.cpp 后半段就依赖
// "--bind 0.0.0.0 --token ..." 能正常起来，所以这里不能无条件拒绝）。
if(address=="0.0.0.0"&&token.empty()){std::cerr<<"--bind 0.0.0.0 requires --token (management API would be world-readable otherwise)\n";return 2;}// 测试部署：不带令牌运行。传入 --token 也会被忽略（板上脚本里残留的 --token 因此失效），
// 目的是让面板与所有 /api/* 在局域网内可直接访问，省掉每天手工带令牌的麻烦。
// 令牌语义：**默认必须校验**；只有显式逃生阀 WEB_OPEN_API=1 才关掉（仅供测试部署）。
// 历史写法是“只要传了 --token 就把 token.clear() 并打印 [test build] --token ignored”，
// 这使安全姿态变成**隐式的**：它只存在于一行没有任何强制力的日志里，
// 万一忽略行为被带到生产，测试也不会拦（http_tests 的 4 条 401 断言反而会永久红）。
// 现在改成显式、可审计、可测试的开关，并在启动时明确打出当前姿态。
const bool open_api=[](){const char* v=std::getenv("WEB_OPEN_API");return v&&(std::strcmp(v,"1")==0||std::strcmp(v,"on")==0);}();
if(open_api){
  if(!token.empty())token.clear();
  std::cerr<<"[security] 警告：WEB_OPEN_API=1 → 管理 API 无令牌校验（仅测试部署；生产不得设置）\n";
}else if(!token.empty()){
  std::cout<<"[security] token 校验已启用（--token 已设置）\n";
}else{
  std::cerr<<"[security] 警告：未设置 --token → 管理 API 无令牌校验\n";
}
std::signal(SIGINT,stop_signal);
  std::set_terminate(terminate_handler);
  // 音频转发开关：默认关（原生播放），CP_MEDIA_AUDIO=1 才打开浏览器通路。
  if(const char* audio_env=std::getenv("CP_MEDIA_AUDIO"))g_media_audio_enabled=(std::strcmp(audio_env,"1")==0||std::strcmp(audio_env,"on")==0);
  std::cout<<"browser audio forwarding: "<<(g_media_audio_enabled?"enabled (debug)":"disabled (native playback)")<<std::endl;
  // 管理页面资源；缺资源时退回内置页面并在日志里说明。
  if(assets_dir.empty()){const char* env=std::getenv("WEB_ASSETS");if(env&&*env)assets_dir=env;else assets_dir="assets";}
  {std::string page_error;g_page=build_page(assets_dir,page_error);if(g_page.empty())std::cerr<<"[warn] management page assets unusable ("<<page_error<<"); serving the built-in page\n";else std::cout<<"management page loaded from "<<assets_dir<<" ("<<g_page.size()<<" bytes)\n";}std::signal(SIGTERM,stop_signal);std::signal(SIGPIPE,SIG_IGN);
// ── 大对象一律堆分配，不要放在 main() 的栈上 ───────────────────────────────
// 这些对象合计远超主线程默认 8 MiB 栈：实测 RealMediaStore 3.25 MiB（下面还有第二个）、
// CatPlayMediaClient 1.2 MiB、SessionCore 0.41 MiB。\n// 放在 main() 里当局部值对象会在【构造期间】就击穿栈，现象是 /health 无响应、退出码 139；
// 而 bind/listen 在更后面的行，根本执行不到 —— 看起来像"服务存活却从不监听"，
// 其实是从未走到那一行。曾因此在集成阶段误判为"另一个 bug"，浪费时间。
// 写法：unique_ptr 持有 + 引用绑定，下游所有 `core.`/`media.` 写法一行不改。
// 新增任何大对象请沿用同一写法；main() 里不要留 sizeof > 64 KiB 的值对象。
auto core_owner=std::make_unique<SessionCore>();SessionCore& core=*core_owner;
auto media_owner=std::make_unique<RealMediaStore>();RealMediaStore& media=*media_owner;
auto media_client_owner=std::make_unique<CatPlayMediaClient>(core,media);CatPlayMediaClient& media_client=*media_client_owner;media_client.start();
auto local_owner=std::make_unique<LocalDesktopBackend>(core);LocalDesktopBackend& local=*local_owner;
auto synthetic_owner=std::make_unique<SyntheticWirelessCarPlayBackend>(core);SyntheticWirelessCarPlayBackend& synthetic=*synthetic_owner;
// 【不启动 local】LocalDesktopBackend 会以 id "local-desktop" 再注册一个 LocalDesktop 源，
// 与下面 Core 自己的桌面（"core-desktop"）重复 —— 管理页上就会冒出两个内置桌面。
// 实测 local-desktop 只产出 41 字节空帧（activeVideo.encoding=none），是空壳；
// 真正渲染内容的是 DesktopRenderer。对象保留（client() 仍会用到 local.refresh()），
// 但不 start()，它就不会注册源。
synthetic.start();
// Core 自己的管理桌面：无输入或多输入未选择时的显示面，内容取自 SessionCore 快照。
auto desktop_owner=std::make_unique<DesktopRenderer>(core);DesktopRenderer& desktop=*desktop_owner;core.set_desktop_source("core-desktop");desktop.start();
#ifdef ZERO2W_WITH_CARLIFE
  // 无线 CarLife+ 输入。默认让车机在 7200/8200/... 监听（被动，不改网卡）；
  // 设置 CARLIFE_PHONE_IP 则改为车机主动连手机。适配器不使用 SDL：
  // 它只做协议与媒体转发，画面由浏览器侧解码。
  // CarLife 输入默认不自动启动：只有显式设置 CARLIFE_ENABLE=1 时才建会话。
  // 这样 Web/测试环境不会被动打开 CarLife 的 HU 端口（7200/8200/…）；
  // 开发板上由 service 显式开启。
  const char* carlife_enable=std::getenv("CARLIFE_ENABLE");
  const bool carlife_wanted=carlife_enable&&(std::strcmp(carlife_enable,"1")==0||std::strcmp(carlife_enable,"on")==0);
  auto carlife_media_owner=std::make_unique<RealMediaStore>();RealMediaStore& carlife_media=*carlife_media_owner;
  carlife::SessionConfig carlife_config;
  // 蓝牙名必须与热点名一致地统一为 Zero2W。
  // 原因：蓝牙引导时 BtLink::advertiseBlueZ() 会拿 config_.btName 去执行
  //   hciconfig hci0 name / btmgmt name，
  // 而 SessionConfig::btName 的默认值是 "CarLife-HU" —— 应用一启动就把适配器
  // 改名成 CarLife-HU，盖掉 /etc/systemd/system/zero2w-bt-pairing.service 里
  // 设好的 Zero2W，手机上看到的名字就跟着变了。
  // 可用 CARLIFE_BT_NAME 覆盖。
  if(const char* bt_name=std::getenv("CARLIFE_BT_NAME")){ if(*bt_name) carlife_config.btName=bt_name; }
  else carlife_config.btName="Zero2W";
  // 车机蓝牙 MAC：同上，必须上报【真实】地址（详见 SessionConfig::btMac 的注释）。
  if(const char* bt_mac=std::getenv("CARLIFE_BT_MAC")){ if(*bt_mac) carlife_config.btMac=bt_mac; }
  // 只在真的要跑 CarLife 时才探测：btMac 只在 CarLife 会话上报 HU_INFO 时用到，
  // 不跑 CarLife 时探测纯属白付启动成本（且它正是启动卡死的来源）。
  // 注：这不能替代上面的 `timeout -k` —— 板上 CARLIFE_ENABLE=1 时仍然会走探测，
  // 那时靠的是 -k 保证不被 bluetoothd 异常拖死；正常板上 bluetoothd 在跑，探测 <0.5s。
  if(carlife_wanted&&carlife_config.btMac.empty()) carlife_config.btMac=detectAdapterBtMac();
  std::cout<<"[carlife] HU btName="<<carlife_config.btName<<" btMAC="
           <<(carlife_config.btMac.empty()?std::string("(未探测到，将不上报)"):carlife_config.btMac)<<std::endl;
  carlife::CarLifeInputAdapter carlife_input(core,carlife_media,carlife_config);
  if(carlife_wanted){
    const char* phone=std::getenv("CARLIFE_PHONE_IP");
    const char* bt=std::getenv("CARLIFE_BT");
    const bool bt_wanted=bt&&(std::strcmp(bt,"1")==0||std::strcmp(bt,"on")==0);
    if(phone&&*phone)carlife_input.connectToPhone(phone);
    else if(bt_wanted){
      const char* channel=std::getenv("CARLIFE_BT_CHANNEL");
      const char* wifi=std::getenv("CARLIFE_WIFI_NAME");
      carlife_input.enableBluetoothBootstrap(channel&&*channel?std::atoi(channel):1,
        wifi&&*wifi?std::string(wifi):std::string("zero2w-CarLife"));
      std::cout<<"Wireless CarLife+ bluetooth bring-up enabled (SPP channel "<<(channel&&*channel?channel:"1")<<")"<<std::endl;
    }
    else carlife_input.listenOnHuPorts();
    g_carlife_adapter=&carlife_input;g_carlife_media=&carlife_media;
    carlife_input.start();
    std::cout<<"Wireless CarLife+ input enabled (source carlife-hu)"<<std::endl;
  }else{
    std::cout<<"Wireless CarLife+ input disabled (set CARLIFE_ENABLE=1 to enable)"<<std::endl;
  }
#endif
int s=socket(AF_INET,SOCK_STREAM,0),yes=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(uint16_t(port));addr.sin_addr=parsed;if(::bind(s,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))||listen(s,16)){std::perror("listen");synthetic.stop();local.stop();return 1;}std::cout<<"Synthetic WirelessCarPlay test UI: http://"<<address<<':'<<port<<" (no iPhone connected)\n";{HttpWorkers clients(core,media,synthetic,local,desktop,token,display_config_path);while(alive){fd_set set;FD_ZERO(&set);FD_SET(s,&set);timeval tv{1,0};if(select(s+1,&set,nullptr,nullptr,&tv)>0){int c=accept(s,nullptr,nullptr);if(c>=0&&!clients.submit(c))close(c);}}}close(s);
#ifdef ZERO2W_WITH_CARLIFE
  carlife_input.stop();
  g_carlife_adapter=nullptr;g_carlife_media=nullptr;
#endif
desktop.stop();media_client.stop();synthetic.stop();local.stop();return 0;}
