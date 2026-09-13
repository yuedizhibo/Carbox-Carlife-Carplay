#!/usr/bin/env bash
# 引导链：ATF bl31(sun50i_h616) + U-Boot v2024.01(官方 next 分支, orangepi_zero2w)
set -euo pipefail
SRC="${SRC:-$HOME/opi/src}"
K="$SRC/01-soc-h616-h618-boot-kernel"
ATF="$K/arm-trusted-firmware-h616-bl31"
UBO="$K/u-boot-orangepi-v2024.01"
OUT="${OUT:-$HOME/opi/out}"
CC="${CC:-aarch64-linux-gnu-}"
J="${J:-$(nproc)}"
mkdir -p "$OUT/u-boot"

echo "==> [1/2] ATF bl31.bin (PLAT=sun50i_h616)"
make -C "$ATF" CROSS_COMPILE="$CC" PLAT=sun50i_h616 DEBUG=1 bl31 -j"$J" > "$OUT/atf.log" 2>&1 || { tail -25 "$OUT/atf.log"; exit 1; }
BL31="$ATF/build/sun50i_h616/debug/bl31.bin"
test -f "$BL31" || { echo "没有生成 $BL31"; tail -25 "$OUT/atf.log"; exit 1; }
install -m 644 "$BL31" "$OUT/u-boot/bl31.bin"
install -m 644 "$BL31" "$UBO/bl31.bin"
echo "    bl31.bin = $(stat -c%s "$BL31") bytes"

echo "==> [2/2] U-Boot orangepi_zero2w_defconfig"
make -C "$UBO" ARCH=arm CROSS_COMPILE="$CC" orangepi_zero2w_defconfig >/dev/null
make -C "$UBO" ARCH=arm CROSS_COMPILE="$CC" -j"$J" > "$OUT/uboot.log" 2>&1 || { tail -30 "$OUT/uboot.log"; exit 1; }
tail -6 "$OUT/uboot.log"
for f in u-boot-sunxi-with-spl.bin u-boot.bin u-boot-dtb.bin spl/sunxi-spl.bin; do
  if [ -f "$UBO/$f" ]; then install -m 644 "$UBO/$f" "$OUT/u-boot/"; echo "    + $f"; fi
done
cp "$UBO/.config" "$OUT/u-boot/uboot.config"
echo
echo "================= 引导链产物 ================="
ls -lh "$OUT/u-boot"
echo "烧写: dd if=u-boot-sunxi-with-spl.bin of=/dev/sdX bs=1024 seek=8"