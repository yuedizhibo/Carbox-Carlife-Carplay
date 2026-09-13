#include "core/desktop.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace mvp {
namespace {

constexpr std::size_t kRefreshIntervalMs = 1000;
constexpr int kWidth = 1280;
constexpr int kHeight = 720;
// 叠加信息里的长文本（标题/歌手/专辑/来电者）**先按显示宽度截断再转义**：
// 192 字节的标题若全被转义成 &amp; 会膨胀 5 倍，足够撑爆 snprintf 缓冲
// （历史上 256 字节缓冲被从中间截断 → SVG 不是合法 XML → 浏览器拒绝渲染）。
constexpr std::size_t kDisplayTextLimit = 72;

// 昼夜配色。日间沿用既有桌面的深色，这样 day_night 为默认 0 时**外观不变**；
// 夜间整体压暗并降低强调色亮度，底色与文字都不是纯黑/纯白。
struct Palette {
  const char* bg;
  const char* panel;
  const char* panel_active;
  const char* text;
  const char* muted;
  const char* accent;
  const char* ok;
  const char* bad;
  const char* warn;
};
constexpr Palette kDayPalette{"#0f151b", "#182530", "#1f3b4d", "#e6eef5", "#93a7b6", "#4cc2ff", "#3ddc84", "#f87171", "#fbbf24"};
constexpr Palette kNightPalette{"#0b0f14", "#12181f", "#18293a", "#c9d4de", "#7d8b99", "#3aa6de", "#35b873", "#d9695f", "#c9a227"};

// 布局换算：safe area 内缩后仍按 1280x720 的比例摆放。
// 用比例而不是绝对坐标，是为了 insets 全 0 时**逐像素复现原布局**（hs=vs=1），
// 而车机给了 safe area 时内容整体收缩，而不是溢出到画外。
struct Layout {
  int x0{}, y0{}, w{kWidth}, h{kHeight};
  double hs{1.0}, vs{1.0};
  int X(int px) const { return x0 + int(px * hs); }
  int Y(int px) const { return y0 + int(px * vs); }
  int W(int px) const { return int(px * hs); }
  int H(int px) const { return int(px * vs); }
  int F(int px) const { return std::max(10, int(px * (hs < vs ? hs : vs))); }
};

// safe area（AUDIT 8.2）：语义照抄 LIVI 的
// projectionSafeAreaTop/Bottom/Left/Right（LIVI/.../driver/aa/AaSession.test.ts:114-117）——
// 都是**像素内缩**，用来把内容避开车机圆角/实体按键/边框遮挡区。
Layout layout_of(const DisplayConfig& cfg) {
  Layout out;
  int left = cfg.safe_left, right = cfg.safe_right;
  int top = cfg.safe_top, bottom = cfg.safe_bottom;
  // 钳制：内缩之和不得吃掉整个画面，否则内容区尺寸为负、坐标会跑到画外。
  if (left + right > kWidth - 320) {
    const double k = double(kWidth - 320) / double(left + right);
    left = int(left * k);
    right = int(right * k);
  }
  if (top + bottom > kHeight - 240) {
    const double k = double(kHeight - 240) / double(top + bottom);
    top = int(top * k);
    bottom = int(bottom * k);
  }
  out.x0 = left;
  out.y0 = top;
  out.w = kWidth - left - right;
  out.h = kHeight - top - bottom;
  out.hs = double(out.w) / double(kWidth);
  out.vs = double(out.h) / double(kHeight);
  return out;
}

// 显示平面（AUDIT 8.4）：参考 LIVI 的 cluster {main, dash, aux} 三平面。
const char* plane_name(uint8_t plane) {
  switch (plane) {
    case 1: return "仪表盘 dash";
    case 2: return "辅助信息 aux";
    default: return "主屏镜像 main";
  }
}
const char* call_state_name(uint8_t state) {
  switch (state) {
    case 1: return "来电";
    case 2: return "拨出";
    case 3: return "通话中";
    case 4: return "保持";
    default: return "空闲";
  }
}
const char* gear_name(uint8_t gear) {
  switch (gear) {
    case 1: return "R";
    case 2: return "N";
    case 3: return "D";
    default: return "P";
  }
}
const char* activation_name(uint8_t state) {
  switch (state) {
    case 1: return "未激活";
    case 2: return "激活中";
    case 3: return "已激活";
    default: return "未知";
  }
}
const char* encryption_name(uint8_t state) {
  switch (state) {
    case 1: return "要求";
    case 2: return "已启用";
    default: return "关闭";
  }
}

// 显示校准（AUDIT 8.3）：LIVI 的合成器把它作用在**合成后的整帧**上
// （livi-compositor/rust/src/ctrl.rs:4,273-281：gamma / contrast / gain，
// `cal.active = nums.iter().any(|&v| v != 1.0)` —— 1.0 表示不改）。
// 这里用 SVG filter 表达同一件事，并保持"1.0 即恒等"的语义：
//   gamma      → feComponentTransfer/type=gamma（amplitude*in^exponent+offset，exponent=1 恒等）
//   contrast   → feComponentTransfer/type=linear（绕 0.5 旋转，slope=1 恒等）
//   saturation → feColorMatrix/type=saturate（1 恒等）
struct Calibration {
  double gamma{1.0}, contrast{1.0}, saturation{1.0};
  bool active{};
};

Calibration calibration_of(const DisplayConfig& cfg) {
  Calibration cal;
  cal.gamma = double(cfg.gamma_pct) / 100.0;
  cal.contrast = double(cfg.contrast_pct) / 100.0;
  cal.saturation = double(cfg.saturation_pct) / 100.0;
  cal.active = cfg.gamma_pct != 100 || cfg.contrast_pct != 100 || cfg.saturation_pct != 100;
  return cal;
}

// 未启用校准时返回空串 —— 默认输出因此与既有桌面逐字节一致（也保住 FNV 指纹基线）。
std::string calibration_filter(const Calibration& cal) {
  std::string body;
  char buf[320];
  if (cal.gamma != 1.0) {
    std::snprintf(buf, sizeof buf,
                  "<feComponentTransfer>"
                  "<feFuncR type='gamma' amplitude='1' exponent='%.3f'/>"
                  "<feFuncG type='gamma' amplitude='1' exponent='%.3f'/>"
                  "<feFuncB type='gamma' amplitude='1' exponent='%.3f'/>"
                  "</feComponentTransfer>",
                  cal.gamma, cal.gamma, cal.gamma);
    body += buf;
  }
  if (cal.contrast != 1.0) {
    const double intercept = 0.5 * (1.0 - cal.contrast);
    std::snprintf(buf, sizeof buf,
                  "<feComponentTransfer>"
                  "<feFuncR type='linear' slope='%.3f' intercept='%.3f'/>"
                  "<feFuncG type='linear' slope='%.3f' intercept='%.3f'/>"
                  "<feFuncB type='linear' slope='%.3f' intercept='%.3f'/>"
                  "</feComponentTransfer>",
                  cal.contrast, intercept, cal.contrast, intercept, cal.contrast, intercept);
    body += buf;
  }
  if (cal.saturation != 1.0) {
    std::snprintf(buf, sizeof buf, "<feColorMatrix type='saturate' values='%.3f'/>", cal.saturation);
    body += buf;
  }
  if (body.empty()) return {};
  return "<defs><filter id='dispcal' x='0' y='0' width='100%' height='100%' "
         "color-interpolation-filters='sRGB'>" + body + "</filter></defs>";
}

std::string escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
        break;
    }
  }
  return out;
}

std::string text32(const std::array<char, 32>& a) {
  std::size_t n = 0;
  while (n < a.size() && a[n]) ++n;
  return std::string(a.data(), n);
}

// 任意长度定长文本（标题/歌手/专辑/来电者等）取到 '\0' 为止。
template <std::size_t N>
std::string text_of(const std::array<char, N>& a) {
  std::size_t n = 0;
  while (n < a.size() && a[n]) ++n;
  return std::string(a.data(), n);
}

// 按 UTF-8 边界截断：**绝不劈开多字节字符**。
// 劈开会同时弄坏 /api/state 的 JSON 与这里的 SVG（两端都会整份被判为非法）。
std::string clamp_text(std::string_view text, std::size_t limit) {
  if (text.size() <= limit) return std::string(text);
  std::size_t n = limit;
  while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
  return std::string(text.substr(0, n));
}

// 叠加信息里的文本：先按显示宽度截断，再转义（顺序不能反，见 kDisplayTextLimit 注释）。
std::string overlay_text(std::string_view raw) { return escape(clamp_text(raw, kDisplayTextLimit)); }

// ── 逐向导航条（NavigationState）────────────────────────────────────────
// 渲染 CarPlay RouteGuidance / CarLife NAV_NEXT_TURN_INFO 解出的导航状态。
// 两条硬约束：
//   1) 【不给 maneuver_code 编代码→图标表】CarLife 的 action 码表由百度导航 App
//      产生，参考树里根本没有，编出来就是编造。所以只用**我们自己的** Maneuver
//      枚举挑图形；maneuver == None 时只显示原值文本，不猜图标。
//   2) 文本仍按 UTF-8 边界截断**再**转义，缓冲要继续够大。
constexpr std::size_t kNavShortLimit = 14;  // 第一行内联槽位（约 14 个汉字，不与邻槽相撞）
constexpr std::size_t kNavWideLimit = 48;   // 第二行（要给右侧车道指示留位置）

std::string nav_text(std::string_view raw, std::size_t limit) { return escape(clamp_text(raw, limit)); }

const char* maneuver_name(Maneuver m) {
  switch (m) {
    case Maneuver::Straight: return "直行";
    case Maneuver::SlightLeft: return "稍向左";
    case Maneuver::Left: return "左转";
    case Maneuver::SharpLeft: return "急左转";
    case Maneuver::SlightRight: return "稍向右";
    case Maneuver::Right: return "右转";
    case Maneuver::SharpRight: return "急右转";
    case Maneuver::Uturn: return "掉头";
    case Maneuver::Merge: return "汇入";
    case Maneuver::ForkLeft: return "靠左";
    case Maneuver::ForkRight: return "靠右";
    case Maneuver::Roundabout: return "环岛";
    case Maneuver::Exit: return "出口";
    case Maneuver::Arrive: return "到达";
    case Maneuver::Destination: return "到达终点";
    default: return "";
  }
}
// 图形与旋转角。旋转角是**视觉编码**（越急的弯转得越多），不是协议码映射。
struct Glyph { const char* shape; double rotate_deg; };
Glyph glyph_of(Maneuver m) {
  switch (m) {
    case Maneuver::Straight: return {"arrow", 0.0};
    case Maneuver::SlightLeft: return {"arrow", -30.0};
    case Maneuver::Left: return {"arrow", -60.0};
    case Maneuver::SharpLeft: return {"arrow", -105.0};
    case Maneuver::SlightRight: return {"arrow", 30.0};
    case Maneuver::Right: return {"arrow", 60.0};
    case Maneuver::SharpRight: return {"arrow", 105.0};
    case Maneuver::Uturn: return {"uturn", 0.0};
    case Maneuver::Merge: return {"merge", 0.0};
    case Maneuver::ForkLeft: return {"fork", -30.0};
    case Maneuver::ForkRight: return {"fork", 30.0};
    case Maneuver::Roundabout: return {"roundabout", 0.0};
    case Maneuver::Exit: return {"exit", 0.0};
    case Maneuver::Arrive: return {"arrive", 0.0};
    case Maneuver::Destination: return {"arrive", 0.0};
    default: return {nullptr, 0.0};   // None / 未知：不画图形，只显示原值
  }
}
// 在 44x44 局部坐标里定义图形，再缩放到给定方框。'C' 是颜色占位符
// （所有属性名都是小写，大写 C 只可能是占位符）。
std::string glyph_svg(const Glyph& g, int x, int y, int size, const char* color) {
  if (!g.shape) return {};
  std::string path;
  if (std::strcmp(g.shape, "roundabout") == 0) {
    path =
        "<circle cx='22' cy='22' r='11' fill='none' stroke='C' stroke-width='3'/>"
        "<path d='M22 3 L22 11' stroke='C' stroke-width='3'/>"
        "<path d='M17 6 L22 11 L27 6' fill='none' stroke='C' stroke-width='3'/>";
  } else if (std::strcmp(g.shape, "arrive") == 0) {
    path =
        "<path d='M10 23 L18 31 L34 11' fill='none' stroke='C' stroke-width='5' "
        "stroke-linecap='round' stroke-linejoin='round'/>";
  } else if (std::strcmp(g.shape, "uturn") == 0) {
    path =
        "<path d='M14 41 L14 20 A8 8 0 0 1 30 20 L30 41' fill='none' stroke='C' "
        "stroke-width='4' stroke-linecap='round'/>"
        "<path d='M9 31 L14 38 L19 31' fill='none' stroke='C' stroke-width='4'/>";
  } else if (std::strcmp(g.shape, "merge") == 0) {
    path =
        "<path d='M14 41 L14 26 L22 14 M30 41 L30 26 L22 14' fill='none' stroke='C' "
        "stroke-width='4' stroke-linecap='round'/>";
  } else if (std::strcmp(g.shape, "fork") == 0) {
    path =
        "<path d='M22 41 L22 26 M22 26 L12 14 M22 26 L32 14' fill='none' stroke='C' "
        "stroke-width='4' stroke-linecap='round'/>";
  } else if (std::strcmp(g.shape, "exit") == 0) {
    path =
        "<path d='M22 41 L22 16 M22 16 L12 26 M22 16 L32 26' fill='none' stroke='C' "
        "stroke-width='4' stroke-linecap='round'/>"
        "<rect x='33' y='7' width='9' height='9' fill='C'/>";
  } else {  // arrow
    path =
        "<path d='M22 41 L22 13' stroke='C' stroke-width='4' stroke-linecap='round'/>"
        "<path d='M12 23 L22 13 L32 23' fill='none' stroke='C' stroke-width='4' "
        "stroke-linecap='round' stroke-linejoin='round'/>";
  }
  std::string colored;
  colored.reserve(path.size() + 32);
  for (char c : path) {
    if (c == 'C') colored += color;
    else colored += c;
  }
  char head[128];
  std::snprintf(head, sizeof head, "<g transform='translate(%d,%d) scale(%.4f) rotate(%.1f 22 22)'>",
                x, y, double(size) / 44.0, g.rotate_deg);
  return std::string(head) + colored + "</g>";
}
// 显示用距离换算（不是协议字段改动）：>=1km 用一位小数。
std::string distance_text(uint32_t meters) {
  char buf[32];
  if (meters >= 1000u) std::snprintf(buf, sizeof buf, "%.1f km", double(meters) / 1000.0);
  else std::snprintf(buf, sizeof buf, "%u m", unsigned(meters));
  return buf;
}
std::string duration_text(uint32_t seconds) {
  char buf[48];
  const uint32_t minutes = seconds / 60u, hours = minutes / 60u;
  if (hours) std::snprintf(buf, sizeof buf, "%u 小时 %u 分", unsigned(hours), unsigned(minutes % 60u));
  else if (minutes) std::snprintf(buf, sizeof buf, "%u 分钟", unsigned(minutes));
  else std::snprintf(buf, sizeof buf, "%u 秒", unsigned(seconds));
  return buf;
}
// ETA 用本地时间显示。用 localtime_r 而非 localtime：渲染可能被多个线程调用。
std::string clock_text(uint64_t epoch_s) {
  if (!epoch_s) return {};
  const std::time_t t = std::time_t(epoch_s);
  std::tm tm{};
  if (!localtime_r(&t, &tm)) return {};
  char buf[16];
  if (std::strftime(buf, sizeof buf, "%H:%M", &tm) == 0) return {};
  return buf;
}

const char* kind_name(InputSourceKind kind) {
  switch (kind) {
    case InputSourceKind::External: return "外部输入";
    case InputSourceKind::Synthetic: return "合成测试";
    default: return "本地桌面";
  }
}

// 桌面要回答的问题：现在有几个输入、谁在活跃、用户该做什么。
struct DesktopView {
  std::size_t connected{};
  std::size_t connected_external{};
  std::string active;
  bool manual{};
  uint64_t video_dropped{}, audio_dropped{}, control_dropped{};
};

DesktopView analyse(const SessionSnapshot& s) {
  DesktopView view;
  view.active = text32(s.active);
  view.manual = s.mode == SelectionMode::Manual;
  view.video_dropped = s.video_dropped;
  view.audio_dropped = s.audio_dropped;
  view.control_dropped = s.control_dropped;
  for (std::size_t i = 0; i < s.source_count; ++i) {
    const auto& source = s.sources[i];
    if (!source.connected) continue;
    ++view.connected;
    // 桌面自身不计入「可选输入」，否则会出现「选择桌面」这种无意义选项。
    if (source.id[0] == 'c' && text32(source.id) == "core-desktop") continue;
    if (source.kind == InputSourceKind::External) ++view.connected_external;
  }
  return view;
}

}  // namespace

DesktopRenderer::DesktopRenderer(SessionCore& core, std::string source_id)
    : core_(core), id_(std::move(source_id)) {}

DesktopRenderer::~DesktopRenderer() { stop(); }

void DesktopRenderer::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;
  core_.upsert_source(id_, InputSourceKind::LocalDesktop, true);
  core_.register_control_sink(id_, [this](const ControlEvent&) { ++controls_received_; });
  thread_ = std::thread([this] { tick(); });
  refresh();
}

void DesktopRenderer::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) return;
  if (thread_.joinable()) thread_.join();
  core_.unregister_control_sink(id_);
  core_.upsert_source(id_, InputSourceKind::LocalDesktop, false);
}

void DesktopRenderer::tick() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kRefreshIntervalMs));
    if (!running_.load()) break;
    refresh();
  }
}

bool DesktopRenderer::active() const { return text32(core_.snapshot().active) == id_; }

// publish_now() 是唯一实现；refresh() 是它的等价别名（供 1 Hz tick 与既有调用点使用）。
//
// 【为什么要加锁】publish_now() 允许从任意线程调用（切源之后由 HTTP worker 立刻叫它出一帧）。
// published_in_epoch_ / last_signature_ 与 1 Hz tick 线程共享，原本是非原子的（数据竞争 = UB）。
// 用 state_mutex_ 把"取快照 → 渲染 → 指纹去重 → 提交"整段串行化。
//
// 【锁序】DesktopRenderer::state_mutex_ → SessionCore 内部的 mutex_（snapshot()/submit_video()）。
// 反向获取不存在，所以不会死锁。持锁期间只做纯计算（render 是 μs~ms 级）+
// 一次 256 KiB 帧体拷贝，不 sleep、不 join、不做 I/O。
bool DesktopRenderer::publish_now() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto snapshot = core_.snapshot();
  if (text32(snapshot.active) != id_) {
    // 不再是活跃面：清除本轮标记，保证下次成为活跃面时必定重发一帧（否则切换后是空屏）。
    published_in_epoch_ = false;
    return false;
  }
  const auto svg = render(snapshot);
  // 内容指纹（FNV-1a）：静态桌面不需要每秒重发 256 KiB 的帧体。
  uint64_t signature = 1469598103934665603ULL;
  for (unsigned char c : svg) {
    signature ^= c;
    signature *= 1099511628211ULL;
  }
  // 【去重必须同时看"帧还在不在"】只看内容是不够的：
  // SessionCore 在活跃源**发生变化**时会清掉 has_video_/latest_video_（帧真的没了），
  // 而内容可能一个字没变 —— 此时若只比指纹就会误判"已发布过"而拒绝重发，
  // 画面会一直空着（切源后的空白就是这么来的）。所以还要确认 SessionCore 里那一帧还在。
  if (published_in_epoch_ && core_.has_video() && signature == last_signature_) return false;

  // 帧体只申请一次并复用：
  //   ① 不让 262184 字节的 VideoFrame 落在这个栈帧上（publish_now() 会被 HTTP worker 调用）；
  //   ② 免掉每次发布的 256 KiB 零初始化（复用后只覆盖前 n 个字节，size 决定有效长度）。
  // 全程在 state_mutex_ 下，不会被 tick 线程与外部线程同时碰。
  if (!frame_) frame_ = std::make_unique<VideoFrame>();
  VideoFrame& frame = *frame_;
  frame.encoding = VideoEncoding::Svg;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.sequence = frames_published_.load() + 1;
  frame.pts = frame.sequence * kRefreshIntervalMs;
  const std::size_t n = std::min(frame.payload.size() - 1, svg.size());
  std::memcpy(frame.payload.data(), svg.data(), n);
  frame.size = n;
  if (!core_.submit_video(id_, frame)) return false;
  ++frames_published_;
  published_in_epoch_ = true;
  last_signature_ = signature;
  return true;
}

void DesktopRenderer::refresh() { (void)publish_now(); }

std::string DesktopRenderer::render(const SessionSnapshot& s) const {
  const auto view = analyse(s);
  const Layout lay = layout_of(s.display);
  const Calibration cal = calibration_of(s.display);
  const Palette& pal = s.display.day_night ? kNightPalette : kDayPalette;

  // 无输入 / 多输入未选择时的引导文案（行为与改前一致）。
  std::string headline;
  std::string guidance;
  if (view.connected <= 1) {
    headline = "等待手机接入";
    guidance = "连接无线 CarPlay 或无线 CarLife+，画面会自动切过去";
  } else if (view.connected_external >= 2) {
    headline = std::to_string(view.connected_external) + " 个手机输入在线";
    guidance = "请在管理页面选择要使用的输入";
  } else {
    headline = "多输入在线";
    guidance = "请在管理页面选择要使用的输入";
  }

  char header[512];
  std::snprintf(header, sizeof header, "%s", headline.c_str());

  // ── 左栏：输入源列表 ──
  std::string rows;
  int y = lay.Y(250);
  for (std::size_t i = 0; i < s.source_count; ++i) {
    const auto& source = s.sources[i];
    const auto name = text32(source.id);
    if (name == id_) continue;   // 桌面本身不列在可选输入里
    const bool is_active = name == view.active;
    // 【缓冲区必须够大】每行含 3 段 <text>，且中文字符是多字节（UTF-8）。
    // 原来写 256：格式化后超过 255 字节，snprintf 会从中间截断，
    // 把 </text> 和后面整段切掉 —— 产出的 SVG 就不再是合法 XML，
    // 浏览器直接拒绝渲染，页面上就是“SVG 帧渲染失败”。
    // 实测截断点正好在中文名字之后，与这个原因完全吻合。
    char line[768];
    std::snprintf(line, sizeof line,
                  "<rect x='%d' y='%d' width='%d' height='%d' rx='10' fill='%s'/>"
                  "<circle cx='%d' cy='%d' r='10' fill='%s'/>"
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>"
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>"
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>",
                  lay.X(80), y, lay.W(660), lay.H(64), is_active ? pal.panel_active : pal.panel,
                  lay.X(116), y + lay.H(32), source.connected ? pal.ok : pal.bad,
                  lay.X(146), y + lay.H(40), pal.text, lay.F(24), escape(name).c_str(),
                  lay.X(340), y + lay.H(40), pal.muted, lay.F(20), kind_name(source.kind),
                  lay.X(600), y + lay.H(40), source.connected ? pal.ok : pal.bad, lay.F(20),
                  source.connected ? (is_active ? "活跃" : "在线") : "离线");
    rows += line;
    y += lay.H(76);
    if (y > lay.Y(560)) break;
  }

  // ── 右栏：元数据（Now Playing / 封面 / 进度）+ 运行时叠加（AUDIT 2.6 / 6.5 / 2.7）──
  std::string overlay;
  char buf[512];
  const int ox = lay.X(800);
  const int ow = lay.W(1280 - 800 - 80);

  std::snprintf(buf, sizeof buf, "<rect x='%d' y='%d' width='%d' height='%d' rx='12' fill='%s'/>",
                ox, lay.Y(250), ow, lay.H(238), pal.panel);
  overlay += buf;
  std::snprintf(buf, sizeof buf, "<text x='%d' y='%d' fill='%s' font-size='%d'>正在播放</text>",
                ox + lay.W(16), lay.Y(250) + lay.H(30), pal.accent, lay.F(20));
  overlay += buf;

  // 封面位：SVG 只留位置与 revision 标记 —— 真实封面由 Web 层按 revision 贴图，
  // 桌面这一层不搬 128 KiB 的图（AUDIT 6.5）。
  const int art = std::min(lay.W(88), lay.H(88));
  const int art_x = ox + lay.W(16);
  const int art_y = lay.Y(250) + lay.H(46);
  if (s.media.has_artwork) {
    std::snprintf(buf, sizeof buf,
                  "<rect x='%d' y='%d' width='%d' height='%d' rx='8' fill='none' stroke='%s' stroke-width='2'/>"
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>封面</text>"
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>rev %u · %u B</text>",
                  art_x, art_y, art, art, pal.accent,
                  art_x + art / 6, art_y + art / 2, pal.accent, lay.F(18),
                  art_x + art / 8, art_y + art - art / 8, pal.muted, lay.F(13),
                  unsigned(s.media.artwork_revision), unsigned(s.media.artwork_bytes));
  } else {
    std::snprintf(buf, sizeof buf,
                  "<rect x='%d' y='%d' width='%d' height='%d' rx='8' fill='none' stroke='%s' "
                  "stroke-width='2' stroke-dasharray='6 6'/>"
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>封面：无</text>",
                  art_x, art_y, art, art, pal.muted,
                  art_x + art / 8, art_y + art / 2, pal.muted, lay.F(16));
  }
  overlay += buf;

  const int text_x = art_x + art + lay.W(14);
  if (!s.media.valid) {
    std::snprintf(buf, sizeof buf, "<text x='%d' y='%d' fill='%s' font-size='%d'>暂无元数据</text>",
                  text_x, art_y + lay.H(40), pal.muted, lay.F(20));
    overlay += buf;
  } else {
    std::snprintf(buf, sizeof buf, "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>",
                  text_x, art_y + lay.H(30), pal.text, lay.F(22),
                  overlay_text(text_of(s.media.title)).c_str());
    overlay += buf;
    std::snprintf(buf, sizeof buf, "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>",
                  text_x, art_y + lay.H(58), pal.muted, lay.F(18),
                  overlay_text(text_of(s.media.artist)).c_str());
    overlay += buf;
    std::snprintf(buf, sizeof buf, "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>",
                  text_x, art_y + lay.H(84), pal.muted, lay.F(16),
                  overlay_text(text_of(s.media.album)).c_str());
    overlay += buf;
  }

  // 线性进度条：position_ms / duration_ms（AUDIT 2.6 播放进度）。
  const int bar_x = ox + lay.W(16);
  const int bar_y = lay.Y(250) + lay.H(164);
  const int bar_w = ow - lay.W(32);
  const int bar_h = std::max(4, lay.H(10));
  uint32_t pct = 0;
  if (s.media.duration_ms > 0) {
    const uint32_t pos = s.media.position_ms > s.media.duration_ms ? s.media.duration_ms : s.media.position_ms;
    pct = uint32_t(double(pos) * 100.0 / double(s.media.duration_ms));
  }
  std::snprintf(buf, sizeof buf, "<rect x='%d' y='%d' width='%d' height='%d' rx='5' fill='%s'/>",
                bar_x, bar_y, bar_w, bar_h, pal.panel_active);
  overlay += buf;
  if (pct) {
    std::snprintf(buf, sizeof buf, "<rect x='%d' y='%d' width='%d' height='%d' rx='5' fill='%s'/>",
                  bar_x, bar_y, std::max(bar_h, int(bar_w * int(pct) / 100)), bar_h, pal.accent);
    overlay += buf;
  }
  std::snprintf(buf, sizeof buf,
                "<text x='%d' y='%d' fill='%s' font-size='%d'>%u:%02u / %u:%02u ｜ %u%%</text>",
                bar_x, bar_y + lay.H(30), pal.muted, lay.F(16),
                unsigned(s.media.position_ms / 60000u), unsigned((s.media.position_ms / 1000u) % 60u),
                unsigned(s.media.duration_ms / 60000u), unsigned((s.media.duration_ms / 1000u) % 60u),
                unsigned(pct));
  overlay += buf;

  // ── 状态行：车况 / 电话 / 链路 / 音频（AUDIT 2.7、3.x、8.5）──
  const int step = lay.H(32);
  int sy = lay.Y(500);
  if (s.vehicle.valid) {
    std::snprintf(buf, sizeof buf,
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>车况 车速 %d km/h · 档位 %s · 航向 %d°</text>",
                  ox, sy, pal.text, lay.F(19), int(s.vehicle.speed_kph),
                  gear_name(s.vehicle.gear), int(s.vehicle.heading_deg));
  } else {
    std::snprintf(buf, sizeof buf, "<text x='%d' y='%d' fill='%s' font-size='%d'>车况 无数据</text>",
                  ox, sy, pal.muted, lay.F(19));
  }
  overlay += buf;
  sy += step;

  if (s.telephony.call_state) {
    std::snprintf(buf, sizeof buf,
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>电话 %s · %s · %us</text>",
                  ox, sy, pal.text, lay.F(19), call_state_name(s.telephony.call_state),
                  overlay_text(text_of(s.telephony.caller)).c_str(),
                  unsigned(s.telephony.call_duration_s));
  } else {
    std::snprintf(buf, sizeof buf,
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>电话 空闲 · 通讯录 %u 条</text>",
                  ox, sy, pal.muted, lay.F(19), unsigned(s.telephony.contact_count));
  }
  overlay += buf;
  sy += step;

  std::snprintf(buf, sizeof buf,
                "<text x='%d' y='%d' fill='%s' font-size='%d'>链路 %s · 内容加密 %s · 会话 %u</text>",
                ox, sy, pal.muted, lay.F(19), activation_name(s.link.activation_state),
                encryption_name(s.link.content_encryption), unsigned(s.link.session_count));
  overlay += buf;
  sy += step;

  if (s.audio.nav_active) {
    // 导航播报中媒体被按比例压低（AUDIT 8.5：maxVolume / VolumReduceRatio）。
    std::snprintf(buf, sizeof buf,
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>音频 导航播报中 · 媒体压低至 %d%% · %u 声道</text>",
                  ox, sy, pal.warn, lay.F(19), int(s.audio.duck_ratio_ppm / 10000),
                  unsigned(s.audio.channels));
  } else {
    std::snprintf(buf, sizeof buf,
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>音频 媒体 %d%% · %u 声道</text>",
                  ox, sy, pal.muted, lay.F(19), int(s.audio.media_volume_ppm / 10000),
                  unsigned(s.audio.channels));
  }
  overlay += buf;

  // ── 逐向导航条：NavigationState ──
  // active==0 整块不画（没在导航就不占位置）；destination_reached 画"已到达"。
  // 位置：内容区底部空带（footer 之下），全宽 —— 不挤动任何既有元素。
  std::string nav;
  if (s.nav.valid && s.nav.active) {
    const int nx = lay.X(80);
    const int nw = lay.W(1120);
    const int ny = lay.Y(658);
    const int nh = lay.H(56);
    char nbuf[1024];
    std::snprintf(nbuf, sizeof nbuf,
                  "<rect x='%d' y='%d' width='%d' height='%d' rx='12' fill='%s' stroke='%s' "
                  "stroke-width='2'/>",
                  nx, ny, nw, nh, pal.panel, s.nav.destination_reached ? pal.ok : pal.accent);
    nav += nbuf;

    // 图形：只由我们的 Maneuver 枚举决定；None 时留空（不猜）
    const int gx = nx + lay.W(14);
    const int gy = ny + lay.H(7);
    const int gs = std::max(24, lay.H(42));
    if (s.nav.destination_reached) {
      nav += glyph_svg(Glyph{"arrive", 0.0}, gx, gy, gs, pal.ok);
    } else {
      const Glyph g = glyph_of(s.nav.maneuver);
      if (g.shape) nav += glyph_svg(g, gx, gy, gs, pal.accent);
    }

    const int tx = nx + lay.W(72);
    const char* action = s.nav.destination_reached ? "已到达"
                         : s.nav.maneuver != Maneuver::None ? maneuver_name(s.nav.maneuver)
                                                            : "导航";
    const std::string act = nav_text(action, kNavShortLimit);
    const std::string road = nav_text(text_of(s.nav.road_name), kNavShortLimit);
    const std::string next = nav_text(text_of(s.nav.next_road_name), kNavShortLimit);
    const std::string nicon = nav_text(text_of(s.nav.icon), kNavShortLimit);

    // 第一行：动作 · 当前道路 · 下一段 · 到下一动作的距离
    std::snprintf(nbuf, sizeof nbuf, "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>",
                  tx, ny + lay.H(26), pal.text, lay.F(24), act.c_str());
    nav += nbuf;
    if (!road.empty()) {
      std::snprintf(nbuf, sizeof nbuf, "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>",
                    tx + lay.W(200), ny + lay.H(26), pal.accent, lay.F(24), road.c_str());
      nav += nbuf;
    }
    if (!next.empty()) {
      std::snprintf(nbuf, sizeof nbuf, "<text x='%d' y='%d' fill='%s' font-size='%d'>→ %s</text>",
                    tx + lay.W(410), ny + lay.H(25), pal.muted, lay.F(20), next.c_str());
      nav += nbuf;
    }
    if (s.nav.distance_to_maneuver_m) {
      std::snprintf(nbuf, sizeof nbuf, "<text x='%d' y='%d' fill='%s' font-size='%d'>前方 %s</text>",
                    tx + lay.W(620), ny + lay.H(25), pal.warn, lay.F(20),
                    distance_text(s.nav.distance_to_maneuver_m).c_str());
      nav += nbuf;
    }
    // 图标名放到第一行的独立槽位：留在第二行会被 48 字节上限从中间截断
    // （自测发现过 “turn-left” 被截成 “turn”），且第一行这里本来就有空位。
    if (!nicon.empty()) {
      std::snprintf(nbuf, sizeof nbuf, "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>",
                    tx + lay.W(830), ny + lay.H(25), pal.muted, lay.F(17), nicon.c_str());
      nav += nbuf;
    }

    // 第二行：全程剩余 · 预计耗时（末尾给 ETA 与车道指示留位置）
    std::string detail;
    if (s.nav.distance_remaining_m) detail += "全程 " + distance_text(s.nav.distance_remaining_m);
    if (s.nav.time_remaining_s) {
      if (!detail.empty()) detail += " ｜ ";
      detail += "约 " + duration_text(s.nav.time_remaining_s);
    }
    // 图标名已挪到第一行独立槽位，不再进第二行（否则会被 48 字节上限截断）
    // maneuver 无法判定时**如实显示原值**，不猜它的含义（此分支的 `{` 与下一行的
    // `if` 必须在同一行之前断开 —— 曾因编辑时漏了换行把两者粘成一行，
    // 导致开括号被注释掉、块提前闭合、后面整段跑出作用域而编不过）。
    if (!s.nav.destination_reached && s.nav.maneuver == Maneuver::None && s.nav.maneuver_code) {
      if (!detail.empty()) detail += " ｜ ";
      detail += "转向码 " + std::to_string(s.nav.maneuver_code) + "（未映射）";
    }
    if (!detail.empty()) {
      std::snprintf(nbuf, sizeof nbuf, "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>",
                    tx, ny + lay.H(47), pal.muted, lay.F(16),
                    nav_text(detail, kNavWideLimit).c_str());
      nav += nbuf;
    }
    // ETA **单独成一个 text**：它是最重要的信息，不能被第二行的 48 字节上限
    // 在 "… ｜ 17:05 到达" 处截掉（自测发现过：组合串 49 字节，"到达" 被吃掉）。
    if (s.nav.eta_epoch_s) {
      const std::string eta = clock_text(s.nav.eta_epoch_s);
      if (!eta.empty()) {
        std::snprintf(nbuf, sizeof nbuf,
                      "<text x='%d' y='%d' fill='%s' font-size='%d'>%s 到达</text>",
                      nx + nw - lay.W(400), ny + lay.H(47), pal.warn, lay.F(16),
                      nav_text(eta, kNavShortLimit).c_str());
        nav += nbuf;
      }
    }

    // 车道指示：lane_bitmap 非零才画。位图只表示"哪些车道可用"；协议里没有
    // "推荐第几条"的字段，所以**不臆造**推荐车道标记。
    if (s.nav.lane_bitmap) {
      const unsigned bits = unsigned(s.nav.lane_bitmap) & 0xFFu;
      int count = 0;
      for (unsigned b = 0; b < 8; ++b) if (bits & (1u << b)) count = int(b) + 1;
      const int cell = std::max(10, lay.W(22));
      const int gap = std::max(3, lay.W(6));
      const int lane_h = std::max(10, lay.H(22));
      const int lane_y = ny + lay.H(20);
      const int lane_x = nx + nw - lay.W(24) - count * cell - (count - 1) * gap;
      for (int i = 0; i < count; ++i) {
        const bool on = (bits & (1u << unsigned(i))) != 0;
        std::snprintf(nbuf, sizeof nbuf,
                      "<rect x='%d' y='%d' width='%d' height='%d' rx='4' fill='%s' "
                      "fill-opacity='%s' stroke='%s' stroke-width='1'/>",
                      lane_x + i * (cell + gap), lane_y, cell, lane_h,
                      on ? pal.accent : pal.panel_active, on ? "0.45" : "0.15", pal.muted);
        nav += nbuf;
      }
      std::snprintf(nbuf, sizeof nbuf, "<text x='%d' y='%d' fill='%s' font-size='%d'>车道</text>",
                    lane_x - lay.W(56), lane_y + lane_h - lay.H(4), pal.muted, lay.F(14));
      nav += nbuf;
    }
  }

  // ── 副屏平面：aux_enabled 时在指定矩形里画独立平面（仪表/第二路内容）──
  // 平面语义照抄 LIVI 的 cluster {main, dash, aux}（AUDIT 8.4）。
  std::string aux;
  if (s.display.aux_enabled && s.display.aux_w && s.display.aux_h) {
    int awx = lay.X(s.display.aux_x);
    int awy = lay.Y(s.display.aux_y);
    int aww = std::max(60, lay.W(s.display.aux_w));
    int awh = std::max(40, lay.H(s.display.aux_h));
    // 钳制进内容区：越界部分浏览器不会渲染，看起来就像“副屏没画出来”。
    if (awx + aww > lay.x0 + lay.w) awx = std::max(lay.x0, lay.x0 + lay.w - aww);
    if (awy + awh > lay.y0 + lay.h) awy = std::max(lay.y0, lay.y0 + lay.h - awh);
    std::snprintf(buf, sizeof buf,
                  "<rect x='%d' y='%d' width='%d' height='%d' rx='10' fill='%s' fill-opacity='0.35' "
                  "stroke='%s' stroke-width='2'/>"
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>副屏平面</text>"
                  "<text x='%d' y='%d' fill='%s' font-size='%d'>%ux%u ｜ 主屏平面 %s</text>",
                  awx, awy, aww, awh, pal.panel, pal.accent,
                  awx + lay.W(14), awy + lay.H(28), pal.accent, lay.F(20),
                  awx + lay.W(14), awy + lay.H(56), pal.muted, lay.F(16),
                  unsigned(s.display.aux_w), unsigned(s.display.aux_h), plane_name(s.display.primary_plane));
    aux += buf;
  }

  // 底部：模式 + 丢帧（原样保留）+ 显示协商与 safe area（有值才追加，默认输出更干净）
  std::string extra;
  {
    char part[192];
    if (s.display.width && s.display.height) {
      std::snprintf(part, sizeof part, " ｜ 显示 %ux%u · %u→%u fps", unsigned(s.display.width),
                    unsigned(s.display.height), unsigned(s.display.target_fps), unsigned(s.display.actual_fps));
      extra += part;
    }
    if (s.display.safe_top || s.display.safe_bottom || s.display.safe_left || s.display.safe_right) {
      std::snprintf(part, sizeof part, " ｜ safe area 上%u 下%u 左%u 右%u", unsigned(s.display.safe_top),
                    unsigned(s.display.safe_bottom), unsigned(s.display.safe_left), unsigned(s.display.safe_right));
      extra += part;
    }
  }
  char footer[1024];
  std::snprintf(footer, sizeof footer,
                "<text x='%d' y='%d' fill='%s' font-size='%d'>模式：%s%s ｜ "
                "丢帧 视频 %llu · 音频 %llu · 控制 %llu</text>",
                lay.X(80), lay.Y(648), pal.muted, lay.F(18), view.manual ? "手动" : "自动",
                extra.c_str(),
                static_cast<unsigned long long>(view.video_dropped),
                static_cast<unsigned long long>(view.audio_dropped),
                static_cast<unsigned long long>(view.control_dropped));

  char head[768];
  std::snprintf(head, sizeof head,
                "<text x='%d' y='%d' fill='%s' font-size='%d'>zero2w 车载互联</text>"
                "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>"
                "<text x='%d' y='%d' fill='%s' font-size='%d'>主屏平面 %s</text>",
                lay.X(80), lay.Y(150), pal.accent, lay.F(52),
                lay.X(80), lay.Y(200), pal.text, lay.F(30), escape(header).c_str(),
                lay.X(80), lay.Y(232), pal.muted, lay.F(16), plane_name(s.display.primary_plane));

  char guide[512];
  std::snprintf(guide, sizeof guide, "<text x='%d' y='%d' fill='%s' font-size='%d'>%s</text>",
                lay.X(80), lay.Y(604), pal.warn, lay.F(22), escape(guidance).c_str());

  std::string svg;
  svg.reserve(8192);
  svg += "<svg xmlns='http://www.w3.org/2000/svg' width='1280' height='720' viewBox='0 0 1280 720'>";
  svg += calibration_filter(cal);   // 未启用校准时为空串
  // 校准作用于**合成后的整帧**（照抄 LIVI 合成器），所以包含副屏平面在内。
  if (cal.active) svg += "<g filter='url(#dispcal)'>";
  // 底色铺满整帧：safe area 之外的区域属于车机遮挡/圆角区，仍需底色避免穿帮。
  svg += "<rect width='100%' height='100%' fill='";
  svg += pal.bg;
  svg += "'/>";
  svg += head;
  svg += rows;
  svg += overlay;
  svg += guide;
  svg += footer;
  // 导航条放最后：它占据 footer 之下的空带，与既有元素不重叠。
  svg += nav;
  svg += aux;
  if (cal.active) svg += "</g>";
  svg += "</svg>";
  return svg;
}

}  // namespace mvp
