# Core/Web 管理后台设计

## 目标与边界

管理后台是 Core 的正式组件，不是旁挂页面。它只做三件事：**看状态**、**选输入**、**把控制送回去**。
它不持有协议实现，也不直接访问输入模块内部——一切经由 `SessionCore`（调度）与各输入自己的
有界媒体面（`RealMediaStore`，`InputAdapter` 写入）。

## 数据流

```
InputAdapter(catplay-real / carlife-hu / local-desktop / synthetic-wireless)
        │  submit_video/audio → 各自 RealMediaStore（有界）
        │  upsert_source / register_control_sink
        ▼
   SessionCore  ── 选择与活跃态 ──►  Web 路由
        ▲                              │
        └──── route_control ◄──────────┘   （浏览器触控/硬键）
```

## 接口契约（v2）

所有接口在 `--token` 非空时都需要 `X-Auth-Token`（`/` 与 `/health` 除外）。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 管理页面（单次响应，内联样式与脚本） |
| GET | `/health` | `{"ok":true,"backend":...}` |
| GET | `/api/state` | 汇总状态：`mode`、`active`、`sources[]`（输入清单）、`drops`、`controls[]`、逐输入状态（`realCarPlayMedia` / `realCarPlayStatus` / `carlife`） |
| GET | `/api/sources` | 仅输入清单，与 `state.sources` 同源，便于轻量轮询 |
| POST | `/api/select` | `mode=automatic` 或 `mode=manual&source=<id>` |
| POST | `/api/control` | `type=touch&phase=down|move|up&x=&y=` 或 `type=key&key=<name|code>` |
| POST | `/api/media/keyframe` | `stream_id=<id>`，仅作用于当前活跃输入 |
| GET | `/api/display-config` | 下一次 CarPlay 会话的显示报价 |
| POST | `/api/display-config` | 同上，写入配置文件 |
| GET | `/media/{source}/video/{main\|alt}/stream` | 该输入的 H.264 分片流（CPMF 记录） |
| GET | `/media/{source}/audio/stream` | 该输入的 PCM 分片流（CPMF 记录） |
| GET | `/media/{source}/video/{main\|alt}` | 轮询式取帧（`X-Media-After`） |
| GET | `/media/{source}/audio` | 轮询式取音频 |

**为什么按 `source` 分路径**：CarPlay 与 CarLife 各有独立的有界媒体面，页面对两者用同一套
WebCodecs 解码通路；按来源限定路径让页面只拉当前活跃输入的流，避免把非活跃输入的帧混进来。
来源 id 与 `/api/sources` 返回的 id 一致（`catplay-real`、`carlife-hu`、`local-desktop`）。

**媒体记录格式**：沿用既有 CPMF 记录（64 字节头 + 载荷），`type` 17=VideoConfig、
18=VideoFrame、32=AudioStart、33=AudioChunk、35=AudioGain。CarLife 侧的 H.264 是 Annex-B，
关键帧由 IDR(nal 5) 判定，参数集 SPS(7)/PPS(8) 作为 VideoConfig；因此浏览器侧不需要为 CarLife
准备第二套解码路径。

**音频语义映射**（CarLife 三通道 → CPMF audio_type）：Media→2 media，TTS→1 alert，VR→4 speech-recognition。

## 页面结构

```
┌ 顶栏：标题 · 活跃输入 · token · 自动/手动 ─────────────┐
├ 左栏：输入卡片列表（每个输入一张卡）                   │
│   · id / kind / connected / 状态名 / 详细             │
│   · 显示分辨率、视频帧数/关键帧/字节                  │
│   · 音频（media/TTS 字节、块数）、控制（sent/dropped）│
│   · 麦克风请求/帧数                                   │
│   · [设为活跃输入]                                    │
├ 主区：当前活跃输入的画面（main / alt 两张画布）        │
│   · 触控 overlay（down/move/up 三阶段）               │
│   · 音频开关（需用户手势）                            │
├ 右栏：硬键面板（方向/确认/返回/Home/音量）             │
└ 底部：原始状态 JSON（诊断）· 显示配置表单             │
```

页面只渲染**活跃输入**的媒体；非活跃输入的卡片只显示状态与计数，不拉流。

## 资源与部署

- 页面资源放在 `Core/Web/assets/`：`index.html`（含 `{{STYLE}}` / `{{SCRIPT}}` 占位符）、`app.css`、`app.js`。
- 服务端启动时读取并做占位符替换，缓存为单份响应体，因此 `/` 只需要一次请求、没有额外静态路由。
- 运行时用 `--assets <dir>` 或 `WEB_ASSETS` 指定资源目录，默认取可执行文件同级的 `assets/`。
  资源缺失时明确报错并在页面上说明，而不是静默返回空白页。
- 部署时把 `assets/` 与二进制一起放进同一个 staging 目录，便于整目录回滚。

## 与旧页面的差异

| 项 | 旧 | 新 |
|---|---|---|
| 位置 | 内联在 `main.cpp` 的 `kPage` | `Core/Web/assets/` 独立资源 |
| 输入 | 只认 `catplay-real` | 任意 `InputAdapter`，按 `/api/sources` 动态渲染 |
| 媒体路由 | 硬编码单一媒体面 | `/media/{source}/…` 按来源选择媒体面 |
| 触控 | down/up 点击 | down/move/up 三阶段 + 硬键面板 |
| 诊断 | 单一 JSON dump | 逐输入卡片 + 原始 JSON |

---

## 新契约状态与 UI（v3，L3）

契约冻在 `Core/MainMenu/include/core/session_core.hpp`。`/api/state` 在原有字段之外，
新增 **7 块**（全部为值语义，一次 `snapshot()` 拷出）：

| 块 | 覆盖能力（对应 AUDIT 的 ❌ 清单） |
|---|---|
| `media` | 元数据（标题/歌手/专辑/曲目/App）+ 封面 + 歌词 + 进度 + 播放态（2.6 / 6.3 / 6.5） |
| `display` | safe area、协商分辨率、昼夜、亮度/对比度/饱和度校准、副屏平面矩形、主屏平面、目标/实际帧率（8.1 / 8.2 / 8.3） |
| `input` | 多点上限与最近用量、触摸板、旋钮、接近、HID 模式、VoiceOver、AssistiveTouch、最近硬键（2.5 / 6.2） |
| `audio` | 活跃用途、导航压低（含比例与渐变时长）、媒体/导航音量、声道/采样率/编码、并发流数（2.4 / 6.1 / 8.5） |
| `vehicle` | 速度/挡位/转速/GPS/航向/里程/油量/续航/车外温度/灯光/手刹/VIN（2.7 / 7.1） |
| `telephony` | 通话状态/来电者/号码/时长/HFP 信号与电量/通讯录与通话记录计数/DTMF 支持（2.7 / 7.1） |
| `link` | 激活、内容加密、文件传输进度、OTA 状态、多设备会话表与活跃会话（3.x / 5.3） |

### 新增只读端点

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/media/{source}/artwork[/{rev}]` | 封面字节。响应带 `ETag: "art-{rev}"`；无封面时 `404` + JSON |
| GET | `/media/{source}/lyrics[/{rev}]` | 歌词 LRC 原文（`text/plain; charset=utf-8`），带 `ETag: "lrc-{rev}"` |
| GET | `/media/{source}/contacts` | 通讯录 JSON（`{count, contacts:[{name,number}]}`） |
| GET | `/media/{source}/calllog` | 通话记录 JSON（`{count, entries:[{name,number,type,when}]}`，`when` 为 Unix 秒） |

`artUrl` / `lyricsUrl` / `contactsUrl` / `callLogUrl` 已随 `/api/state` 一并下发，页面无需拼接路径。
**URL 里带 `revision`**：换曲换封面时 URL 自然变化，浏览器缓存自动失效，因此不需要写 `If-None-Match` 的 304 分支。

### 新增写端点

| 方法 | 路径 | 参数 |
|---|---|---|
| POST | `/api/interact` | `kind=` 之一：`multitouch`（`pts=x,y,id,phase;…`，最多 10 点）、`knob`（`dir=left\|right\|up\|down\|press` + 可选 `steps`）、`gesture`（`name`）、`proximity`（`near=0\|1`）、`voice`、`telephony`（`action=answer\|hangup\|dial`）、`dtmf`（`dtmf=0-9*#`，≤15 位）、`mediakey`（`key=play\|pause\|next\|prev\|ff\|rew\|seek`）、`vehicle`（`ctrl=`<车控名\>） |
| POST | `/api/playback` | `action=resume\|play\|pause\|next\|prev\|ff\|rew`；`seek` 需 `positionMs=` |
| POST | `/api/settings/display` | `gamma`/`contrast`/`saturation`（10..200）、`dayNight`（0/1）、`safeTop\|Bottom\|Left\|Right`（≤4096）、`auxEnabled`、`auxX\|Y\|W\|H`、`targetFps`（1..120）。**只覆盖请求里出现的项**（读-改-写） |
| POST | `/api/sessions` | `action=upsert&id=&active=` 或 `action=switch&id=` |
| POST | `/api/telephony/contacts` | `count=N`（≤8）+ `name{i}=` / `number{i}=`，推入即替换全部 |
| POST | `/api/telephony/calllog` | `count=N`（≤8）+ `name{i}=`/`number{i}=`/`type{i}=`/`when{i}=` |

**注入永远不会拆会话**：不带 `kind`、未知 `kind`、参数越界一律 `400` + `{"error": …}`，
不会返回 `500`、不会断开链路。已有 `/api/control`（单点触控 + 硬键）保持原样不变。

### 表单编码：所有写入端点必须能接住浏览器的 percent 转义

浏览器的 `new URLSearchParams(obj).toString()` 会把 `,` `;` `#` 和**中文**都转义成 `%XX`。
既有代码用 `field()` 取原值，只因为历史参数（`mode=manual`、`key=volume_up`）恰好无反斜杠才没暴露；
而 `pts=x,y;x,y`（多点触控）与中文姓名/号码**必定会被转义**，不解码就会把 `%2C` 当字面量解析 → 400。

本轮修法：新增 `url_decode()` / `field_dec()`，本段内**所有字符串型字段都用 `field_dec()`**：
`kind` `pts` `dir` `steps` `name` `action` `dtmf` `key` `ctrl` `name{i}` `number{i}` `id`。
数字型字段仍走 `decimal_field()`（数字不会被转义，无需变）。

验证方式：用 `urllib.parse.urlencode`（与 `URLSearchParams` 同款编码）发请求，
已覆盖 2/10/11 点多点触控、`swipe_left`、`ac_temp_up`、`123#`、`positionMs`、
中文姓名与中文会话 id —— 全部 `200` 且中文完整还原（`Temp/audit/l3-encode.sh`）。

### JSON 合法性的两个硬要求

1. **字符串按 UTF-8 边界截断**：`utf8_safe()` 逐字符校验，末尾不完整/非法的多字节序列丢弃。
   历史 bug 是截断劈开汉字，导致整份 `/api/state` 被 `JSON.parse` 拒绝——本轮已用真中文
   （`张三丰` 等）走完推入→回读全链路验证。
2. **浮点先判有限**：`json_num()` 对 `NaN`/`Inf` 输出 `null`（`latitude`/`longitude`/`headingDeg`）。

### 页面新增面板

| 面板 | 内容 |
|---|---|
| 正在播放 | 封面（按 revision 换 URL）、标题/歌手/专辑/App/曲目、**可拖动进度条**（拖动→`seek`）、播放控制、**LRC 歌词高亮滚动** |
| 交互注入 | 多点（2/3/4/5 点预设、自定义 `x,y;x,y`、全部抬起）、旋钮（五向+步长）、触摸板手势、接近、语音键、媒体键、车控 |
| 车况 / 电话 | 速度/挡位/转速/油量/续航/温度/GPS/航向/灯光/手刹/VIN；通话状态/来电者/号码/时长/信号电量；接听/挂断/重拨；**DTMF 键盘**；通讯录与通话记录列表 |
| 显示设置 | 校准（gamma/对比度/饱和度实时作用于 CSS `filter`）、昼夜主题、safe area、副屏平面矩形、帧率请求 |
| 音频 / 链路 | 导航压低状态与比例、音量、声道/采样率/编码、交互能力集合、激活与内容加密、文件传输进度、OTA 状态 |
| 多设备会话 | 会话表（点击切为活跃）、登记/切换输入框 |

**safe area 的换算**：契约里是**协商像素**，页面是 CSS 像素，所以按
`canvas.clientWidth / display.width` 的比例缩放后再内缩（`--safe-*` 变量）。

**校准的落地方式**：`gamma/contrast/saturation` → CSS `filter: brightness() contrast() saturate()`，
作用在显示层，**不碰解码后的像素**（符合「不自行解码/串改音频视频」的一贯约束）。

### 已知契约缺口（本轮以最小约定先跑通，建议下轮在契约层补）

1. **`ControlEvent` 没有 seek 位置字段**。约定：`key="seek"` 时用 `x` 承载目标毫秒数。
   Input 端（`carlife_input.cpp` / CPMF 客户端）若也要支持 seek，需按同一约定读取。
   更干净的修法是在契约里加 `uint32_t seek_ms`，届时两端同步改。
2. **通讯录/通话记录没有存储位置**。契约声明了 `kMaxContacts`，但 `TelephonyState` 只有计数、
   没有条目数组；本轮把缓存放在 Web 层（定长数组 + 互斥量），由 Input 端通过
   `/api/telephony/*` 推入。正确的位置应当是 `SessionCore`——因为它才是车机/手机交互状态的所有者。
3. **`DisplayConfig` 同时被当作「Input 上报的状态」和「用户在页面上的设置」**。
   两个写入方共用一块结构会互相覆盖；本轮用**读-改-写**（改前先 `snapshot().display`，
   只覆盖请求里出现的项）把上报值（分辨率/实际帧率/主平面）保住。
   根治办法是拆成 `DisplayReported`（Input 写）+ `DisplayDesired`（页面写）。
4. **封面/歌词读取的并发窗口**。`SessionCore::artwork/lyrics` 返回的是固定缓冲的裸指针，
   锁外读期间生产者可能整块覆盖。本轮用「读一遍 → 复核 revision 未变才采用，最多重试 3 次」
   兜住（缓冲长度固定，不会越界，最坏是这一帧图撕裂，下个轮询周期即自愈）。

### 验证方式（可复现）

脚本：`Temp/audit/l3-*.sh`（素材/构建/端点实测/前端静态检查）。
构建用**自己的目录** `Temp/build-web`（多车道并发时共享构建目录会产生**假编译错误**）。

```bash
cmake -S . -B Temp/build-web -DCMAKE_BUILD_TYPE=Release
cmake --build Temp/build-web --target mvp_server -j2
Temp/build-web/mvp_server --bind 127.0.0.1 --port 18099 --assets Core/Web/assets &
curl -s http://127.0.0.1:18099/api/state | python3 -m json.tool | head -5
```

### 未验证的边界（如实列出）

- `json_num()` 的 `NaN`/`Inf` 分支：当前没有生产者会写 `vehicle.latitude/longitude/headingDeg`，
  所以只做了代码审阅，**未被本轮测试执行到**。
- 前端 JS 已过 `node --check` 与 ID 完整性检查（96 个引用 id 全部存在），
  但**未在真实浏览器里执行过**（无头环境不可用）；交互控件的实际点按效果需人工或浏览器测试确认。
- 服务端侧已用与浏览器同款编码（`urllib.parse.urlencode`）跑通全链路（`l3-encode.sh`，22/22 PASS），
  所以“前端发的形式后端接得住”这一半是验证过的；未验证的是“前端确实发的是这个形式”（需真浏览器）。

## 安全姿态：令牌校验与显式逃生阀（WEB_OPEN_API）

**默认：传了 `--token` 就必须校验。** 只有显式设置 `WEB_OPEN_API=1` 才关闭管理 API 的令牌校验。

| 启动方式 | 行为 | 启动日志 |
|---|---|---|
| `--token <t>`（不设 WEB_OPEN_API） | 必须带令牌；`/` 除外；**`/health` 也要令牌** | `[security] token 校验已启用（--token 已设置）` |
| `--token <t>` + `WEB_OPEN_API=1` | 管理 API 免令牌（**仅测试部署**） | `[security] 警告：WEB_OPEN_API=1 → 管理 API 无令牌校验（仅测试部署；生产不得设置）` |
| 不给 `--token` | 免令牌（历史行为，测试流程依赖） | `[security] 警告：未设置 --token → 管理 API 无令牌校验` |

**生产不得设置 `WEB_OPEN_API`。**

公开面**只有** `/`（页面外壳）；`/health` 在启用令牌时必须带令牌 —— `http_tests` 明确要求
`/health` 无令牌返回 401。理由是：把 `/health` 当公开探针，等于未授权也能探测"服务在跑"。

### 为什么要显式开关，而不是"传了就忽略"

早先实现是：只要传了 `--token` 就 `token.clear()` 并打印 `[test build] --token ignored`。
这使安全姿态变成**隐式的** —— 它只存在于一行没有任何强制力的日志里：

- 忽略行为万一被带到生产，**测试也不会拦**（`http_tests` 的 4 条 401 断言反而会永久为红，
  于是"红"被当成常态，真正的回归就淹没了）；
- 也无法审计：看日志才知道当前姿态，而不是看配置。

现在改成显式开关，并**两条路径都有测试覆盖**：不设 `WEB_OPEN_API` 时断言 401，
设为 `1` 时断言免令牌可访问。这样"关闭校验"是一个被测试钉住的显式行为，
而不是一处不可见的环境差异。

