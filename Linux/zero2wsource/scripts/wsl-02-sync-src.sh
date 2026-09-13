#!/bin/bash
# 把需要的源码从 /mnt/d 同步到 WSL ext4（编译速度提升 10x+，并避开 Windows 路径问题）
set -e
WIN="${1:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}"
DST="${DST:-$HOME/opi/src}"
mkdir -p "$DST"
for rel in \
  01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9 \
  01-soc-h616-h618-boot-kernel/u-boot-orangepi-v2024.01 \
  01-soc-h616-h618-boot-kernel/arm-trusted-firmware-h616-bl31 \
  01-soc-h616-h618-boot-kernel/orangepi-build \
  03-wifi-bt-ap6256/orangepi-firmware
do
  echo "==> rsync $rel"
  mkdir -p "$DST/$(dirname $rel)"
  rsync -a --delete --exclude '.git' "$WIN/$rel/" "$DST/$rel/"
done
echo "==> 恢复 .git（便于以后 git status / 打 patch）"
for rel in 01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9; do
  [ -d "$DST/$rel/.git" ] || ( cd "$DST/$rel" && git init -q && git remote add origin https://github.com/orangepi-xunlong/linux-orangepi.git )
done
echo "SRC_SYNCED -> $DST"
du -sh "$DST"/*/* 2>/dev/null | sort -h
