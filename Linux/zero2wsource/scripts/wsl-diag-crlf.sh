#!/usr/bin/env bash
K="$HOME/opi/src/01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9"
echo "HOME=$HOME"
echo "global autocrlf: $(git config --global --get core.autocrlf || echo none)"
echo "repo autocrlf  : $(git -C "$K" config --get core.autocrlf || echo none)"
for f in Kconfig Makefile lib/Kconfig.debug arch/arm64/Kconfig; do
  n=$(tr -cd "\r" < "$K/$f" | wc -c)
  echo "  $f : CR=$n"
done
echo "--- git status (前5行) ---"
git -C "$K" status --porcelain | head -5
echo "--- 抽样 .c 文件 CR ---"
for f in $(find "$K/kernel" -name "*.c" | head -5); do echo "$f CR=$(tr -cd "\r" < "$f" | wc -c)"; done