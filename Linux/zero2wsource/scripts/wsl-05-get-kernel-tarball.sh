#!/usr/bin/env bash
# 用可续传的 codeload tarball 取内核源码（git 协议大传输会被 TLS 中断）
set -uo pipefail
SRC="${SRC:-$HOME/opi/src}"
K="$SRC/01-soc-h616-h618-boot-kernel"
DL="$HOME/opi/dl"
BR="orange-pi-6.1-sun50iw9"
URL="https://codeload.github.com/orangepi-xunlong/linux-orangepi/tar.gz/refs/heads/$BR"
T="$DL/linux-orangepi-$BR.tar.gz"
DIR="$K/linux-orangepi-6.1-sun50iw9"
mkdir -p "$DL" "$K"

if [ -s "$T" ] && gzip -t "$T" 2>/dev/null; then
  echo "==> 已有完整 tarball"
else
  for i in $(seq 1 40); do
    echo "==> 下载尝试 #$i"
    curl -fL -C - --retry 3 --retry-delay 3 -o "$T" "$URL" && break
    sleep 5
  done
fi

if ! gzip -t "$T" 2>/dev/null; then echo "TARBALL 不完整: $(du -h "$T")"; exit 1; fi
echo "==> tarball OK: $(du -h "$T")"
rm -rf "$DIR"; mkdir -p "$DIR"
tar --strip-components=1 -xzf "$T" -C "$DIR"
echo "    Makefile CR=$(tr -cd "\r" < "$DIR/Makefile" | wc -c)  version=$(make -C "$DIR" -s kernelversion)"
ls "$DIR/arch/arm64/boot/dts/allwinner/" | grep -i zero2w
echo "KERNEL_SRC_READY $DIR"