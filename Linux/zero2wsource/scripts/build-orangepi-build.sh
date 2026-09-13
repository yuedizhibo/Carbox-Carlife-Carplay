#!/usr/bin/env bash
# 路线 A：官方 orangepi-build 一键生成 CarPlay headless 镜像。
# 内置 OpenSSH；允许 root 密码登录；默认密码 root。
# 用法: bash scripts/build-orangepi-build.sh [next] [jammy]
set -euo pipefail
ZSRC="${ZSRC:-$HOME/opi/src}"
BD="$ZSRC/01-soc-h616-h618-boot-kernel/orangepi-build"
BR="${1:-next}"; RE="${2:-jammy}"
cd "$BD"
[[ -x build.sh ]] || { echo "未找到 $BD/build.sh（先执行 wsl-02-sync-src.sh）"; exit 1; }
[[ -f userpatches/config-carplay.conf ]] || { echo "缺少 userpatches/config-carplay.conf"; exit 1; }
[[ -f userpatches/customize-image.sh ]] || { echo "缺少 userpatches/customize-image.sh"; exit 1; }
chmod +x userpatches/customize-image.sh
sudo apt-get install -y git whiptail libncurses-dev tzdata bison flex libssl-dev \
  device-tree-compiler u-boot-tools qemu-user-static binfmt-support pigz rsync
# 命令行变量覆盖 config-carplay.conf，避免进入桌面/密码选择交互。
# Zero2W: next = 内核6.1 + u-boot v2024.01 + ATF。
sudo ./build.sh carplay BOARD=orangepizero2w BRANCH="$BR" RELEASE="$RE" \
  BUILD_DESKTOP=no BUILD_MINIMAL=yes KERNEL_CONFIGURE=no ROOTPWD=root

echo "[i] SSH 登录: ssh root@<板子IP>  密码: root"
echo "[i] 镜像目录: $BD/output/images/"; ls -lh "$BD/output/images/" 2>/dev/null || true
