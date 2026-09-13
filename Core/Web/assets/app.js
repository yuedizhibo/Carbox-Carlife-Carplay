'use strict';
/* zero2w 管理后台前端。
 *
 * 设计要点：
 *  - 只渲染「活跃输入」的媒体；其它输入只显示状态与计数，不拉流。
 *  - 媒体路径按来源限定：/media/{source}/... ，因此 CarPlay 与 CarLife 复用同一套解码通路。
 *  - 记录格式是 CPMF（64 字节头 + 载荷），与 Core/Convert 的编码一致。
 */

const $ = (id) => document.getElementById(id);
const MAGIC = 0x43504d46;          // 'CPMF'
const T = { VIDEO_CONFIG: 17, VIDEO_FRAME: 18, AUDIO_START: 32, AUDIO_CHUNK: 33, AUDIO_GAIN: 35 };
const KF = 1, DISC = 2;

let token = '';
const H = (extra) => Object.assign(token ? { 'X-Auth-Token': token } : {}, extra || {});

/* ---------------- CPMF 解析 ---------------- */
function records(buf) {
  const view = new DataView(buf), out = [];
  let o = 0;
  while (o + 64 <= buf.byteLength) {
    if (view.getUint32(o) !== MAGIC || view.getUint8(o + 4) !== 1 || view.getUint16(o + 6) !== 64) {
      throw new Error('bad media record header');
    }
    const n = view.getUint32(o + 40);
    if (n > 1 << 20 || o + 64 + n > buf.byteLength) throw new Error('truncated media record');
    out.push({
      t: view.getUint16(o + 8), flags: view.getUint16(o + 10), stream: view.getUint32(o + 12),
      seq: Number(view.getBigUint64(o + 24)), pts: Number(view.getBigUint64(o + 32)),
      p0: view.getUint32(o + 44), p1: view.getUint32(o + 48),
      p2: view.getUint32(o + 52), p3: view.getUint32(o + 56),
      data: new Uint8Array(buf, o + 64, n),
    });
    o += 64 + n;
  }
  if (o !== buf.byteLength) throw new Error('trailing media bytes');
  return out;
}

/* 从 Annex-B 参数集里取出 SPS 以构造 codec 字符串（avc1.PPCCLL）。 */
function avcCodec(cfg) {
  for (let i = 0; i + 7 < cfg.length; i++) {
    if (cfg[i] === 0 && cfg[i + 1] === 0 && (cfg[i + 2] === 1 || (cfg[i + 2] === 0 && cfg[i + 3] === 1))) {
      const j = cfg[i + 2] === 1 ? i + 3 : i + 4;
      if ((cfg[j] & 31) === 7 && j + 3 < cfg.length) {
        return 'avc1.' + [cfg[j + 1], cfg[j + 2], cfg[j + 3]]
          .map((x) => x.toString(16).padStart(2, '0')).join('').toUpperCase();
      }
    }
  }
  throw new Error('参数集里找不到 SPS');
}

/* ---------------- 解码器与流 ---------------- */
function makeRole(name, canvasId, statId) {
  return {
    name, canvas: $(canvasId), stat: $(statId), ctx: $(canvasId).getContext('2d'),
    decoder: null, codec: '', codedW: 0, codedH: 0, streamId: 0, config: null,
    seq: 0, needKey: true, rendered: 0, fpsAt: performance.now(), abort: null,
    streaming: false, pending: new Uint8Array(0), keyframeAt: 0, error: '',
  };
}

const roles = {
  main: makeRole('main', 'mainCanvas', 'mainStats'),
  alt: makeRole('alt', 'altCanvas', 'altStats'),
};
let activeSource = '';
let sessionId = 0;

function teardown(role, why) {
  if (role.decoder) { try { role.decoder.close(); } catch (_) {} }
  role.decoder = null; role.seq = 0; role.needKey = true; role.pending = new Uint8Array(0);
  role.config = null; role.streamId = 0; role.error = why || '';
}

function setupDecoder(role, cfg, w, h) {
  const codec = avcCodec(cfg);
  if (role.decoder && role.codec === codec && role.codedW === w && role.codedH === h) return;
  if (role.decoder) { try { role.decoder.close(); } catch (_) {} role.decoder = null; }
  role.codec = codec; role.codedW = w; role.codedH = h;
  role.decoder = new VideoDecoder({
    output: (frame) => {
      role.canvas.width = frame.codedWidth;
      role.canvas.height = frame.codedHeight;
      role.ctx.drawImage(frame, 0, 0);
      frame.close();
      role.rendered++;
      const now = performance.now();
      if (now - role.fpsAt >= 1000) {
        role.stat.textContent = (role.rendered * 1000 / (now - role.fpsAt)).toFixed(1) + ' fps';
        role.rendered = 0; role.fpsAt = now;
      }
    },
    error: (e) => { role.error = e.message; teardown(role, e.message); requestKeyframe(role); },
  });
  try {
    role.decoder.configure({ codec, codedWidth: w, codedHeight: h, optimizeForLatency: true });
  } catch (e) {
    role.error = e.message; teardown(role, e.message);
  }
}

async function requestKeyframe(role) {
  if (!role.streamId || !activeSource) return;
  try {
    await fetch('/api/media/keyframe', {
      method: 'POST', headers: H({ 'Content-Type': 'application/x-www-form-urlencoded' }),
      body: 'stream_id=' + role.streamId,
    });
  } catch (_) {}
}

function consumeVideo(role, recs) {
  for (const r of recs) {
    if (r.t === T.VIDEO_CONFIG) {
      role.config = r;
      if (r.p0 === 1) setupDecoder(role, r.data, r.p1, r.p2);
      role.streamId = r.stream;
    }
  }
  for (const f of recs) {
    if (f.t !== T.VIDEO_FRAME) continue;
    const key = !!(f.flags & KF);
    if ((f.flags & DISC) && role.streamId) teardown(role, 'discontinuity');
    if (!key && role.seq && f.seq !== role.seq + 1) { role.needKey = true; requestKeyframe(role); }
    if (role.needKey && !key) continue;
    if (key) role.needKey = false;
    if (f.seq <= role.seq) continue;
    if (!role.decoder || role.decoder.state !== 'configured') continue;
    // 队列积压时【只暂停喂帧，不要拆解码器】：拆掉会强制重等关键帧（一次往返），
    // 而画面切换（码率爆发）正是队列最易积压的时刻 —— 这就是“切画面就掉到 1”的放大机制。
    if (role.decoder.decodeQueueSize > 60) return;
    const cfg = role.config;
    const payload = key && cfg && cfg.data.length
      ? (() => { const u = new Uint8Array(cfg.data.length + f.data.length); u.set(cfg.data); u.set(f.data, cfg.data.length); return u; })()
      : f.data;
    try {
      role.decoder.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: f.pts, data: payload }));
      role.seq = f.seq;
    } catch (e) { role.error = e.message; teardown(role, e.message); requestKeyframe(role); }
  }
}

function streamRecords(role, chunk) {
  const all = new Uint8Array(role.pending.length + chunk.length);
  all.set(role.pending); all.set(chunk, role.pending.length);
  if (all.length > 2 << 20) throw new Error('stream buffer overflow');
  const recs = [];
  const view = new DataView(all.buffer);
  let o = 0;
  while (o + 64 <= all.length) {
    if (view.getUint32(o) !== MAGIC || view.getUint8(o + 4) !== 1 || view.getUint16(o + 6) !== 64) {
      throw new Error('bad stream record');
    }
    const n = view.getUint32(o + 40);
    if (n > 1 << 20) throw new Error('oversize stream record');
    if (o + 64 + n > all.length) break;
    recs.push({
      t: view.getUint16(o + 8), flags: view.getUint16(o + 10), stream: view.getUint32(o + 12),
      seq: Number(view.getBigUint64(o + 24)), pts: Number(view.getBigUint64(o + 32)),
      p0: view.getUint32(o + 44), p1: view.getUint32(o + 48),
      p2: view.getUint32(o + 52), p3: view.getUint32(o + 56),
      data: new Uint8Array(all.buffer, o + 64, n),
    });
    o += 64 + n;
  }
  role.pending = all.slice(o);
  return recs;
}

async function runVideoStream(role) {
  // 不要求先知道 streamId：服务端自己知道当前流的 id，并会在开流时主动请求关键帧。
  // （之前卡在 !role.streamId 上，与轮询 204 互相锁死，导致流永远开不起来 —— 黑屏的直接原因。）
  if (role.streaming || svgMode || !activeSource) return;
  role.streaming = true;
  role.abort = new AbortController();
  role.pending = new Uint8Array(0);
  role.needKey = true;
  try {
    const url = `/media/${encodeURIComponent(activeSource)}/video/${role.name}/stream`;
    const res = await fetch(url, { headers: H(), signal: role.abort.signal });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const reader = res.body.getReader();
    for (;;) {
      const part = await reader.read();
      if (part.done) break;
      consumeVideo(role, streamRecords(role, part.value));
    }
    role.error = '流已结束';
  } catch (e) {
    if (e.name !== 'AbortError') role.error = e.message;
  } finally {
    role.streaming = false; role.abort = null;
    // 重连条件不能依赖 streamId（它可能一直没学到），否则一次失败就永久停流。
    if (activeSource && !svgMode) setTimeout(() => runVideoStream(role), 400);
  }
}

/* ---------------- 音频 ---------------- */
let audioCtx = null, audioAbort = null, audioOn = false, audioPending = new Uint8Array(0);

// SVG 帧通路：本地桌面等源产出的是 SVG（几 KB），不能走 WebCodecs 解码，
// 由浏览器把 SVG 画进画布。只在内容变化时重建位图，避免无谓重绘。
// 注：真正的内存开销在浏览器侧的一张画布位图（1280x720x4 约 3.5 MB），
// 板上只发出几 KB 文本，对 1 GB 内存不构成压力。
let svgMode = false, svgLast = '', svgTimer = null;
async function svgLoopOnce() {
  svgTimer = null;
  if (!svgMode) return;
  try {
    const res = await fetch('/media/frame.svg', { headers: H() });
    if (res.ok) {
      const text = await res.text();
      if (text !== svgLast) {
        svgLast = text;
        const url = URL.createObjectURL(new Blob([text], { type: 'image/svg+xml' }));
        const img = new Image();
        img.onload = () => {
          const cv = roles.main.canvas;
          cv.width = img.naturalWidth || 1280;
          cv.height = img.naturalHeight || 720;
          roles.main.ctx.drawImage(img, 0, 0);
          URL.revokeObjectURL(url);
          $('mainStats').textContent = 'SVG';
        };
        img.onerror = () => { URL.revokeObjectURL(url); $('mediaStatus').textContent = 'SVG 帧渲染失败'; };
        img.src = url;
      }
    }
  } catch (_) {}
  if (svgMode) svgTimer = setTimeout(svgLoopOnce, 1000);
}
function enterSvgMode(on) {
  if (on === svgMode) return;
  svgMode = on;
  if (on) {
    teardown(roles.main, 'svg');
    teardown(roles.alt, 'svg');
    if (audioAbort) { audioAbort.abort(); audioAbort = null; }
    $('mediaStatus').textContent = '当前输入是本地桌面（SVG 帧），非 H.264 流';
    svgLast = '';
    if (!svgTimer) svgLoopOnce();
  } else if (svgTimer) {
    clearTimeout(svgTimer);
    svgTimer = null;
  }
}
const audioStreams = {};
let duckGain = 1;

// ---- 照 react-carplay / pcm-ringbuf-player 的做法实现音频播放 ----
// 依据仓库内对照实现：Reference/react-carplay/src/renderer/public/audio.worklet.js
//（源自 padenot/ringbuf.js，MPL-2.0，见该目录 RingBuffer_LICENSE.txt）。
// 核心差别：不再“每 10ms 预约一个 AudioBufferSourceNode”，而是
// 主线程写 SharedArrayBuffer 环形缓冲、AudioWorklet 按 128 帧渲染量子【连续拉取】。
// 这样就没有排程边界、没有浮点漂移、欠载只输出静音而从不重置。
const PCM_WORKLET_SRC = `
class RingBuffReader {
  constructor(buffer) {
    const storageSize = (buffer.byteLength - 8) / Int16Array.BYTES_PER_ELEMENT;
    this.storage = new Int16Array(buffer, 8, storageSize);
    this.writePointer = new Uint32Array(buffer, 0, 1);
    this.readPointer = new Uint32Array(buffer, 4, 1);
  }
  readTo(array) {
    const readPos = Atomics.load(this.readPointer, 0);
    const writePos = Atomics.load(this.writePointer, 0);
    const available = (writePos + this.storage.length - readPos) % this.storage.length;
    if (available < array.length) return 0;
    const first = Math.min(this.storage.length - readPos, array.length);
    const second = array.length - first;
    for (let i = 0; i < first; i++) array[i] = this.storage[readPos + i];
    for (let i = 0; i < second; i++) array[first + i] = this.storage[i];
    Atomics.store(this.readPointer, 0, (readPos + array.length) % this.storage.length);
    return array.length;
  }
}
class PcmWorkletProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { sab, channels } = options.processorOptions;
    this.channels = channels;
    this.reader = new RingBuffReader(sab);
    this.out = new Int16Array(128 * channels);
    this.under = false;
  }
  process(_, outputs) {
    const oc = outputs[0];
    if (this.reader.readTo(this.out) === 0) {
      if (!this.under) console.debug('audio underflow');
      this.under = true;
      return true;
    }
    this.under = false;
    for (let i = 0; i < 128; i++) {
      for (let ch = 0; ch < this.channels; ch++) oc[ch][i] = this.out[i * this.channels + ch] / 32768;
    }
    return true;
  }
}
registerProcessor('pcm-worklet', PcmWorkletProcessor);
`;
const AUDIO_RING_FRAMES = 16384;   // 每流约 370ms@44.1k，用于吸收抖动
function createRingWriter(channels) {
  const total = AUDIO_RING_FRAMES * channels;
  const sab = new SharedArrayBuffer(8 + total * 2);
  const storage = new Int16Array(sab, 8, total);
  const write = new Uint32Array(sab, 0, 1);
  const read = new Uint32Array(sab, 4, 1);
  return {
    sab, channels,
    push(pcm) {
      const wp = Atomics.load(write, 0);
      const rp = Atomics.load(read, 0);
      const free = (rp + total - wp - 1) % total;
      const n = Math.min(pcm.length, free);
      for (let i = 0; i < n; i++) storage[(wp + i) % total] = pcm[i];
      Atomics.store(write, 0, (wp + n) % total);
      return n;
    },
  };
}
const audioPlayers = new Map();     // key = rate:type → { gain, writer, type }
let workletReady = null;
function ensureWorklet() {
  if (!workletReady) {
    const url = URL.createObjectURL(new Blob([PCM_WORKLET_SRC], { type: 'application/javascript' }));
    workletReady = audioCtx.audioWorklet.addModule(url).then(() => { URL.revokeObjectURL(url); });
  }
  return workletReady;
}
// 音量：媒体 1.0 / 导航 0.5，并支持带时长的渐变（CarPlay 的 ducking 语义）。
function setPlayerVolume(p, value, durationMs) {
  const at = audioCtx.currentTime;
  const g = p.gain.gain;
  g.cancelScheduledValues(at);
  g.setValueAtTime(g.value, at);
  g.linearRampToValueAtTime(value, at + Math.max(0, Math.min(60, (durationMs || 0) / 1000)));
}
async function getAudioPlayer(rate, channels, type) {
  // 上下文采样率必须等于源采样率，否则 worklet 会变速播放
  if (audioCtx.sampleRate !== rate) {
    try { await audioCtx.close(); } catch (_) {}
    audioPlayers.clear();
    for (const k of Object.keys(audioStreams)) delete audioStreams[k];
    audioCtx = new AudioContext({ sampleRate: rate });
    await audioCtx.resume();
    workletReady = null;
    $('mediaStatus').textContent = '音频上下文已按源采样率重建 (' + rate + ' Hz)';
  }
  const key = rate + ':' + type;
  let p = audioPlayers.get(key);
  if (p) return p;
  await ensureWorklet();
  const writer = createRingWriter(channels);
  const node = new AudioWorkletNode(audioCtx, 'pcm-worklet', {
    numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [channels],
    processorOptions: { sab: writer.sab, channels },
  });
  const gain = audioCtx.createGain();
  gain.gain.value = type === 1 ? 0.5 : 1.0;
  node.connect(gain);
  gain.connect(audioCtx.destination);
  p = { node, gain, writer, type };
  audioPlayers.set(key, p);
  return p;
}

function consumeAudio(recs) {
  for (const r of recs) {
    if (r.t === T.AUDIO_GAIN) {
      duckGain = Math.max(0, Math.min(1, r.p1 / 1000000));
      // 带时长的音量渐变（r.p0 = 持续毫秒）：这才是 CarPlay 的 ducking 语义，
      // 导航播报时把媒体音平滑压低，而不是两条流各拉满音量。
      for (const p of audioPlayers.values()) if (p.type === 2) setPlayerVolume(p, duckGain, r.p0);
    }
  }
  for (const c of recs) {
    if (c.t !== T.AUDIO_CHUNK) continue;
    const start = recs.find((x) => x.t === T.AUDIO_START && x.stream === c.stream);
    let s = audioStreams[c.stream];
    if (!s && start) {
      // 每条流一个独立播放器（按 采样率+音频类型 区分）：这是参考实现的核心结构，
      // 导航音(type 1)与媒体音(type 2)各自持有音量，避免两条流简单叠加“打架”。
      s = audioStreams[c.stream] = { seq: 0, rate: start.p1, ch: Math.max(1, start.p2), type: start.p0, player: null };
      getAudioPlayer(start.p1, s.ch, start.p0)
        .then((p) => { s.player = p; })
        .catch((e) => { $('mediaStatus').textContent = '音频初始化失败: ' + e.message; });
    }
    if (!s) continue;
    if (c.seq <= s.seq) continue;
    if (!s.player) continue;
    // 交织 int16 → 环形缓冲。写不进去只说明消费端落后，丢掉这一块即可：
    // 不阻塞、不重置、不动已在播的内容 —— 欠载由 worklet 自然输出静音。
    const frames = c.data.byteLength >> 1;
    const pcm = new Int16Array(frames);
    const dv = new DataView(c.data.buffer, c.data.byteOffset, c.data.byteLength);
    for (let i = 0; i < frames; i++) pcm[i] = dv.getInt16(i * 2, true);
    s.player.writer.push(pcm);
  }
}

// 流式解析：chunk 边界与记录边界无关，必须容忍末尾的半条记录（留到下次）。
// 之前音频路径复用了“整包必须完整”的 records()，它会在遇到半条记录时
// 抛出 'trailing media bytes'，于是流刚连上就中断重连 ——
// 表现就是“浏览器完全没有音频输出”。
function parseStreamRecords(buf) {
  const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const out = [];
  let o = 0;
  while (o + 64 <= buf.byteLength) {
    if (view.getUint32(o) !== MAGIC || view.getUint8(o + 4) !== 1 || view.getUint16(o + 6) !== 64) {
      throw new Error('bad stream record header');
    }
    const n = view.getUint32(o + 40);
    if (n > (1 << 20)) throw new Error('oversize stream record');
    if (o + 64 + n > buf.byteLength) break;   // 半条记录，留到下次
    out.push({
      t: view.getUint16(o + 8), flags: view.getUint16(o + 10), stream: view.getUint32(o + 12),
      seq: Number(view.getBigUint64(o + 24)), pts: Number(view.getBigUint64(o + 32)),
      p0: view.getUint32(o + 44), p1: view.getUint32(o + 48),
      p2: view.getUint32(o + 52), p3: view.getUint32(o + 56),
      data: new Uint8Array(buf.buffer, buf.byteOffset + o + 64, n),
    });
    o += 64 + n;
  }
  return { records: out, consumed: o };
}

async function runAudioStream() {
  if (audioAbort || !audioOn || !audioCtx || !activeSource) return;
  audioAbort = new AbortController();
  audioPending = new Uint8Array(0);
  try {
    const res = await fetch(`/media/${encodeURIComponent(activeSource)}/audio/stream`, { headers: H(), signal: audioAbort.signal });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const reader = res.body.getReader();
    for (;;) {
      const part = await reader.read();
      if (part.done) break;
      // 必须用容忍半条记录的解析（见 parseStreamRecords 的说明）。
      const all = new Uint8Array(audioPending.length + part.value.length);
      all.set(audioPending); all.set(part.value, audioPending.length);
      const parsed = parseStreamRecords(all);
      audioPending = all.slice(parsed.consumed);
      consumeAudio(parsed.records);
    }
  } catch (e) {
    if (e.name !== 'AbortError') $('mediaStatus').textContent = '音频流: ' + e.message;
  } finally {
    audioAbort = null;
    if (audioOn && activeSource) setTimeout(runAudioStream, 500);
  }
}

/* ---------------- 触控与硬键 ---------------- */
let pointerId = null, pendingMove = null, rafId = 0, touchBusy = false;
const touchQueue = [];

function canvasPoint(ev) {
  const c = roles.main.canvas, r = c.getBoundingClientRect();
  return {
    x: Math.max(0, Math.min(c.width - 1, Math.floor((ev.clientX - r.left) * c.width / r.width))),
    y: Math.max(0, Math.min(c.height - 1, Math.floor((ev.clientY - r.top) * c.height / r.height))),
  };
}

async function pumpTouch() {
  if (touchBusy) return;
  touchBusy = true;
  try {
    while (touchQueue.length) {
      const t = touchQueue.shift();
      const body = new URLSearchParams({ type: 'touch', phase: t.phase, x: String(t.x), y: String(t.y) });
      const res = await fetch('/api/control', { method: 'POST', headers: H({ 'Content-Type': 'application/x-www-form-urlencoded' }), body });
      if (!res.ok) throw new Error('HTTP ' + res.status);
    }
  } catch (e) {
    touchQueue.length = 0;
    $('mediaStatus').textContent = '触控失败: ' + e.message;
  } finally { touchBusy = false; }
}

function sendTouch(phase, ev) {
  const p = canvasPoint(ev);
  const last = touchQueue[touchQueue.length - 1];
  if (phase === 'move' && last && last.phase === 'move') touchQueue[touchQueue.length - 1] = { phase, ...p };
  else if (touchQueue.length < 8) touchQueue.push({ phase, ...p });
  else if (phase !== 'move') touchQueue.length = 0, touchQueue.push({ phase, ...p });
  pumpTouch();
}

const cv = roles.main.canvas;
cv.addEventListener('pointerdown', (e) => {
  if (pointerId !== null) return;
  pointerId = e.pointerId; cv.setPointerCapture(e.pointerId);
  sendTouch('down', e); e.preventDefault();
});
cv.addEventListener('pointermove', (e) => {
  if (e.pointerId !== pointerId) return;
  pendingMove = e;
  if (!rafId) rafId = requestAnimationFrame(() => { rafId = 0; if (pendingMove) { sendTouch('move', pendingMove); pendingMove = null; } });
  e.preventDefault();
});
function endTouch(e) {
  if (e.pointerId !== pointerId) return;
  pendingMove = null; sendTouch('up', e); pointerId = null; e.preventDefault();
}
cv.addEventListener('pointerup', endTouch);
cv.addEventListener('pointercancel', endTouch);
cv.addEventListener('contextmenu', (e) => e.preventDefault());

async function sendKey(name) {
  try {
    const res = await fetch('/api/control', {
      method: 'POST', headers: H({ 'Content-Type': 'application/x-www-form-urlencoded' }),
      body: new URLSearchParams({ type: 'key', key: name }),
    });
    if (!res.ok) throw new Error('HTTP ' + res.status);
  } catch (e) { $('mediaStatus').textContent = '硬键失败: ' + e.message; }
}
document.querySelectorAll('.key').forEach((b) => b.addEventListener('click', () => sendKey(b.dataset.key)));

/* ---------------- 状态渲染 ---------------- */
function num(v) { return typeof v === 'number' ? v.toLocaleString() : '—'; }

function inputCard(src, st) {
  const el = document.createElement('div');
  el.className = 'card' + (src.id === activeSource ? ' active' : '');
  const detail = st ? (st.detail || '') : '';
  const rows = [];
  rows.push(['类型', src.kind]);
  rows.push(['连接', src.connected ? '已连接' : '未连接']);
  if (st) {
    rows.push(['状态', (st.state || st.stateName || '—') + (st.running === false ? '（未运行）' : '')]);
    if (st.display && st.display.width) rows.push(['分辨率', `${st.display.width}×${st.display.height}@${st.display.fps}`]);
    if (st.video) rows.push(['视频', `帧 ${num(st.video.framesReceived)} / 关键帧 ${num(st.video.keyframes)} / ${num(st.video.bytes)} B`]);
    if (st.audio) rows.push(['音频', `媒体 ${num(st.audio.mediaBytes)} B · TTS ${num(st.audio.ttsBytes)} B · 块 ${num(st.audio.chunks)}`]);
    if (st.control) rows.push(['控制', `发出 ${num(st.control.sent)} / 丢弃 ${num(st.control.dropped)}`]);
    if (st.microphone) rows.push(['麦克风', `请求 ${num(st.microphone.requests)} / 帧 ${num(st.microphone.frames)}`]);
    if (st.screens) {
      for (const s of st.screens) rows.push([`${s.role === 'main' ? '主屏' : '副屏'}`, `${s.state} · 队列 ${s.queuedFrames}`]);
    }
    if (st.lifecycle) rows.push(['生命周期', st.lifecycle]);
  }
  el.innerHTML = `<div class="card-head"><span class="card-id">${src.id}</span>
      <span class="tag ${src.connected ? 'ok' : ''}">${src.connected ? '在线' : '离线'}</span></div>
    <dl class="kv">${rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('')}</dl>
    ${detail ? `<div class="hint">${detail}</div>` : ''}
    <div style="margin-top:.5rem"><button class="btn sm">设为活跃输入</button></div>`;
  el.querySelector('button').onclick = () => selectInput(src.id);
  return el;
}

function buildPerInputDetails(state) {
  const map = {};
  const rm = state.realCarPlayMedia;
  if (rm) {
    map['catplay-real'] = {
      screens: (rm.screens || []).map((s) => ({ role: s.role, state: s.state, queuedFrames: s.queuedFrames })),
      lifecycle: (state.realCarPlayStatus || {}).lifecycle,
      detail: `IPC ${rm.ipc} · 会话 ${rm.session} · 视频 ${rm.video}`,
    };
  }
  if (state.carlife) {
    map['carlife-hu'] = {
      state: state.carlife.state, running: state.carlife.running, detail: state.carlife.detail,
      display: state.carlife.display, video: state.carlife.video, audio: state.carlife.audio,
      control: state.carlife.control, microphone: state.carlife.microphone,
    };
  }
  map['local-desktop'] = { state: '内置桌面', detail: '无输入时兜底，提供 SVG 帧与静音音频' };
  map['synthetic-wireless'] = { state: '合成测试源', detail: '仅用于联调，不代表真实手机媒体' };
  return map;
}

async function refresh() {
  let state;
  try {
    const res = await fetch('/api/state', { headers: H() });
    if (res.status === 401) {
      // 未授权不是「页面坏了」：把该做的事直接写在主区和状态栏。
      $('mediaStatus').textContent = '需要访问令牌：请在右上角「访问令牌」粘贴后点「应用令牌」';
      $('inputList').innerHTML =
        '<div class="hint">尚未授权。<br>把访问令牌粘贴到右上角输入框，点「应用令牌」即可。<br>' +
        '也可以直接用 <code>http://localhost:18080/#token=&lt;令牌&gt;</code> 打开。</div>';
      $('activeName').textContent = '未授权';
      return;
    }
    state = await res.json();
  } catch (e) { $('mediaStatus').textContent = '状态获取失败: ' + e.message; return; }

  $('rawState').textContent = JSON.stringify(state, null, 2);
  $('modeName').textContent = state.mode === 'manual' ? '手动' : '自动';
  $('activeName').textContent = state.active || '（无）';
  // 帧格式决定渲染通路：SVG 走画布直绘，H.264 走 WebCodecs。
  enterSvgMode(!!(state.activeVideo && state.activeVideo.encoding === 'svg'));

  const sources = state.sources || [];
  $('inputCount').textContent = sources.length;
  const details = buildPerInputDetails(state);
  const list = $('inputList');
  list.innerHTML = '';
  sources.forEach((s) => list.appendChild(inputCard(s, details[s.id])));
  if (!sources.length) list.textContent = '暂无输入';

  const active = state.active || '';
  $('viewerSource').textContent = active || '—';
  $('activeKind').textContent = (sources.find((s) => s.id === active) || {}).kind || '—';
  if (active !== activeSource) { switchSource(active); }
  if (state.realCarPlayMedia && state.realCarPlayMedia.sessionId !== sessionId) {
    sessionId = state.realCarPlayMedia.sessionId || 0;
    if (activeSource === 'catplay-real') { teardown(roles.main, '新会话'); teardown(roles.alt, '新会话'); runAll(); }
  }
  if (state.drops) {
    $('mediaStatus').textContent =
      `丢帧 视频 ${state.drops.video} · 音频 ${state.drops.audio} · 控制 ${state.drops.control}` +
      (activeSource ? ` ｜ 正在观看 ${activeSource}` : '');
  }
}

function switchSource(next) {
  activeSource = next;
  roles.main.streamId = 0; roles.alt.streamId = 0;
  teardown(roles.main, ''); teardown(roles.alt, '');
  for (const k of Object.keys(audioStreams)) { audioStreams[k].nodes.forEach((n) => n.stop()); audioStreams[k].gain.disconnect(); delete audioStreams[k]; }
  if (audioAbort) { audioAbort.abort(); audioAbort = null; }
  runAll();
}

async function runAll() {
  if (!activeSource) return;
  if (!window.VideoDecoder || !window.EncodedVideoChunk) {
    $('mediaStatus').textContent = '此浏览器上下文不支持 WebCodecs。请用 https 或从本地端口转发访问（见下方说明）。';
    return;
  }
  // 先取一次轮询接口，拿到 streamId 后才能开流。
  for (const role of [roles.main, roles.alt]) {
    // 轮询只做一次“预热”：拿到 VideoConfig 就顺手把解码器建好。
    // 204 表示此刻没有可发的帧（ring 里还没关键帧），这是正常态，绝不能因此就不开流。
    try {
      const res = await fetch(`/media/${encodeURIComponent(activeSource)}/video/${role.name}`, { headers: H({ 'X-Media-After': '0' }) });
      if (res.ok && res.status !== 204) consumeVideo(role, records(await res.arrayBuffer()));
    } catch (_) {}
    runVideoStream(role);
  }
  if (audioOn) runAudioStream();
}

/* ---------------- 显示配置 ---------------- */
async function loadDisplay() {
  try {
    const res = await fetch('/api/display-config', { headers: H() });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const c = await res.json();
    $('mw').value = c.main.width; $('mh').value = c.main.height; $('mf').value = c.main.fps;
    $('ae').checked = !!c.alt.enabled;
    $('aw').value = c.alt.width; $('ah').value = c.alt.height; $('af').value = c.alt.fps;
    $('displayStatus').textContent = '已加载，改动在下一次 CarPlay 会话生效';
  } catch (e) { $('displayStatus').textContent = '不可用: ' + e.message; }
}
$('displayForm').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const body = new URLSearchParams({
    main_width: $('mw').value, main_height: $('mh').value, main_fps: $('mf').value,
    alt_enabled: $('ae').checked ? '1' : '0',
    alt_width: $('aw').value, alt_height: $('ah').value, alt_fps: $('af').value,
  });
  try {
    const res = await fetch('/api/display-config', { method: 'POST', headers: H({ 'Content-Type': 'application/x-www-form-urlencoded' }), body });
    const j = await res.json();
    if (!res.ok) throw new Error(j.error || ('HTTP ' + res.status));
    $('displayStatus').textContent = '已保存，重连 CarPlay 后生效';
  } catch (e) { $('displayStatus').textContent = '被拒绝: ' + e.message; }
});

async function selectInput(id) {
  try {
    const res = await fetch('/api/select', {
      method: 'POST', headers: H({ 'Content-Type': 'application/x-www-form-urlencoded' }),
      body: new URLSearchParams({ mode: 'manual', source: id }),
    });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    await refresh();
  } catch (e) { $('mediaStatus').textContent = '切换失败: ' + e.message; }
}

$('btnAuto').onclick = async () => {
  await fetch('/api/select', { method: 'POST', headers: H({ 'Content-Type': 'application/x-www-form-urlencoded' }), body: 'mode=automatic' });
  refresh();
};
$('btnManual').onclick = () => { if (activeSource) selectInput(activeSource); };
$('btnToken').onclick = () => { token = $('token').value.trim(); loadDisplay(); refresh(); };
$('audioOn').onchange = async (e) => {
  audioOn = e.target.checked;
  if (audioOn) {
    if (!window.AudioContext) { $('mediaStatus').textContent = '此浏览器不支持 Web Audio'; audioOn = false; e.target.checked = false; return; }
    audioCtx = audioCtx || new AudioContext();
    await audioCtx.resume();
    runAudioStream();
  } else if (audioAbort) { audioAbort.abort(); audioAbort = null; }
};

if (!isSecureContext) {
  $('mediaStatus').textContent = '当前不是安全上下文（http 访问非 localhost），WebCodecs 可能不可用。建议：ssh -L 18080:127.0.0.1:18080 root@<板子IP> 后打开 http://localhost:18080';
}

// 支持 #token=xxx 直接进入：避免首次打开因为没令牌而看起来像「页面坏了」。
if (location.hash.startsWith('#token=')) {
  token = decodeURIComponent(location.hash.slice(7));
  $('token').value = token;
}

setInterval(refresh, 1000);
refresh();
loadDisplay();

/* ═══════════════════════════════════════════════════════════════════════════
   新契约 UI（L3）
   字段/端点对应 Core/MainMenu/include/core/session_core.hpp、Core/Web/DESIGN.md。
   全为追加：不改动上面的 refresh()/视频/音频/触控逻辑，只复用全局 $ / H / token
   / activeSource / roles，另起一个 1s 轮询只读 /api/state 的新字段。
   ═══════════════════════════════════════════════════════════════════════════ */
(function () {
  'use strict';

  const post = async (path, params) => {
    const res = await fetch(path, {
      method: 'POST',
      headers: H({ 'Content-Type': 'application/x-www-form-urlencoded' }),
      body: new URLSearchParams(params).toString(),
    });
    let json = null;
    try { json = await res.json(); } catch (_) { /* 非 JSON 也无妨 */ }
    return { ok: res.ok, status: res.status, json };
  };
  const put = (id, text) => { const el = $(id); if (el) el.textContent = text; };
  const fmtTime = (ms) => {
    if (!ms || ms < 0) return '0:00';
    const s = Math.floor(ms / 1000);
    return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
  };

  /* ── 元数据 / 封面 / 歌词 / 进度 ───────────────────────────────────── */
  let artRev = -1, lrcRev = -1, lrcLines = [], lrcIndex = -1, seeking = false, lastMedia = null;

  // LRC 解析：支持 [mm:ss.xx] 与 [mm:ss:xx]；一行多时间戳展开成多条。
  function parseLrc(text) {
    const out = [];
    for (const raw of text.split('\n')) {
      const m = raw.match(/^\s*((?:\[\d+:\d+(?:[.:]\d+)?\])+)(.*)$/);
      if (!m) continue;
      const body = m[2].trim();
      if (!body) continue;
      for (const stamp of m[1].matchAll(/\[(\d+):(\d+(?:[.:]\d+)?)\]/g)) {
        const min = parseInt(stamp[1], 10);
        const sec = parseFloat(stamp[2].replace(':', '.'));
        out.push({ t: (min * 60 + sec) * 1000, text: body });
      }
    }
    out.sort((a, b) => a.t - b.t);
    return out;
  }

  function renderMedia(m) {
    put('npTitle', m.title || '—');
    put('npArtist', m.artist || '—');
    put('npAlbum', m.album || '—');
    put('npApp', m.app || '—');
    put('npTrack', m.trackCount ? `${m.trackNumber || 0}/${m.trackCount}` : '—');
    put('npState', m.valid ? (m.playing ? '播放中' : '已暂停') : '无元数据');
  }

  // 封面：URL 自带 revision，换曲换封面时 URL 变，浏览器缓存自然失效。
  async function ensureArtwork(m) {
    const img = $('npArt'), none = $('npArtNone');
    if (!m.hasArtwork || !m.artUrl) {
      if (artRev !== -1) { artRev = -1; img.hidden = true; img.removeAttribute('src'); }
      none.hidden = false;
      return;
    }
    if (m.artworkRevision === artRev && img.getAttribute('src') === m.artUrl) return;
    artRev = m.artworkRevision;
    img.onload = () => { img.hidden = false; none.hidden = true; };
    img.onerror = () => { img.hidden = true; none.hidden = false; none.textContent = '封面不可用'; };
    img.src = m.artUrl;
  }

  async function ensureLyrics(m) {
    const box = $('npLyrics');
    if (!m.hasLyrics || !m.lyricsUrl) {
      if (lrcRev !== -1) { lrcRev = -1; lrcLines = []; lrcIndex = -1; box.textContent = '无歌词'; }
      return;
    }
    if (m.lyricsRevision === lrcRev) return;
    lrcRev = m.lyricsRevision;
    try {
      const res = await fetch(m.lyricsUrl, { headers: H() });
      if (!res.ok) { box.textContent = '歌词取不到（' + res.status + '）'; return; }
      lrcLines = parseLrc(await res.text());
      lrcIndex = -1;
      box.innerHTML = '';
      if (!lrcLines.length) { box.textContent = '（无时间轴或空歌词）'; return; }
      lrcLines.forEach((l, i) => {
        const d = document.createElement('div');
        d.dataset.i = String(i);
        d.textContent = l.text;          // 用 textContent 而不是 innerHTML：歌词是外部数据
        box.appendChild(d);
      });
    } catch (_) { box.textContent = '歌词加载失败'; }
  }

  function highlightLyric(positionMs) {
    if (!lrcLines.length) return;
    let idx = -1;
    for (let i = 0; i < lrcLines.length; i++) { if (lrcLines[i].t <= positionMs) idx = i; else break; }
    if (idx === lrcIndex) return;
    const box = $('npLyrics');
    const prev = box.querySelector('div.cur');
    if (prev) prev.classList.remove('cur');
    lrcIndex = idx;
    if (idx < 0) return;
    const cur = box.querySelector(`div[data-i="${idx}"]`);
    if (cur) { cur.classList.add('cur'); cur.scrollIntoView({ block: 'nearest' }); }
  }

  function renderProgress(m) {
    put('npPos', fmtTime(m.positionMs));
    put('npDur', fmtTime(m.durationMs));
    if (!seeking) {
      const r = $('npSeek');
      r.value = m.durationMs > 0 ? Math.round((m.positionMs || 0) * 1000 / m.durationMs) : 0;
    }
    highlightLyric(m.positionMs || 0);
  }

  /* ── 主题 / 校准 / safe area / 副屏 ────────────────────────────────── */
  function applyDisplay(d) {
    document.body.classList.toggle('night', d.dayNight === 1);
    const root = document.documentElement.style;
    root.setProperty('--cal-gamma', String(Math.max(0.1, (d.gamma || 100) / 100)));
    root.setProperty('--cal-contrast', String(Math.max(0.1, (d.contrast || 100) / 100)));
    root.setProperty('--cal-sat', String(Math.max(0.1, (d.saturation || 100) / 100)));
    // safe area 按契约是**协商像素**，画面却是 CSS 像素，所以按宽度比例缩放后再内缩。
    const cv = roles.main.canvas;
    const scale = d.width > 0 ? (cv.clientWidth || d.width) / d.width : 1;
    const px = (v) => ((v || 0) * scale).toFixed(1) + 'px';
    root.setProperty('--safe-top', px(d.safeTop));
    root.setProperty('--safe-bottom', px(d.safeBottom));
    root.setProperty('--safe-left', px(d.safeLeft));
    root.setProperty('--safe-right', px(d.safeRight));
    put('cfgActualFps', d.actualFps || '—');
  }

  // 回填表单：跳过正在输入的控件，否则用户一边打字一边被覆盖。
  function fillCfgForm(d) {
    const set = (id, v, isCheck) => {
      const el = $(id);
      if (!el || el === document.activeElement) return;
      if (isCheck) el.checked = !!v;
      else if (el.value !== String(v)) el.value = v;
    };
    set('cfgGamma', d.gamma, false); set('cfgContrast', d.contrast, false); set('cfgSaturation', d.saturation, false);
    set('cfgDayNight', d.dayNight === 1, true);
    set('cfgSafeTop', d.safeTop, false); set('cfgSafeBottom', d.safeBottom, false);
    set('cfgSafeLeft', d.safeLeft, false); set('cfgSafeRight', d.safeRight, false);
    set('cfgAuxEnabled', d.auxEnabled, true);
    set('cfgAuxX', d.auxX, false); set('cfgAuxY', d.auxY, false);
    set('cfgAuxW', d.auxW, false); set('cfgAuxH', d.auxH, false);
    set('cfgTargetFps', d.targetFps || 60, false);
  }

  /* ── 车况 / 电话 / 音频 / 链路 ────────────────────────────────────── */
  const GEARS = ['P', 'R', 'N', 'D'];
  function renderVehicle(v) {
    const ok = $('vehValid');
    ok.textContent = v.valid ? '有数据' : '无数据';
    ok.className = 'tag ' + (v.valid ? 'ok' : '');
    put('vehSpeed', v.valid ? (v.speedKph + ' km/h') : '—');
    put('vehGear', v.valid ? (GEARS[v.gear] || ('#' + v.gear)) : '—');
    put('vehRpm', v.valid ? (v.rpm + ' rpm') : '—');
    put('vehFuel', v.fuelPct >= 0 ? (v.fuelPct + ' %') : '—');
    put('vehRange', v.rangeKm >= 0 ? (v.rangeKm + ' km') : '—');
    put('vehTemp', v.valid ? (v.outsideTempC + ' °C') : '—');
    put('vehGps', (v.latitude || v.longitude) ? (v.latitude.toFixed(5) + ', ' + v.longitude.toFixed(5)) : '—');
    put('vehHeading', typeof v.headingDeg === 'number' ? (v.headingDeg.toFixed(0) + '°') : '—');
    put('vehLights', v.valid ? (v.lights ? '开' : '关') : '—');
    put('vehBrake', v.valid ? (v.parkingBrake ? '拉起' : '松开') : '—');
    put('vehVin', v.vin || '—');
  }

  /* 导航逐向。两条纪律：
   *   1. maneuverCode 是**各协议自己的原始值**（CarLife 的 action 码表由百度导航 App 产生，
   *      参考树里没有任何映射依据），所以只当数字原样展示，绝不在前端展开成转向语义。
   *   2. maneuver 才是我们确定知道的那部分（取值顺序与 Core 契约的 Maneuver 枚举一致），
   *      不在这张表里就退回显示 '#原始值'，不猜。 */
  const MANEUVERS = ['未知', '直行', '稍向左', '左转', '急左转', '稍向右', '右转', '急右转', '掉头',
    '汇入', '左分岔', '右分岔', '环岛', '出口', '到达', '目的地'];
  function meterText(m) {
    if (typeof m !== 'number' || m < 0) return '—';
    return m >= 1000 ? ((m / 1000).toFixed(1) + ' km') : (m + ' m');
  }
  function durationText(sec) {
    if (typeof sec !== 'number' || sec <= 0) return '—';
    const h = Math.floor(sec / 3600), mins = Math.round((sec % 3600) / 60);
    return h ? (h + ' 小时 ' + mins + ' 分') : (mins + ' 分');
  }
  function renderNav(n) {
    const tag = $('navState');
    if (tag) {
      tag.textContent = n.valid ? (n.active ? '导航中' : '已停止') : '无数据';
      tag.className = 'tag ' + (n.valid && n.active ? 'ok' : '');
    }
    // icon 由协议给出时原样附带显示（我们不解释它）；转向文案只从 maneuver 取。
    const label = MANEUVERS[n.maneuver] || ('#' + n.maneuver);
    put('navManeuver', n.icon ? (label + ' · ' + n.icon) : label);
    put('navCode', n.maneuverCode ? String(n.maneuverCode) : '—');
    put('navRoad', n.roadName || '—');
    put('navNextRoad', n.nextRoadName || '—');
    put('navDistManeuver', meterText(n.distanceToManeuverM));
    put('navRemain', meterText(n.distanceRemainingM));
    put('navTime', durationText(n.timeRemainingS));
    put('navEta', n.etaEpochS ? new Date(n.etaEpochS * 1000).toLocaleTimeString() : '—');
    put('navDest', n.destination || '—');
  }

  const CALLS = ['空闲', '来电', '拨出', '通话中', '保持'];
  function renderTelephony(t) {
    const tag = $('telState');
    tag.textContent = CALLS[t.callState] || '—';
    tag.className = 'tag ' + (t.callState === 0 ? '' : (t.callState === 3 ? 'ok' : 'warn'));
    put('telCaller', t.caller || '—');
    put('telNumber', t.callerNumber || '—');
    put('telDuration', t.callDurationS ? fmtTime(t.callDurationS * 1000) : '—');
    put('telRssi', `${t.signalBars || 0}/5 · ${t.batteryPct || 0}%`);
    put('contactState', t.contactsReady ? `${t.contactCount} 条` : '未就绪');
    put('calllogState', t.callLogReady ? `${t.callLogCount} 条` : '未就绪');
  }

  function renderAudioInput(a, i) {
    const duck = $('audDuck');
    duck.textContent = a.navActive ? `压低中（${(a.duckRatioPpm / 10000).toFixed(1)}%，${a.duckTransitionMs}ms）` : '未压低';
    duck.className = a.navActive ? 'tag warn' : '';
    put('audVolume', `媒体 ${(a.mediaVolumePpm / 10000).toFixed(1)}% · 导航 ${(a.navVolumePpm / 10000).toFixed(1)}%`);
    put('audStream', `${a.channels || '—'} 声道 · ${a.sampleRate || '—'} Hz · codec ${a.codec} · 活跃流 ${a.simultaneousStreams}`);
    put('intCaps', `触点 ${i.multiTouchPoints || '—'}（最近 ${i.multiTouchUsed}）· 旋钮 ${i.knob ? '有' : '无'} · 触摸板 ${i.touchpad ? '有' : '无'} · 接近 ${i.proximity ? '近' : '远'} · HID ${i.hidMode} · VoiceOver ${i.voiceover ? '开' : '关'} · AssistiveTouch ${i.assistiveTouch ? '开' : '关'}`);
  }

  const ACTIVATION = ['未知', '未激活', '激活中', '已激活'];
  const OTA = ['无', '下载中', '校验中', '可安装', '失败'];
  function renderLink(l) {
    put('lnkActivation', `${ACTIVATION[l.activationState] || '—'} · 内容加密 ${['关', '要求', '已启用'][l.contentEncryption] || '—'}`);
    put('lnkFile', l.fileTransferActive ? `${l.fileTransferBytes}/${l.fileTransferTotal} B` : '空闲');
    put('lnkOta', OTA[l.otaState] || '—');
    put('sessCount', String(l.sessionCount || 0));
    const box = $('sessList');
    const sessions = l.sessions || [];
    box.innerHTML = '';
    if (!sessions.length) { box.innerHTML = '<li class="muted">—</li>'; return; }
    for (const s of sessions) {
      const li = document.createElement('li');
      const name = document.createElement('span');
      name.textContent = s.id || '(空 id)';
      const right = document.createElement('span');
      right.className = 'num';
      right.textContent = s.active ? '活跃' : '';
      li.appendChild(name); li.appendChild(right);
      li.title = '点击切为活跃会话';
      li.style.cursor = 'pointer';
      li.onclick = () => switchSession(s.id);
      box.appendChild(li);
    }
  }

  // 通讯录/通话记录来自 Web 侧缓存（契约的 TelephonyState 只有计数、无条目数组）。
  async function refreshContacts() {
    try {
      const src = encodeURIComponent(activeSource || 'carlife-hu');
      const [c, l] = await Promise.all([
        fetch(`/media/${src}/contacts`, { headers: H() }),
        fetch(`/media/${src}/calllog`, { headers: H() }),
      ]);
      if (c.ok) {
        const j = await c.json();
        const box = $('contactList'); box.innerHTML = '';
        if (!j.contacts || !j.contacts.length) box.innerHTML = '<li class="muted">—</li>';
        else for (const e of j.contacts) {
          const li = document.createElement('li');
          const n = document.createElement('span'); n.textContent = e.name || '(无名)';
          const num = document.createElement('span'); num.className = 'num'; num.textContent = e.number || '';
          li.appendChild(n); li.appendChild(num); box.appendChild(li);
        }
      }
      if (l.ok) {
        const j = await l.json();
        const box = $('calllogList'); box.innerHTML = '';
        if (!j.entries || !j.entries.length) box.innerHTML = '<li class="muted">—</li>';
        else for (const e of j.entries) {
          const li = document.createElement('li');
          const n = document.createElement('span'); n.textContent = (e.name || e.number || '(未知)') + ' · ' + ['来电', '已接', '未接', '拒接'][e.type];
          const t = document.createElement('span'); t.className = 'num';
          t.textContent = e.when ? new Date(e.when * 1000).toLocaleTimeString() : '';
          li.appendChild(n); li.appendChild(t); box.appendChild(li);
        }
      }
    } catch (_) { /* 网络瞬断：下个周期再试 */ }
  }

  /* ── 交互注入 ────────────────────────────────────────────────────── */
  let mtDown = [];                       // 已按下的触点，供“全部抬起”用
  async function interact(params, statusId) {
    const r = await post('/api/interact', params);
    if (statusId) {
      const el = $(statusId);
      if (el) el.textContent = r.ok ? '已注入' : ('被拒 ' + r.status + (r.json && r.json.error ? ('：' + r.json.error) : ''));
    }
    return r;
  }

  function sendTouchGroup(points, statusId) {
    if (!points.length) { interact({ kind: 'multitouch', pts: '' }, statusId); return; }
    const pts = points.map((p, i) => `${p.x},${p.y},${i},${p.phase}`).join(';');
    interact({ kind: 'multitouch', pts }, statusId);
  }

  // 预设多点：在主屏里均匀铺开按下；点“全部抬起”时对同一组发 phase=0。
  function presetMultiTouch(n, statusId) {
    if (n === 0) {
      if (!mtDown.length) { const el = $(statusId); if (el) el.textContent = '当前没有按下的触点'; return; }
      sendTouchGroup(mtDown.map(p => ({ x: p.x, y: p.y, phase: 0 })), statusId);
      mtDown = [];
      return;
    }
    const cv = roles.main.canvas;
    const w = cv.width, h = cv.height;
    mtDown = [];
    for (let i = 0; i < n; i++) mtDown.push({ x: Math.round(w * (i + 1) / (n + 1)), y: Math.round(h / 2) });
    sendTouchGroup(mtDown.map(p => ({ x: p.x, y: p.y, phase: 1 })), statusId);
  }

  function customMultiTouch(text, statusId) {
    const cv = roles.main.canvas;
    const pts = [];
    for (const chunk of text.split(';')) {
      const parts = chunk.trim().split(',').map(s => Number(s.trim()));
      if (parts.length !== 2 || !parts.every(Number.isFinite)) {
        const el = $(statusId); if (el) el.textContent = '格式应为 x,y;x,y'; return;
      }
      const [x, y] = parts;
      if (x < 0 || y < 0 || x > cv.width - 1 || y > cv.height - 1) {
        const el = $(statusId); if (el) el.textContent = `坐标越界（主屏 ${cv.width}x${cv.height}）`; return;
      }
      pts.push({ x: Math.round(x), y: Math.round(y), phase: 1 });
    }
    mtDown = pts.map(p => ({ x: p.x, y: p.y }));
    sendTouchGroup(pts, statusId);
  }

  /* ── 事件绑定 ────────────────────────────────────────────────────── */
  for (const b of document.querySelectorAll('[data-mt]')) b.onclick = () => presetMultiTouch(Number(b.dataset.mt), 'mtStatus');
  $('mtSend').onclick = () => customMultiTouch($('mtCustom').value, 'mtStatus');
  for (const b of document.querySelectorAll('[data-knob]')) b.onclick = () => interact({ kind: 'knob', dir: b.dataset.knob, steps: $('knobSteps').value || '1' }, 'actStatus');
  for (const b of document.querySelectorAll('[data-gesture]')) b.onclick = () => interact({ kind: 'gesture', name: b.dataset.gesture }, 'actStatus');
  for (const b of document.querySelectorAll('[data-proximity]')) b.onclick = () => interact({ kind: 'proximity', near: b.dataset.proximity }, 'actStatus');
  $('btnVoice').onclick = () => interact({ kind: 'voice' }, 'actStatus');
  for (const b of document.querySelectorAll('[data-media]')) b.onclick = () => interact({ kind: 'mediakey', key: b.dataset.media }, 'actStatus');
  for (const b of document.querySelectorAll('[data-vehctrl]')) b.onclick = () => interact({ kind: 'vehicle', ctrl: b.dataset.vehctrl }, 'actStatus');
  for (const b of document.querySelectorAll('[data-tel]')) b.onclick = () => interact({ kind: 'telephony', action: b.dataset.tel }, 'actStatus');
  for (const b of document.querySelectorAll('[data-dtmf]')) b.onclick = () => interact({ kind: 'dtmf', dtmf: b.dataset.dtmf }, 'actStatus');

  const playback = async (action, params) => {
    const r = await post('/api/playback', Object.assign({ action }, params || {}));
    $('npSeekStatus').textContent = r.ok ? '已请求 ' + action : ('被拒 ' + r.status);
  };
  $('npPlay').onclick = () => playback((lastMedia && lastMedia.playing) ? 'pause' : 'play');
  $('npNext').onclick = () => playback('next');
  $('npPrev').onclick = () => playback('prev');
  $('npFf').onclick = () => playback('ff');
  $('npRew').onclick = () => playback('rew');
  $('npSeek').addEventListener('input', () => { seeking = true; });
  $('npSeek').addEventListener('change', async () => {
    const dur = lastMedia ? (lastMedia.durationMs | 0) : 0;
    if (dur <= 0) { seeking = false; $('npSeekStatus').textContent = '未知时长，无法定位'; return; }
    const ms = Math.round(dur * Number($('npSeek').value) / 1000);
    await playback('seek', { positionMs: ms });
    seeking = false;
  });

  $('cfgForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const body = {
      gamma: $('cfgGamma').value, contrast: $('cfgContrast').value, saturation: $('cfgSaturation').value,
      dayNight: $('cfgDayNight').checked ? 1 : 0,
      safeTop: $('cfgSafeTop').value, safeBottom: $('cfgSafeBottom').value,
      safeLeft: $('cfgSafeLeft').value, safeRight: $('cfgSafeRight').value,
      auxEnabled: $('cfgAuxEnabled').checked ? 1 : 0,
      auxX: $('cfgAuxX').value, auxY: $('cfgAuxY').value, auxW: $('cfgAuxW').value, auxH: $('cfgAuxH').value,
      targetFps: $('cfgTargetFps').value,
    };
    const r = await post('/api/settings/display', body);
    $('cfgStatus').textContent = r.ok ? '已应用' : ('被拒 ' + r.status + (r.json && r.json.error ? ('：' + r.json.error) : ''));
    if (r.ok && r.json && r.json.display) applyDisplay(r.json.display);
  });

  const switchSession = async (id) => {
    if (!id) return;
    const r = await post('/api/sessions', { action: 'switch', id });
    $('sessStatus').textContent = r.ok ? ('已切到 ' + id) : ('切换被拒 ' + r.status);
  };
  $('sessAdd').onclick = async () => {
    const id = $('sessId').value.trim();
    if (!id) { $('sessStatus').textContent = '请填会话 id'; return; }
    const r = await post('/api/sessions', { action: 'upsert', id, active: 0 });
    $('sessStatus').textContent = r.ok ? ('已登记 ' + id) : ('登记被拒 ' + r.status);
  };
  $('sessSwitch').onclick = () => switchSession($('sessId').value.trim());

  /* ── 轮询 ────────────────────────────────────────────────────────── */
  async function npRefresh() {
    try {
      const res = await fetch('/api/state', { headers: H() });
      if (!res.ok) return;
      const j = await res.json();
      const m = j.media || {}, d = j.display || {};
      lastMedia = m;
      renderMedia(m);
      await ensureArtwork(m);
      await ensureLyrics(m);
      renderProgress(m);
      applyDisplay(d);
      fillCfgForm(d);
      renderVehicle(j.vehicle || {});
      renderNav(j.nav || {});
      renderTelephony(j.telephony || {});
      renderAudioInput(j.audio || {}, j.input || {});
      renderLink(j.link || {});
    } catch (_) { /* 下个周期再试 */ }
  }
  setInterval(npRefresh, 1000);
  npRefresh();
  setInterval(refreshContacts, 5000);
  refreshContacts();
})();
