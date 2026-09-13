#!/usr/bin/env bash
# Take mainline u-boot (LF sources, works with swig 4.3) and build orangepi_zero2w_defconfig
set -uo pipefail
SRC="${SRC:-$HOME/opi/src}"
DIR="$SRC/02-mainline-linux-uboot"
UB="$DIR/u-boot-mainline"
DL="$HOME/opi/dl"
OUT="${OUT:-$HOME/opi/out}"
URL="https://codeload.github.com/u-boot/u-boot/tar.gz/refs/heads/master"
T="$DL/u-boot-mainline.tar.gz"
mkdir -p "$DIR" "$DL"
if ! gzip -t "$T" 2>/dev/null; then
  for i in $(seq 1 20); do curl -fsSL -C - --retry 3 -o "$T" "$URL" && break; sleep 5; done
fi
gzip -t "$T" 2>/dev/null || { echo "tarball incomplete"; exit 1; }
rm -rf "$UB"; mkdir -p "$UB"
tar --strip-components=1 -xzf "$T" -C "$UB"
echo "=== u-boot version ==="
grep -E "^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION)" "$UB/Makefile" | tr "\n" " "; echo
echo "=== configs/orangepi_zero2w_defconfig ==="
cat "$UB/configs/orangepi_zero2w_defconfig"
echo "=== build ==="
cd "$UB" || exit 1
# binman 需要 ATF 的 bl31.bin（放在 u-boot 源码根目录）
BL31="${BL31:-$OUT/u-boot/bl31.bin}"
[ -f "$BL31" ] || BL31="$SRC/01-soc-h616-h618-boot-kernel/arm-trusted-firmware-h616-bl31/build/sun50i_h616/debug/bl31.bin"
if [ -f "$BL31" ]; then install -m 644 "$BL31" "$UB/bl31.bin"; echo "  + bl31.bin ($(stat -c%s "$BL31") bytes)"; else echo "  ! 缺 bl31.bin，先跑 wsl-06-build-bootloader.sh"; fi
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- orangepi_zero2w_defconfig >/dev/null || { echo defconfig_failed; exit 1; }
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" > "$DL/uboot-mainline.log" 2>&1
rc=$?
tail -8 "$DL/uboot-mainline.log"
if [ $rc -eq 0 ]; then
  mkdir -p "$OUT/u-boot-mainline"
  for f in u-boot-sunxi-with-spl.bin u-boot.bin u-boot-dtb.bin spl/sunxi-spl.bin; do
    if [ -f "$f" ]; then install -m 644 "$f" "$OUT/u-boot-mainline/"; echo "  + $f"; fi
  done
  cp .config "$OUT/u-boot-mainline/uboot.config"
  ls -lh "$OUT/u-boot-mainline"
fi
echo "MAINLINE_UBOOT_EXIT=$rc"