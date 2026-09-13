// CarLife 车机硬键码表 —— 名称 → KEYCODE_* 的唯一映射点。
//
// 【为什么单独一个文件】原来这段映射藏在 carlife_input.cpp 的匿名命名空间里，
// 既无法被单测直接调用（于是只能靠"两端自洽"的端到端验证），也不方便按参考表逐行核对。
// 现在它是公共 API + 有自己的单测。
//
// 【表本身的来源，逐行可复核】
//   Reference/apollo-DuerOS/CarLife-Android-Vehicle-V2.0/carlife-sdk/src/main/java/
//     com/baidu/carlife/sdk/internal/protocol/ServiceTypes.kt:358-408
//   （同一张表也出现在官方 C++ 车机库
//     Reference/.../CarLife-Vehicle-Lib/LibSource/include/CTranRecvPackageProcess.h 的
//     E_PACKAGE_HEAD_TYPE 枚举里，取值一致。）
//
//   名称            十进制  十六进制  参考表行
//   HOME              1     0x01      358
//   PHONE_CALL        2     0x02      359
//   PHONE_END         3     0x03      360
//   PHONE_END_MUTE    4     0x04      361
//   HFP               5     0x05      362
//   SELECTOR_NEXT     6     0x06      363
//   SELECTOR_PREVIOUS 7     0x07      364
//   SETTING           8     0x08      365
//   MEDIA             9     0x09      366
//   RADIO            10     0x0A      367
//   NAV              11     0x0B      368
//   SRC              12     0x0C      369
//   MODE             13     0x0D      370
//   BACK             14     0x0E      371
//   SEEK_SUB         15     0x0F      372
//   SEEK_ADD         16     0x10      373
//   VOLUME_SUB       17     0x11      374
//   VOLUME_ADD       18     0x12      375
//   MUTE             19     0x13      376
//   OK               20     0x14      377
//   MOVE_LEFT        21     0x15      378
//   MOVE_RIGHT       22     0x16      379
//   MOVE_UP          23     0x17      380
//   MOVE_DOWN        24     0x18      381
//   MOVE_UP_LEFT     25     0x19      382
//   MOVE_UP_RIGHT    26     0x1A      383
//   MOVE_DOWN_LEFT   27     0x1B      384
//   MOVE_DOWN_RIGHT  28     0x1C      385
//   TEL              29     0x1D      386
//   MAIN             30     0x1E      387
//   MEDIA_START      31     0x1F      388
//   MEDIA_STOP       32     0x20      389
//   VR_START         33     0x21      390
//   VR_STOP          34     0x22      391
//   NUMBER_0..9      35..44 0x23..0x2C 392-401
//   NUMBER_STAR      45     0x2D      402
//   NUMBER_POUND     46     0x2E      404
//   NUMBER_DEL       47     0x2F      406
//   NUMBER_CLEAR     48     0x30      407
//   NUMBER_ADD       49     0x31      408
//
// 【重要】这不是 Android 的 ADB keycode。历史 bug：我们曾把 "up" 发成 19 ——
// 而 19 在 CarLife 表里是 MUTE（也就是说一直在按静音键）。详见 keycode_map.cpp 的注释。
#pragma once
#include <cstdint>

namespace carlife {

// 名称 → CarLife KEYCODE_*。无法识别返回 0（调用方应丢弃并计数，不要发 0）。
// 接受的名称见 keycode_map.cpp 的映射表；大小写不敏感。
// 下面几个名字在 CarLife 表里【没有】对应项，采用最接近的已知键，属于适配器的命名选择：
//   "menu"  → MAIN(0x1E)       （没有 KEYCODE_MENU）
//   "play"  → MEDIA_START(0x1F)（没有 KEYCODE_PLAY）
//   "pause" → MEDIA_STOP(0x20) （没有 KEYCODE_PAUSE）
int32_t carlife_keycode_for(const char* name);

}  // namespace carlife
