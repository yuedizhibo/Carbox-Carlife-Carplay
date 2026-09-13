#!/bin/bash
# CarPlay 全量接口测试运行器。
#
# 为什么不用 root CMakeLists：那个文件归属集成方维护，本任务的写入范围只有
# Input/WirelessCarPlay/**，所以测试自带一个独立编译路径。集成时把
#   add_executable(media_ext_tests Input/WirelessCarPlay/tests/media_ext_tests.cpp)
#   target_link_libraries(media_ext_tests PRIVATE synthetic_wireless Threads::Threads)
#   add_test(NAME media_ext_tests COMMAND media_ext_tests)
# 三行加进 root CMakeLists 即可（见 README 的「集成步骤」）。
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1
INC="-I Input/WirelessCarPlay/include -I Core/Convert/include -I Core/MainMenu/include -I Core/Web/include"
CORE_SRC="Core/MainMenu/src/session_core.cpp Core/Convert/src/real_media_store.cpp Core/Web/src/display_config.cpp"

echo "=== 1) 编译新测试（含 Core 依赖 TU）==="
timeout 600 g++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -o /tmp/ext_tests $INC \
  Input/WirelessCarPlay/tests/media_ext_tests.cpp \
  Input/WirelessCarPlay/src/catplay_media_client.cpp $CORE_SRC -lpthread 2>&1 | head -40
rc=${PIPESTATUS[0]}
echo "--- build exit=$rc ---"
[ "$rc" = 0 ] || { echo "编译失败，停止"; exit 1; }

echo "=== 2) 跑新测试（超时 60s 强制）==="
timeout 60 /tmp/ext_tests; echo "--- ext_tests exit=$? ---"

echo "=== 3) 回归：原有 media_client_tests（确认旧通路没被改坏）==="
timeout 600 g++ -std=c++20 -O1 -Wno-unused-parameter -o /tmp/old_tests $INC \
  Input/WirelessCarPlay/tests/media_client_tests.cpp \
  Input/WirelessCarPlay/src/catplay_media_client.cpp $CORE_SRC -lpthread 2>&1 | head -20
rc2=${PIPESTATUS[0]}
echo "--- old build exit=$rc2 ---"
if [ "$rc2" = 0 ]; then timeout 60 /tmp/old_tests; echo "--- media_client_tests exit=$? ---"; fi

echo "=== 4) 清残（测试进程/套接字）==="
pkill -9 -f '/tmp/ext_tests' 2>/dev/null; pkill -9 -f '/tmp/old_tests' 2>/dev/null
rm -f /tmp/cpmf-ext-* 2>/dev/null
echo "已清理"
