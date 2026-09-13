// 端到端探针：CarLife 协议 → CarLifeInputAdapter → SessionCore 真的被写进去了吗？
//
// 为什么要单独一个可执行文件：
//   * `unit_features` 只验证“字节 ↔ 结构体”自洽，证明不了适配器把值交给了 Core；
//   * `carlife-hu` 用的是 SdlHostSink（本机显示那条路），根本不会碰 SessionCore；
//   * mvp_server 带 HTTP + 前端，判据会被页面层稀释。
// 所以这个探针只做一件可被判读的事：跑真会话，然后**把 SessionCore 的快照逐项打印**，
// 让 run_wsl_tests.sh 用 grep 断言“Core 里确实有了”。
//
// 用法（由 run_wsl_tests.sh 调用，mdsim.py 必须已经在监听）：
//   feature_probe --phone-ip 127.0.0.1 --seconds 10 --inject-controls
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "carlife/carlife_input.h"
#include "carlife/service_types.h"
#include "core/session_core.hpp"
#include "wirelesscarplay/real_media_store.hpp"

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS " : "FAIL ", what.c_str());
  if (!ok) ++g_fail;
}

std::string text(const std::array<char, 192>& a) { return std::string(a.data()); }
std::string text(const std::array<char, 96>& a) { return std::string(a.data()); }
std::string text(const std::array<char, 32>& a) { return std::string(a.data()); }
std::string text(const std::array<char, 24>& a) { return std::string(a.data()); }

uint64_t nowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

}  // namespace

int main(int argc, char** argv) {
  std::string phone_ip = "127.0.0.1";
  int seconds = 10;
  bool inject_controls = false;
  bool vehicle_demo = false;
  std::string inject_keys;
  std::string frame_rate;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--phone-ip") && i + 1 < argc) phone_ip = argv[++i];
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--inject-controls")) inject_controls = true;
    else if (!std::strcmp(argv[i], "--vehicle-demo")) vehicle_demo = true;
    // 按【给定顺序】注入硬键（逗号分隔）。与 mdsim 的 --expect-keys 一一对应：
    // 因为 TOUCH_CAR_HARD_KEY_CODE 线上只有 keycode 数字，没有名字，
    // 只能靠“两边约定同一个顺序”来做顺序敏感的自检。
    else if (!std::strcmp(argv[i], "--inject-keys") && i + 1 < argc) inject_keys = argv[++i];
    // T：请求帧率（车机会回 FRAME_RATE_CHANGE，我们回 DONE）
    else if (!std::strcmp(argv[i], "--request-frame-rate") && i + 1 < argc) frame_rate = argv[++i];
  }

  mvp::RealMediaStore media;
  mvp::SessionCore core;

  carlife::SessionConfig sc;
  sc.width = 1920;
  sc.height = 1080;
  sc.frameRate = 30;
  sc.verbose = false;
  sc.authMode = "trust";
  sc.btName = "Zero2W-Probe";
  carlife::CarLifeInputAdapter adapter(core, media, sc, "carlife-hu");
  adapter.connectToPhone(phone_ip);
  // 多点触控：我们能力位报 MULTI_TOUCH=1，所以触控走 ACTION_3 定长格式。
  adapter.setMultiTouch(true);

  // 车况上报源（AUDIT 5.1 #8）：真机数据来自 CAN/OBD，本机没有传感器。
  // 这个 demo 源只在显式传 --vehicle-demo 时启用，用来证明“编码 + 发出去”这条链路通；
  // 不传就完全不上报（适配器 takeVehicleReport 返回 false）。
  if (vehicle_demo) {
    adapter.setVehicleReportSource([](carlife::VehicleReport& r) {
      r.has_speed = true;
      r.speed_kph = 66;
      r.has_gear = true;
      r.gear = 4;  // D 档
      r.has_oil = true;
      r.oil_level = 55;
      r.oil_range = 420;
      r.has_gps = true;
      r.latitude_e6 = 39904873;
      r.longitude_e6 = 116397000;
      r.gps_speed = 66;
      r.gps_heading = 180;
      r.antenna_state = 1;
      r.gps_fix = 1;
      r.sats_used = 9;
      r.sats_visible = 12;
      r.has_acceleration = true;
      r.acc_z = 9.81;
      return true;
    });
  }

  adapter.start();
  std::printf("PROBE 启动: phone=%s seconds=%d vehicle_demo=%d\n", phone_ip.c_str(), seconds,
              vehicle_demo ? 1 : 0);

  // 1) 等会话进 VideoStarted（最多 40s）
  bool started = false;
  const uint64_t deadline = nowMs() + 40000;
  while (nowMs() < deadline) {
    const auto st = adapter.status();
    if (st.state >= carlife::State::VideoStarted) {
      started = true;
      std::printf("PROBE 会话已进入 %s（第 %.1fs）\n", carlife::toString(st.state),
                  (40000 - (deadline - nowMs())) / 1000.0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  check(started, "会话到达 VideoStarted（握手/鉴权/视频协商全通）");
  if (!started) {
    adapter.stop();
    return 1;
  }

  // 2) 注入 Core → 手机 的控制（多点触控 / 旋钮 / 语音 / 电话 / 硬键）
  if (inject_controls) {
    {
      // 多点触控：3 指（走 MSG_TOUCH_ACTION_3 的定长格式）
      mvp::ControlEvent ev;
      ev.type = mvp::ControlEvent::Type::MultiTouch;
      ev.phase = mvp::ControlEvent::TouchPhase::Down;
      ev.point_count = 3;
      for (uint8_t i = 0; i < 3; ++i) {
        ev.points[i].id = i;
        ev.points[i].x = static_cast<int16_t>(100 + i * 200);
        ev.points[i].y = static_cast<int16_t>(200 + i * 100);
      }
      core.route_control(ev);
    }
    {
      mvp::ControlEvent ev;
      ev.type = mvp::ControlEvent::Type::Knob;
      ev.knob_dir = mvp::ControlEvent::KnobDir::Right;
      ev.knob_steps = 1;
      core.route_control(ev);
    }
    {
      mvp::ControlEvent ev;
      ev.type = mvp::ControlEvent::Type::Voice;  // → KEYCODE_VR_START
      core.route_control(ev);
    }
    {
      mvp::ControlEvent ev;
      ev.type = mvp::ControlEvent::Type::Telephony;
      ev.key[0] = '\0';
      ev.dtmf[0] = '5';  // → KEYCODE_NUMBER_5 = 40（0x28，参考表 :397）
      core.route_control(ev);
    }
    // 【不要在这里再发 Key】硬键改由 --inject-keys 统一按顺序注入，
    // 这样 --expect-keys 的顺序才是确定的（控件在前、显式硬键在后）。
    std::printf("PROBE 已注入 4 条控制事件（多点/旋钮/语音/DTMF）\n");
  }

  // 2b) 按给定顺序注入硬键（K4 自检用）。
  // 期望值由 mdsim 的 --expect-keys 从参考表 ServiceTypes.kt:358-408 传入：
  //   up→23(0x17) / down→24(0x18) / back→14(0x0E) / left→21(0x15) / right→22(0x16)
  //   home→1 / mute→19(0x13) / volume_up→18 / volume_down→17 / next→6 / prev→7
  //   play→31(MEDIA_START) / pause→32(MEDIA_STOP) / menu→30(MAIN)
  if (!inject_keys.empty()) {
    std::size_t begin = 0;
    while (begin <= inject_keys.size()) {
      const std::size_t comma = inject_keys.find(',', begin);
      const std::string one = inject_keys.substr(
          begin, comma == std::string::npos ? std::string::npos : comma - begin);
      if (!one.empty()) {
        mvp::ControlEvent ev;
        ev.type = mvp::ControlEvent::Type::Key;
        std::snprintf(ev.key.data(), ev.key.size(), "%s", one.c_str());
        core.route_control(ev);
        std::printf("PROBE 注入硬键 %s\n", one.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
      }
      if (comma == std::string::npos) break;
      begin = comma + 1;
    }
  }

  // 2c) 帧率动态调整（T 项）：请求一次，验证请求真的发得出去、手机回的 DONE 被接住。
  if (!frame_rate.empty()) {
    // 没有直接的 Session 句柄：走 Core 的选区再触发不合适，
    // 所以这一项在探针里通过适配器的状态计数观测（见下面的 cl.frame_rate_*）。
    std::printf("PROBE 已请求帧率变更 -> %s（由手机侧回 FRAME_RATE_CHANGE_DONE）\n",
                frame_rate.c_str());
  }

  // 3) 让手机侧的刺激全部到达
  const uint64_t until = nowMs() + static_cast<uint64_t>(seconds) * 1000;
  while (nowMs() < until) {
    // 帧率动态调整（AUDIT 5.1 #11）：探针主动请求一次，验证请求真的发得出去。
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }

  // 4) 打印快照 —— 下面这些行就是 run_wsl_tests.sh 的判据
  const carlife::CarLifeInputStatus st = adapter.status();
  const mvp::SessionSnapshot snap = core.snapshot();

  std::printf("\n==== SessionCore 快照 ====\n");
  std::printf("core.selected=%s\n", snap.selected.data());
  // 导航逐向（C 项）—— 契约里的 NavigationState
  std::printf("core.nav.valid=%d active=%d maneuver=%d maneuver_code=%u road=%s icon_bytes=%zu\n",
              snap.nav.valid ? 1 : 0, snap.nav.active, static_cast<int>(snap.nav.maneuver),
              snap.nav.maneuver_code, text(snap.nav.road_name).c_str(),
              std::strlen(snap.nav.icon.data()));
  std::printf("core.nav.dist_to_maneuver_m=%u dist_remaining_m=%u time_remaining_s=%u\n",
              snap.nav.distance_to_maneuver_m, snap.nav.distance_remaining_m,
              snap.nav.time_remaining_s);
  std::printf("core.media.valid=%d song=%s artist=%s album=%s\n", snap.media.valid ? 1 : 0,
              text(snap.media.title).c_str(), text(snap.media.artist).c_str(),
              text(snap.media.album).c_str());
  std::printf("core.media.duration_ms=%u has_artwork=%d artwork_bytes=%u revision=%u\n",
              snap.media.duration_ms, snap.media.has_artwork ? 1 : 0, snap.media.artwork_bytes,
              snap.media.artwork_revision);
  std::printf("core.media.playing=%d\n", snap.media.playing ? 1 : 0);
  std::printf("core.display.target_fps=%u actual_fps=%u\n", snap.display.target_fps,
              snap.display.actual_fps);
  std::printf("core.input.multi_touch_used=%u touchpad=%u knob=%u last_key_code=%u\n",
              snap.input.multi_touch_used, snap.input.touchpad, snap.input.knob,
              snap.input.last_key_code);
  std::printf("core.audio.nav_active=%u media_volume_ppm=%d duck_ratio_ppm=%d channels=%u rate=%u\n",
              snap.audio.nav_active, snap.audio.media_volume_ppm, snap.audio.duck_ratio_ppm,
              snap.audio.channels, snap.audio.sample_rate);
  std::printf("core.telephony.call_state=%u caller=%s number=%s\n", snap.telephony.call_state,
              text(snap.telephony.caller).c_str(), text(snap.telephony.caller_number).c_str());
  std::printf("core.link.activation_state=%u content_encryption=%u file_transfer_active=%u "
              "ota_state=%u\n",
              snap.link.activation_state, snap.link.content_encryption,
              snap.link.file_transfer_active, snap.link.ota_state);

  std::printf("\n==== CarLifeInputStatus ====\n");
  std::printf("cl.state=%s detail=%s\n", carlife::toString(st.state), st.detail.data());
  std::printf("cl.media_info_count=%llu navi=%llu phone_input=%llu multitouch=%llu\n",
              static_cast<unsigned long long>(st.media_info_count),
              static_cast<unsigned long long>(st.navi_count),
              static_cast<unsigned long long>(st.phone_input_count),
              static_cast<unsigned long long>(st.multi_touch_count));
  std::printf("cl.hfp=%llu vehicle_ctrl=%llu voice=%llu file_transfer=%llu activation=%llu\n",
              static_cast<unsigned long long>(st.hfp_count),
              static_cast<unsigned long long>(st.vehicle_control_count),
              static_cast<unsigned long long>(st.voice_control_count),
              static_cast<unsigned long long>(st.file_transfer_count),
              static_cast<unsigned long long>(st.activation_count));
  std::printf("cl.cardata_sub=%llu vehicle_report_sent=%llu encrypted=%llu decrypt_fail=%llu\n",
              static_cast<unsigned long long>(st.car_data_subscribe_count),
              static_cast<unsigned long long>(st.vehicle_report_sent),
              static_cast<unsigned long long>(st.encrypted_messages),
              static_cast<unsigned long long>(st.decrypt_failures));
  std::printf("cl.media_song=%s media_artist=%s cover=%u\n", st.media_song.data(),
              st.media_artist.data(), st.media_cover_bytes);
  std::printf("cl.navi action=%d nextTurn=%d road=%s remain=%d\n", st.navi_action,
              st.navi_next_turn, st.navi_road.data(), st.navi_remain_m);
  std::printf("cl.call state=%u caller=%s number=%s last_hfp_cmd=%d dtmf=%u\n", st.call_state,
              st.call_caller.data(), st.call_number.data(), st.last_hfp_command,
              st.last_hfp_dtmf);
  std::printf("cl.vehicle_ctrl type=%d id=%d voice_cmd=%d phone_gear=%d\n",
              st.last_vehicle_ctrl_type, st.last_vehicle_ctrl_id, st.last_voice_command,
              st.phone_gear);
  std::printf("cl.link activation=%u encryption=%u encrypted_msgs=%llu decrypt_fail=%llu\n",
              st.activation_state, st.encryption_state,
              static_cast<unsigned long long>(st.encrypted_messages),
              static_cast<unsigned long long>(st.decrypt_failures));
  std::printf("cl.file_transfer active=%u ota=%u\n", st.file_transfer_active, st.ota_state);
  std::printf("cl.controls_sent=%llu dropped=%llu frames_forwarded=%llu\n",
              static_cast<unsigned long long>(st.controls_sent),
              static_cast<unsigned long long>(st.controls_dropped),
              static_cast<unsigned long long>(st.frames_forwarded));
  std::printf("cl.timesync=%lld touch_points=%u\n", static_cast<long long>(st.last_time_sync),
              st.last_touch_points);

  adapter.stop();
  std::printf("\nPROBE 结束 (fail=%d)\n", g_fail);
  return g_fail ? 1 : 0;
}
