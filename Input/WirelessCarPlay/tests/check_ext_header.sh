#!/bin/bash
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1
cat > /tmp/ext_syntax.cpp <<'EOF'
#include "wirelesscarplay/catplay_media_ext.hpp"
using namespace mvp::catplay_media::ext;
int main(){
  // 正向/反向各来一发，确保模板与助手都能实例化
  std::array<TouchPoint,2> pts{}; pts[0].x=10; pts[1].id=1;
  auto r = encode_multitouch({pts.data(),pts.size()});
  auto k = encode_up(UType::Key, 19, 0, 0, 0, "up");
  std::array<uint8_t,kVehiclePayloadBytes> v{};
  put_vehicle_payload(v, 42, 1500, 55, 300, 21, 12345, 45.5, -73.6, 180.0f, 0x1, 3, 0, 2, 0, true, "VIN123");
  return int(r.size + k.size + v[46]);
}
EOF
echo "=== syntax-only ==="
g++ -std=c++20 -fsyntax-only -Wall -Wextra -Wno-unused-parameter \
  -I Input/WirelessCarPlay/include -I Core/Convert/include -I Core/MainMenu/include \
  /tmp/ext_syntax.cpp 2>&1 | head -40
echo "--- syntax exit=${PIPESTATUS[0]} ---"
echo "=== 真正编译+连接（确认无缺符号）==="
g++ -std=c++20 -O1 -o /tmp/ext_syntax \
  -I Input/WirelessCarPlay/include -I Core/Convert/include -I Core/MainMenu/include \
  /tmp/ext_syntax.cpp 2>&1 | head -30
echo "--- link exit=${PIPESTATUS[0]} ---"
/tmp/ext_syntax; echo "--- run exit=$? ---"
