#!/usr/bin/env bash
# 路线 C：全主线（mainline u-boot，无需 ATF）+ 主线内核
# 注意: 主线 dts 未描述 AP6256(mmc1) 与 AC200 100M 网口，见 HARDWARE.md 第 11 节
set -euo pipefail
ZSRC="${ZSRC:-${1:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}}"
OUT="${OUT:-$ZSRC/../build-out/zero2w-mainline}"
JOBS="${JOBS:-$(nproc)}"
UBO="$ZSRC/02-mainline-linux-uboot/u-boot-mainline-master"
KER="$ZSRC/02-mainline-linux-uboot/linux-mainline-master"
CC="aarch64-linux-gnu-"
mkdir -p "$OUT"

make -C "$UBO" ARCH=arm CROSS_COMPILE="$CC" orangepi_zero2w_defconfig
make -C "$UBO" ARCH=arm CROSS_COMPILE="$CC" -j"$JOBS"
install -m 644 "$UBO/u-boot-sunxi-with-spl.bin" "$OUT/"

make -C "$KER" ARCH=arm64 CROSS_COMPILE="$CC" sunxi_defconfig
"$KER/scripts/config" --file "$KER/.config" \
  --enable BRCMFMAC BRCMFMAC_SDIO BT_BCM BT_HCIUART BT_HCIUART_BCM \
  MFD_AXP20X_I2C REGULATOR_AXP20X SUN4I_EMAC DRM_PANFROST DRM_SUN4I \
  IR_SUNXI LEDS_GPIO INPUT_SUN4I_LRADC_KEYS MTD_SPI_NOR SPI_SPINOR_MTD \
  COMMON_CLK_SUNXI PINCTRL_SUN50I_H616_PINCTRL
make -C "$KER" ARCH=arm64 CROSS_COMPILE="$CC" olddefconfig
make -C "$KER" ARCH=arm64 CROSS_COMPILE="$CC" -j"$JOBS" Image dtbs modules
install -m 644 "$KER/arch/arm64/boot/Image" "$OUT/"
install -m 644 "$KER/arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dtb" "$OUT/"
( cd "$KER" && make ARCH=arm64 CROSS_COMPILE="$CC" INSTALL_MOD_PATH="$OUT/rootfs" modules_install )
( cd "$OUT/rootfs" && tar czf "$OUT/modules.tar.gz" lib )
cp "$KER/.config" "$OUT/kernel.config"
echo "[i] 产物: $OUT"
