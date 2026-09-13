#!/bin/bash
# 旧测试失败是"我引入的"还是"本来就有"？找编于我改代码之前的旧产物直接跑。
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1
for b in Temp/build-root/media_client_tests Temp/build-input/media_client_tests; do
  if [ -x "$b" ]; then
    echo "== $b =="
    echo "   产物时间: $(stat -c %y "$b" 2>/dev/null | cut -d. -f1)"
    echo "   源码时间: $(stat -c %y Input/WirelessCarPlay/src/catplay_media_client.cpp | cut -d. -f1)"
    timeout 60 "$b"; echo "   exit=$?"
  else
    echo "-- $b 不存在 --"
  fi
done
pkill -9 -f 'media_client_tests' 2>/dev/null
rm -f /tmp/cpmf-client-* 2>/dev/null
echo "已清理"
