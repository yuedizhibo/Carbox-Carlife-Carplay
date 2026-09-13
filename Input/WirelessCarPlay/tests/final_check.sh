#!/bin/bash
# 收尾：确认没留临时诊断、删掉 Temp 中间产物、跑最终验证。
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1

echo "=== 1) 检查是否残留临时诊断 ==="
if grep -n '\[dbg\]\|临时诊断' Input/WirelessCarPlay/src/catplay_media_client.cpp Input/WirelessCarPlay/tests/media_ext_tests.cpp 2>/dev/null; then
  echo "✗ 还有残留"
else
  echo "✓ 无残留（无 [dbg]、无「临时诊断」）"
fi

echo "=== 2) 删除 Temp 中间产物（协议表已进 README）==="
rm -f Temp/cpmf-ext-table.md Temp/append-ext-table.sh && echo "✓ 已删"

echo "=== 3) 严格告警编译我的两个文件 ==="
INC="-I Input/WirelessCarPlay/include -I Core/Convert/include -I Core/MainMenu/include -I Core/Web/include"
g++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -c -o /tmp/f1.o $INC Input/WirelessCarPlay/src/catplay_media_client.cpp 2>&1 | head -20
echo "--- client exit=${PIPESTATUS[0]} ---"

echo "=== 4) 最终跑一遍端到端测试 ==="
timeout 600 g++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -o /tmp/ext_tests $INC \
  Input/WirelessCarPlay/tests/media_ext_tests.cpp \
  Input/WirelessCarPlay/src/catplay_media_client.cpp \
  Core/MainMenu/src/session_core.cpp Core/Convert/src/real_media_store.cpp Core/Web/src/display_config.cpp \
  -lpthread 2>&1 | grep -E 'error|warning' | head -10
timeout 60 /tmp/ext_tests; echo "--- ext_tests exit=$? ---"

pkill -9 -f '/tmp/ext_tests' 2>/dev/null
rm -f /tmp/cpmf-ext-* 2>/dev/null
echo "已清理"
