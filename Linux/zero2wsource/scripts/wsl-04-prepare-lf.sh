#!/usr/bin/env bash
# 确保 WSL 侧源码是 GitHub 直取的 LF 版本（Windows 那份只做归档，不能编译）
set -euo pipefail
SRC="${SRC:-$HOME/opi/src}"
K="$SRC/01-soc-h616-h618-boot-kernel"
mkdir -p "$K"

reclone() {
  local dir="$K/$1" branch="$2" url="$3" clean=YES cr
  echo "==> $1 ($branch)"
  rm -rf "$dir"
  git clone --quiet --depth 1 --single-branch -b "$branch" "$url" "$dir"
  if [ -n "$(git -C "$dir" status --porcelain | head -1)" ]; then clean=NO; fi
  cr=$(tr -cd "\r" < "$dir/Makefile" | wc -c)
  echo "    HEAD=$(git -C "$dir" rev-parse --short HEAD)  clean=$clean  CR_in_Makefile=$cr"
}

reclone linux-orangepi-6.1-sun50iw9 orange-pi-6.1-sun50iw9 https://github.com/orangepi-xunlong/linux-orangepi.git
reclone u-boot-orangepi-v2024.01 v2024.01 https://github.com/orangepi-xunlong/u-boot-orangepi.git

echo "==> arm-trusted-firmware-h616-bl31 (pin 4b9be5a)"
ATF="$K/arm-trusted-firmware-h616-bl31"
rm -rf "$ATF"
git clone --quiet --depth 1 https://github.com/ARM-software/arm-trusted-firmware.git "$ATF"
git -C "$ATF" fetch --quiet --depth 1 origin 4b9be5abfa8b1a7c7e00776da01768d3358e648a
git -C "$ATF" checkout --quiet FETCH_HEAD
echo "    HEAD=$(git -C "$ATF" rev-parse --short HEAD)"

echo "==> orangepi-build / firmware 从 Windows 同步（文本文件转 LF）"
WIN=/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource
for rel in 01-soc-h616-h618-boot-kernel/orangepi-build 03-wifi-bt-ap6256/orangepi-firmware; do
  rsync -a --delete --exclude ".git" "$WIN/$rel/" "$SRC/$rel/"
done
find "$SRC/03-wifi-bt-ap6256" "$SRC/01-soc-h616-h618-boot-kernel/orangepi-build" -type f \
  \( -name "*.txt" -o -name "*.conf" -o -name "*.cfg" -o -name "*.ini" -o -name "*.sh" -o -name "*.cmd" \) -print0 \
  | xargs -0 -r sed -i "s/\r$//"
echo "PREPARE_LF_DONE"
du -sh "$K"/* "$SRC/03-wifi-bt-ap6256"/*