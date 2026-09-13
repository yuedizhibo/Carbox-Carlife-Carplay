#!/usr/bin/env bash
# FEL(Maskrom) 救砖 / 免 SD-bootloader 直接把 u-boot+内核推进内存
set -euo pipefail
ZSRC="${ZSRC:-${1:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}}"
ST="$ZSRC/01-soc-h616-h618-boot-kernel/sunxi-tools"
OUT="${OUT:-$ZSRC/../build-out/zero2w-next}"
cd "$ST" && make -j"$(nproc)" && sudo install -m 755 sunxi-fel /usr/local/bin/ || true
if ! sunxi-version; then
  echo '板子不在 FEL：断电 -> 短接 Maskrom(或按住 BOOT 键) -> 上电 -> 再接 USB-C'
  exit 1
fi
sudo sunxi-fel -p uboot "$OUT/u-boot-sunxi-with-spl.bin" \
  write 0x40078000 "$OUT/Image" \
  write 0x4a000000 "$OUT/sun50i-h618-orangepi-zero2w.dtb"
echo '[i] 串口里执行: booti 0x40078000 - 0x4a000000'
