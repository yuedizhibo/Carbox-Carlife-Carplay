#!/usr/bin/env bash
# 路线 B：Zero2W 官方 "next" 栈 = ATF(bl31) + u-boot(v2024.01) + 内核 6.1-sun50iw9
set -euo pipefail
ZSRC="${ZSRC:-${1:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}}"
OUT="${OUT:-$ZSRC/../build-out/zero2w-next}"
JOBS="${JOBS:-$(nproc)}"
KDIR="$ZSRC/01-soc-h616-h618-boot-kernel"
ATF="$KDIR/arm-trusted-firmware-h616-bl31"
UBO="$KDIR/u-boot-orangepi-v2024.01"
KER="$KDIR/linux-orangepi-6.1-sun50iw9"
CC="aarch64-linux-gnu-"
mkdir -p "$OUT"

echo "==> [1/3] ARM Trusted Firmware (BL31, PLAT=sun50i_h616)"
make -C "$ATF" CROSS_COMPILE="$CC" PLAT=sun50i_h616 DEBUG=1 bl31 -j"$JOBS"
BL31="$ATF/build/sun50i_h616/debug/bl31.bin"
[[ -f $BL31 ]] || { echo "缺少 $BL31"; exit 1; }
install -m 644 "$BL31" "$OUT/bl31.bin"
install -m 644 "$BL31" "$UBO/bl31.bin"   # Allwinner fork 在源码根目录找 bl31.bin

echo "==> [2/3] U-Boot（官方 next = v2024.01 全志 fork）"
make -C "$UBO" ARCH=arm CROSS_COMPILE="$CC" orangepi_zero2w_defconfig
make -C "$UBO" ARCH=arm CROSS_COMPILE="$CC" -j"$JOBS"
install -m 644 "$UBO/u-boot-sunxi-with-spl.bin" "$OUT/"

echo "==> [3/3] Linux 6.1-sun50iw9（BSP）"
make -C "$KER" ARCH=arm64 CROSS_COMPILE="$CC" linux_sunxi64_defconfig
make -C "$KER" ARCH=arm64 CROSS_COMPILE="$CC" -j"$JOBS" Image dtbs modules
install -m 644 "$KER/arch/arm64/boot/Image" "$OUT/"
install -m 644 "$KER/arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dtb" "$OUT/"
( cd "$KER" && make ARCH=arm64 CROSS_COMPILE="$CC" INSTALL_MOD_PATH="$OUT/rootfs" modules_install )
( cd "$OUT/rootfs" && tar czf "$OUT/modules.tar.gz" lib )
cp "$KER/.config" "$OUT/kernel.config"

cat <<EOM

完成，产物在 $OUT :
  u-boot-sunxi-with-spl.bin  ->  dd if=... of=/dev/sdX bs=1024 seek=8
  Image / *.dtb / modules.tar.gz  ->  替换 SD 卡 /boot 分区同名文件即可验证
EOM
