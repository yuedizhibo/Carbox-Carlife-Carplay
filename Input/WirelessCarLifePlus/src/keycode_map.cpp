#include "carlife/keycode_map.h"

#include <string>

#include "carlife/service_types.h"

namespace carlife {

// 全部取值见 keycode_map.h 顶部的对照表（来源 ServiceTypes.kt:358-408）。
//
// 【这里修的是个真 bug】改动前的实现把 "up" 映到 19、"down" 映到 20、"back" 映到 4，
// 那是 Android 的 KEYCODE_DPAD_UP / DPAD_DOWN / BACK。而 CarLife 的
// TOUCH_CAR_HARD_KEY_CODE 用的是它自己那张表，于是线上实际含义是：
//     19 → KEYCODE_MUTE      （按静音！）
//     20 → KEYCODE_OK        （按确定！）
//      4 → KEYCODE_PHONE_END_MUTE
// 之前的"验证通过"只证明了我们自己写的两端互相自洽（mdsim 照我们的实现回显），
// 所以这次把映射抽成公共函数 + 单测直接对着参考表的十进制/十六进制断言。
int32_t carlife_keycode_for(const char* name) {
  if (!name || !*name) return 0;
  std::string text(name);
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  // ── 方向键（"up/down/left/right" 是 Core 桌面与触控板最常用的四个）──
  if (text == "up") return keycode::MOVE_UP;
  if (text == "down") return keycode::MOVE_DOWN;
  if (text == "left") return keycode::MOVE_LEFT;
  if (text == "right") return keycode::MOVE_RIGHT;
  if (text == "move_up_left") return keycode::MOVE_UP_LEFT;
  if (text == "move_up_right") return keycode::MOVE_UP_RIGHT;
  if (text == "move_down_left") return keycode::MOVE_DOWN_LEFT;
  if (text == "move_down_right") return keycode::MOVE_DOWN_RIGHT;
  // ── 确认 / 返回 / 主界面 ──
  if (text == "enter" || text == "select" || text == "ok") return keycode::OK;
  if (text == "back" || text == "escape") return keycode::BACK;
  if (text == "home") return keycode::HOME;
  if (text == "menu" || text == "main") return keycode::MAIN;  // 无 KEYCODE_MENU
  // ── 音量 / 静音 ──
  if (text == "volume_up") return keycode::VOLUME_ADD;
  if (text == "volume_down") return keycode::VOLUME_SUB;
  if (text == "mute") return keycode::MUTE;
  // ── 媒体播放 ──
  if (text == "play" || text == "media_start") return keycode::MEDIA_START;  // 无 KEYCODE_PLAY
  if (text == "pause" || text == "stop" || text == "media_stop") {
    return keycode::MEDIA_STOP;  // 无 KEYCODE_PAUSE
  }
  if (text == "next" || text == "selector_next") return keycode::SELECTOR_NEXT;
  if (text == "prev" || text == "previous") return keycode::SELECTOR_PREVIOUS;
  if (text == "seek_add") return keycode::SEEK_ADD;
  if (text == "seek_sub") return keycode::SEEK_SUB;
  // ── 功能入口 ──
  if (text == "setting" || text == "settings") return keycode::SETTING;
  if (text == "media") return keycode::MEDIA;
  if (text == "nav" || text == "navigation") return keycode::NAV;
  if (text == "radio") return keycode::RADIO;
  if (text == "src" || text == "source") return keycode::SRC;
  if (text == "mode") return keycode::MODE;
  // ── 电话 / HFP ──
  if (text == "tel" || text == "phone") return keycode::TEL;
  if (text == "hfp") return keycode::HFP;
  if (text == "answer" || text == "call") return keycode::PHONE_CALL;
  if (text == "hangup" || text == "end" || text == "reject") return keycode::PHONE_END;
  if (text == "end_mute") return keycode::PHONE_END_MUTE;
  // ── 语音 ──
  if (text == "voice" || text == "vr" || text == "assistant") return keycode::VR_START;
  if (text == "vr_stop") return keycode::VR_STOP;
  // ── 拨号盘字符 ──
  if (text == "star" || text == "*") return keycode::NUMBER_STAR;
  if (text == "pound" || text == "#") return keycode::NUMBER_POUND;
  if (text == "del" || text == "delete") return keycode::NUMBER_DEL;
  if (text == "clear") return keycode::NUMBER_CLEAR;
  if (text == "plus" || text == "+") return keycode::NUMBER_ADD;
  // 单个数字 0..9 → KEYCODE_NUMBER_0(0x23) + n（表里 35..44 连续递增）
  if (text.size() == 1 && text[0] >= '0' && text[0] <= '9') {
    return keycode::NUMBER_0 + (text[0] - '0');
  }
  // 直接写十进制数字串（如 "23"）也接受，方便诊断与 mdsim 对齐。
  if (text.size() <= 2) {
    int32_t v = 0;
    bool digits = true;
    for (char c : text) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
      v = v * 10 + (c - '0');
    }
    if (digits && text.size() == 2) return v;  // 两位纯数字 = 直接给 keycode
  }
  return 0;
}

}  // namespace carlife
