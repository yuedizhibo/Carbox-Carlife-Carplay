#!/usr/bin/env bash
# 构建 Orange Pi Zero 2W 定制内核（官方 next = Allwinner BSP 6.1.31 + 板载全外设 + GPIO）
# 在 WSL 里运行：bash /mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource/scripts/wsl-03-build-kernel.sh
set -euo pipefail
SRC="${SRC:-$HOME/opi/src}"
WIN="${WIN:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}"
KER="$SRC/01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9"
OUT="${OUT:-$HOME/opi/out}"
CC="${CC:-aarch64-linux-gnu-}"
J="${J:-$(nproc)}"
FRAG="$WIN/configs/zero2w-full.config"
# GCC 14 把 int-conversion / implicit-function-declaration 等升级为硬错误，全志老驱动会中招 -> 降级为警告
KCFLAGS="${KCFLAGS:--Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=incompatible-pointer-types -Wno-error=discarded-qualifiers}"

[[ -d $KER ]] || { echo "找不到内核树 $KER，先跑 scripts/wsl-02-sync-src.sh"; exit 1; }
command -v "${CC}gcc" >/dev/null || { echo "缺 ${CC}gcc，先跑 wsl-01-setup-env.sh"; exit 1; }
mkdir -p "$OUT"
cd "$KER"

echo "==> [1/4] linux_sunxi64_defconfig + Zero2W 片段"
make ARCH=arm64 CROSS_COMPILE="$CC" linux_sunxi64_defconfig >/dev/null
cat "$FRAG" >> .config
make ARCH=arm64 CROSS_COMPILE="$CC" olddefconfig >/dev/null

echo "==> [2/4] 校验板载驱动是否真的打开"
BAD=""
chk() {
  local s="$1" v
  v=$(grep -E "^CONFIG_${s}=[ym]" .config | head -1 | cut -d= -f2 || true)
  if [ -z "$v" ]; then
    if grep -qE "^# CONFIG_${s} is not set" .config; then v="n"; else v="-"; fi
  fi
  printf "  %-44s = %-3s\n" "$s" "$v"
  [ "$v" = y ] || [ "$v" = m ] || BAD="$BAD $s"
}
sec() { echo "  --- $1 ---"; }
sec "SoC / GPIO / 低速总线"
for s in ARCH_SUNXI PINCTRL_SUN50I_H616 PINCTRL_SUN50I_H616_R GPIO_SYSFS GPIO_CDEV OF_GPIO SUNXI_RSB SUNXI_CCU SUN50I_H616_CCU SUNXI_WATCHDOG SUN8I_THERMAL NVMEM_SUNXI_SID ARM_ALLWINNER_SUN50I_CPUFREQ_NVMEM LEDS_GPIO KEYBOARD_SUN4I_LRADC KEYBOARD_GPIO IR_SUNXI SERIAL_8250 I2C_MV64XXX SPI_SUN4I SPI_SPIDEV I2C_CHARDEV OF_OVERLAY PWM_SUN4I; do chk "$s"; done
sec "存储 / 显示 / GPU / VPU / 音频 / RTC"
for s in MMC_SUNXI MTD_SPI_NOR DRM_SUN4I DRM_SUN8I_DW_HDMI SUN50I_DE2_BUS DRM_PANFROST VIDEO_SUNXI_CEDRUS VIDEO_HANTRO_SUNXI SND_SUN50IW9_CODEC SND_SOC_SUNXI_SUN50IW9_CODEC SND_SUN50I_CODEC_ANALOG SND_SOC_AW8738 RTC_DRV_SUN6I SUN4I_GPADC; do chk "$s"; done
sec "Wi-Fi / BT / 以太网 / USB / CAN"
for s in BRCMFMAC BRCMFMAC_SDIO CFG80211 MAC80211 BT_BCM BT_HCIUART_BCM MFD_AC200 SUN4I_EMAC SUNXI_GMAC MDIO_SUNXI DWMAC_SUN8I BRIDGE NF_TABLES USB_EHCI_HCD_PLATFORM USB_MUSB_SUNXI USB_CONFIGFS_F_FS USB_VIDEO_CLASS CRYPTO_DEV_SUN4I_SS VIDEO_HANTRO; do chk "$s"; done

if [ -n "$BAD" ]; then
  echo
  echo "!! 下列符号未生效（该内核树无此驱动或依赖未满足），不影响其它驱动："
  for b in $BAD; do echo "     - $b"; done
fi

echo "==> [3/4] 编译 Image + dtbs + modules  (-j$J)"
make ARCH=arm64 CROSS_COMPILE="$CC" KCFLAGS="$KCFLAGS" -j"$J" Image dtbs modules

echo "==> [4/4] 收集产物"
K="$OUT/kernel"
rm -rf "$K"; mkdir -p "$K/dtb" "$K/modules"
cp arch/arm64/boot/Image "$K/"
cp arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dtb "$K/dtb/"
cp .config "$K/kernel.config"
cp "$FRAG" "$K/zero2w-full.config"
make ARCH=arm64 CROSS_COMPILE="$CC" KCFLAGS="$KCFLAGS" INSTALL_MOD_PATH="$K/modules" modules_install >/dev/null
( cd "$K/modules" && tar czf "$OUT/zero2w-modules.tar.gz" lib )
echo
echo "================= 完成 ================="
echo "内核版本 : $(make -s ARCH=arm64 kernelversion)-$(grep -m1 "^CONFIG_LOCALVERSION=" .config | cut -d\" -f2)"
echo "模块数量 : $(find "$K/modules" -name "*.ko" | wc -l)"
ls -lh "$K" "$K/dtb"
echo "产物目录 : $K   (Windows 侧可见 \\wsl$\\Debian\\home\\...)"