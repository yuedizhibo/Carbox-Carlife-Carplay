#!/usr/bin/env bash
K="${K:-$HOME/opi/src/01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9}"
cd "$K"
echo "=== 1) 谁匹配 allwinner,sun50i-h616-rtc ==="
grep -rn "sun50i-h616-rtc" drivers/rtc/ | head -5
echo "=== 2) Allwinner CAN 驱动 ==="
grep -rni "^config CAN_[A-Z_]*SUN\|^config SUN.*CAN" --include=Kconfig drivers/net/can | head -8
ls drivers/net/can/
echo "=== 3) emac1 节点 ==="
awk "/emac1: ethernet@/,/^\t\t\}/" arch/arm64/boot/dts/allwinner/sun50i-h616.dtsi | head -14
echo "=== 4) MDIO_SUNXI 定义? ==="
grep -rn "^config MDIO_SUNXI" --include=Kconfig . | head -3
echo "=== 5) 现有 .config 里的相关项 ==="
if [ -f .config ]; then grep -E "^CONFIG_(VIDEO_HANTRO|V4L_MEM2MEM_DRIVERS|MEDIA_CONTROLLER_REQUEST_API|RTC_DRV_SUN|CAN_SUN|MDIO_SUN|VIDEO_SUNXI)" .config; else echo "  (无 .config)"; fi