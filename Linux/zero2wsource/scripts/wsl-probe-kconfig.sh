#!/usr/bin/env bash
K="${K:-$HOME/opi/src/01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9}"
cd "$K"
for s in VIDEO_HANTRO VIDEO_HANTRO_SUNXI VIDEO_SUNXI_CEDRUS RTC_DRV_SUNXI RTC_DRV_SUN6I MDIO_SUNXI MDIO_SUN4I CAN_SUN4I CAN_DEV; do
  echo "##### $s"
  grep -rn -A9 "^config ${s}$" --include=Kconfig . | head -16
done