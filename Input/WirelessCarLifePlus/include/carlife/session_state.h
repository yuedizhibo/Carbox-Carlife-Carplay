// 会话生命周期与协商信息。
//
// 单独成文件是为了让协议侧（wire/transport/session）不再依赖 SDL/FFmpeg：
// Core 接入路径只需要协议与媒体转发，不需要本地显示栈。
// 依据 carlife-vehicle-lib（百度官方车机库）的划分：库只产出类型化结构体，
// 由宿主机决定如何呈现；本文件即“协议侧可见的最小类型集”。
#pragma once

#include <cstdint>

namespace carlife {

struct VideoInfo {
  int width = 1920;
  int height = 1080;
  int frameRate = 30;
};

enum class State {
  Idle = 0,
  Connected,
  VersionMatched,
  AuthRequested,
  AuthVerified,
  Established,
  VideoInitSent,
  VideoStarted,
  Failed,
  Closed
};

const char* toString(State s);

}  // namespace carlife
