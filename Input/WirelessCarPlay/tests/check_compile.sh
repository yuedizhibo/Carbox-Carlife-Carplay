#!/bin/bash
# L5 编译检查：先单独编我的 TU（隔离其它 agent 正在改的 Core 文件），再试 CMake 目标。
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1
INC="-I Input/WirelessCarPlay/include -I Core/Convert/include -I Core/MainMenu/include -I Core/Web/include"
echo "=== 1) 单独编译 catplay_media_client.cpp ==="
g++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -c -o /tmp/cpmc.o $INC \
  Input/WirelessCarPlay/src/catplay_media_client.cpp 2>&1 | head -60
echo "--- client compile exit=${PIPESTATUS[0]} ---"
echo "=== 2) 单独编译 wireless_carplay.cpp（确认没连带破坏）==="
g++ -std=c++20 -O1 -c -o /tmp/wcp.o $INC Input/WirelessCarPlay/src/wireless_carplay.cpp 2>&1 | head -20
echo "--- wireless_carplay exit=${PIPESTATUS[0]} ---"
