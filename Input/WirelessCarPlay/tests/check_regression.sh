#!/bin/bash
# 判定：原有 media_client_tests 的失败是我的改动引入的，还是本来就有的？
# 做法：从 git 取出**原始**的 client 源+头，单独编一份跑同一个旧测试。
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1
INC="-I Input/WirelessCarPlay/include -I Core/Convert/include -I Core/MainMenu/include -I Core/Web/include"
CORE_SRC="Core/MainMenu/src/session_core.cpp Core/Convert/src/real_media_store.cpp Core/Web/src/display_config.cpp"

mkdir -p /tmp/orig_inc/wirelesscarplay
git show HEAD:Input/WirelessCarPlay/src/catplay_media_client.cpp > /tmp/orig_client.cpp 2>/dev/null || { echo "✗ 取不到原始 cpp"; exit 1; }
git show HEAD:Input/WirelessCarPlay/include/wirelesscarplay/catplay_media_client.hpp > /tmp/orig_inc/wirelesscarplay/catplay_media_client.hpp 2>/dev/null || { echo "✗ 取不到原始 hpp"; exit 1; }
echo "原始文件行数: cpp=$(wc -l < /tmp/orig_client.cpp) hpp=$(wc -l < /tmp/orig_inc/wirelesscarplay/catplay_media_client.hpp)"

echo "=== A) 旧测试 × 原始 client（HEAD 版本）==="
timeout 600 g++ -std=c++20 -O1 -w -o /tmp/old_orig $INC /tmp/orig_inc \
  Input/WirelessCarPlay/tests/media_client_tests.cpp /tmp/orig_client.cpp $CORE_SRC -lpthread 2>&1 | head -10
echo "--- build exit=${PIPESTATUS[0]} ---"
if [ -x /tmp/old_orig ]; then for i in 1 2; do timeout 60 /tmp/old_orig; echo "--- 第 $i 次 exit=$? ---"; done; fi

echo "=== B) 旧测试 × 我的新 client（对照）==="
timeout 600 g++ -std=c++20 -O1 -w -o /tmp/old_new $INC \
  Input/WirelessCarPlay/tests/media_client_tests.cpp \
  Input/WirelessCarPlay/src/catplay_media_client.cpp $CORE_SRC -lpthread 2>&1 | head -10
echo "--- build exit=${PIPESTATUS[0]} ---"
if [ -x /tmp/old_new ]; then for i in 1 2; do timeout 60 /tmp/old_new; echo "--- 第 $i 次 exit=$? ---"; done; fi

pkill -9 -f '/tmp/old_orig' 2>/dev/null; pkill -9 -f '/tmp/old_new' 2>/dev/null
rm -f /tmp/cpmf-client-* 2>/dev/null
echo "已清理"
