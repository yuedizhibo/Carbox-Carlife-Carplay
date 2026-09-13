#!/usr/bin/env bash
# 把 WSL 里的构建产物拷一份到 Windows 工作区 build-out/
set -euo pipefail
O=/home/dfkvm/opi/out
D=/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource/build-out
mkdir -p "$D/kernel/dtb" "$D/u-boot" "$D/u-boot-mainline" "$D/config"
cp -f "$O/kernel/Image" "$D/kernel/"
cp -f "$O/kernel/dtb/"*.dtb "$D/kernel/dtb/"
cp -f "$O/zero2w-modules.tar.gz" "$D/kernel/"
cp -f "$O/u-boot/bl31.bin" "$D/u-boot/"
cp -f "$O/u-boot-mainline/u-boot-sunxi-with-spl.bin" "$O/u-boot-mainline/sunxi-spl.bin" "$O/u-boot-mainline/u-boot-dtb.bin" "$D/u-boot-mainline/"
cp -f "$O/kernel/kernel.config" "$O/kernel/zero2w-full.config" "$O/u-boot-mainline/uboot.config" "$D/config/"
echo "=== build-out 内容 ==="
ls -lR "$D" | head -40
du -sh "$D"
echo "COPY_ARTIFACTS_DONE"