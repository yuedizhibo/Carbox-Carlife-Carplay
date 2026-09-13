#!/usr/bin/env bash
# 验证构建产物：dtb 板载节点 / 驱动归属 / 固件清单
set -u
SRC="${SRC:-$HOME/opi/src}"
OUT="${OUT:-$HOME/opi/out}"
KER="$SRC/01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9"
DTB="$OUT/kernel/dtb/sun50i-h618-orangepi-zero2w.dtb"
CFG="$OUT/kernel/kernel.config"

echo "=== 1) 产物 ==="
file "$OUT/kernel/Image"
ls -l "$OUT/kernel/Image" "$DTB"
ls -l "$OUT/u-boot-mainline/u-boot-sunxi-with-spl.bin" "$OUT/u-boot/bl31.bin"

echo "=== 2) dtb 里的板载节点 ==="
dtc -I dtb -O dts "$DTB" 2>/dev/null > /tmp/z2w.dts
wc -l /tmp/z2w.dts
for n in 'xunlong,orangepi-zero2w' 'mmc@4020000' 'mmc@4021000' 'mmc-pwrseq-simple' 'ethernet@5030000' 'pmic@36' 'x-powers,ac200' 'ac200-ephy' 'ir@' 'codec' 'gpu@1800000' 'hdmi' 'spi@4025000' 'jedec,spi-nor' 'lradc' 'gpio-leds' 'sun50i-h616-rtc'; do
  c=$(grep -c -- "$n" /tmp/z2w.dts)
  if [ "$c" -gt 0 ]; then s="OK($c)"; else s="--"; fi
  echo "    $n : $s"
done

echo "=== 3) 关键驱动 y/m 归属 ==="
for s in BRCMFMAC BRCMFMAC_SDIO BT_BCM BT_HCIUART_BCM MFD_AC200 SUNXI_GMAC MDIO_SUN4I SUN4I_EMAC MMC_SUNXI MTD_SPI_NOR DRM_SUN4I DRM_SUN8I_DW_HDMI DRM_PANFROST SND_SUN50IW9_CODEC SND_SUN50I_CODEC_ANALOG SND_SOC_AW8738 RTC_DRV_SUN6I SUNXI_RSB PINCTRL_SUN50I_H616 GPIO_SYSFS GPIO_CDEV IR_SUNXI KEYBOARD_SUN4I_LRADC KEYBOARD_GPIO LEDS_GPIO SUN4I_GPADC VIDEO_SUNXI_CEDRUS USB_MUSB_SUNXI USB_CONFIGFS_F_FS USB_VIDEO_CLASS CRYPTO_DEV_SUN4I_SS ARM_ALLWINNER_SUN50I_CPUFREQ_NVMEM NVMEM_SUNXI_SID SUN8I_THERMAL OF_OVERLAY; do
  v=$(grep -m1 -E "^CONFIG_${s}=" "$CFG" | cut -d= -f2)
  if [ -z "$v" ]; then v="(未设置)"; fi
  echo "    $s = $v"
done

echo "=== 4) 编译出来的 .ko 里与本板相关的 ==="
find "$OUT/kernel/modules" -name '*.ko*' 2>/dev/null | sed 's|.*/||' | grep -Ei 'brcm|btbcm|sunxi|sun4i|sun50i|sun8i|gmac|emac|panfrost|cedrus|hantro|aw8|mv64xxx|spi_sun|phy-sun|nvmem|rsb|lradc|gpadc|8250' | sort | head -40
echo "    模块总数: $(find "$OUT/kernel/modules" -name '*.ko*' | wc -l)"

echo "=== 5) AP6256 固件是否齐全 ==="
FW="$SRC/03-wifi-bt-ap6256/orangepi-firmware"
for f in brcm/brcmfmac43455-sdio.bin brcm/brcmfmac43455-sdio.txt brcm/brcmfmac43455-sdio.clm_blob brcm/BCM4345C5.hcd BCM4345C0.hcd nvram_ap6256.txt; do
  if [ -f "$FW/$f" ]; then echo "    OK   $f"; else echo "    MISS $f"; fi
done
echo "    nvram 里的 CR 数量(应为0): $(tr -cd '\r' < "$FW/nvram_ap6256.txt" | wc -c)"
echo "VERIFY_DONE"