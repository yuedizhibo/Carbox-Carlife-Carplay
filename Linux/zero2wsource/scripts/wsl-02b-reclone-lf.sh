#!/usr/bin/env bash
# 在 WSL 内直接克隆 LF 版源码（Windows 归档那份是 CRLF，不能用来编译）
set -euo pipefail
SRC="${SRC:-$HOME/opi/src}"
K="$SRC/01-soc-h616-h618-boot-kernel"
mkdir -p "$K"
cd "$K"

clone() { # dir branch url
  local dir="$1" branch="$2" url="$3"
  if [ -d "$dir/.git" ]; then echo "SKIP $dir (已存在)"; return 0; fi
  rm -rf "$dir"
  echo "==> clone $branch  ($url)"
  git clone --quiet --depth 1 --single-branch -b "$branch" "$url" "$dir"
}

clone linux-orangepi-6.1-sun50iw9 orange-pi-6.1-sun50iw9 https://github.com/orangepi-xunlong/linux-orangepi.git
clone u-boot-orangepi-v2024.01 v2024.01 https://github.com/orangepi-xunlong/u-boot-orangepi.git
clone arm-trusted-firmware-h616-bl31 master https://github.com/ARM-software/arm-trusted-firmware.git
( cd arm-trusted-firmware-h616-bl31 && git fetch --quiet --depth 1 origin 4b9be5abfa8b1a7c7e00776da01768d3358e648a && git checkout --quiet FETCH_HEAD )

echo "==> 校验换行符（应为 LF）"
for f in linux-orangepi-6.1-sun50iw9/Kconfig linux-orangepi-6.1-sun50iw9/Makefile; do
  printf "  %-52s CR=%s\n" "$f" "$(tr -cd "\r" < "$K/$f" | wc -c)"
done

echo "==> 修掉从 Windows 同步过来的文本固件里的 CRLF（nvram 等）"
FW="$SRC/03-wifi-bt-ap6256/orangepi-firmware"
if [ -d "$FW" ]; then
  find "$FW" -type f \( -name "*.txt" -o -name "*.conf" -o -name "*.cfg" -o -name "*.ini" \) -print0 \
    | xargs -0 -r sed -i "s/\r$//"
  echo "  nvram 文件已转换: $(find "$FW" -name "nvram_ap6256.txt" | wc -l)"
fi
echo "RECLONE_DONE"
du -sh "$K"/* 2>/dev/null