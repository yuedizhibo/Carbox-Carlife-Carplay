#!/usr/bin/env bash
# 只编设备树（改 dts 时秒级验证），并列出可用 overlay
set -euo pipefail
ZSRC="${ZSRC:-${1:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}}"
KER="${KER:-$ZSRC/01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9}"
CC="${CC:-aarch64-linux-gnu-}"
cd "$KER"
[[ -f .config ]] || make ARCH=arm64 CROSS_COMPILE="$CC" linux_sunxi64_defconfig >/dev/null
make ARCH=arm64 CROSS_COMPILE="$CC" dtbs -j"$(nproc)"
ls -l arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dtb
echo '[i] 现成 overlay:'
ls arch/arm64/boot/dts/allwinner/overlay/ 2>/dev/null | head -30
echo
echo '单独编一个 dts 便于定位报错:'
echo '  dtc -I dts -O dtb -i arch/arm64/boot/dts/allwinner -o /tmp/x.dtb \'
echo '      arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dts'
