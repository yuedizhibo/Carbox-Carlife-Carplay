#!/bin/bash
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1

echo "=== 1) CMake 路径（Temp/build-input）==="
timeout 600 cmake --build Temp/build-input --target synthetic_wireless -j2 > /tmp/l5b.log 2>&1
echo "   build exit=$?  $(grep -c 'error:' /tmp/l5b.log) errors"
tail -1 /tmp/l5b.log

echo
echo "=== 2) 端到端测试：期望 61 checks / exit 0 ==="
timeout 600 bash Input/WirelessCarPlay/tests/run_ext_tests.sh 2>&1 \
  | grep -E 'checks executed|ext_tests exit|build exit|FAIL|error:'

echo
echo "=== 3) 关键新增断言确实在测（导航三件事）==="
grep -n 'maneuver_code == 250\|maneuver == Maneuver::None\|distance_remaining_m == 12345\|set_navigation_state(n2)\|!= extract\|Second St' \
  Input/WirelessCarPlay/tests/media_ext_tests.cpp | sed 's/^/   /'

echo
echo "=== 4) README 硬键章节（确认中文完好、两表齐全）==="
awk '/### 硬键标识/,/### 音频角色映射/' Input/WirelessCarPlay/README.md | head -22

echo
echo "=== 5) 残留检查 ==="
grep -rn 'NavInfo\|nav_info\|dbg\]\|临时诊断' Input/WirelessCarPlay/ || echo "   ✓ 无残留"
