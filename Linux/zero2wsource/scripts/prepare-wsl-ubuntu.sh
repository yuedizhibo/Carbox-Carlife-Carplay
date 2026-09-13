#!/usr/bin/env bash
# Orange Pi Zero 2W 内核定制环境准备（Ubuntu 22.04 / 24.04，推荐 WSL2）
# 用法: bash scripts/prepare-wsl-ubuntu.sh [源码目录]
set -euo pipefail
SRC="${1:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}"
echo "[i] 源码目录: $SRC"

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential bc bison flex git ca-certificates ccache curl wget xz-utils \
  libssl-dev libncurses-dev device-tree-compiler u-boot-tools \
  crossbuild-essential-arm64 gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  gcc-arm-linux-gnueabi g++-arm-linux-gnueabi \
  rsync swig python3 python3-pip unzip zip pigz kmod fakeroot \
  debootstrap qemu-user-static binfmt-support whiptail libelf-dev

# aarch64: 内核/主线 u-boot/ATF ；arm-linux-gnueabi: Allwinner v2018.05 BSP u-boot
aarch64-linux-gnu-gcc --version | head -1
arm-linux-gnueabi-gcc --version | head -1
dtc --version || true

# 从 Windows 盘挂载的仓库常有 dubious ownership / 检出缺失
git config --global --add safe.directory '*' || true

# 建议在 Linux 本地 fs（~/opi）编译：/mnt/d IO 慢，且 Windows 侧检出的
# drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c 等文件可能缺失，rsync 后修复一次。
if [[ ! -d ~/opi ]]; then
  read -rp "[?] 把源码 rsync 到 ~/opi/src（推荐，可避开 Windows 路径问题）? [Y/n] " a
  if [[ ! ${a:-Y} =~ ^[Nn] ]]; then
    mkdir -p ~/opi && rsync -a --info=progress2 "$SRC/" ~/opi/src/
    cd ~/opi/src
    for r in $(find . -maxdepth 2 -name .git -printf '%h\n'); do
      git -C "$r" config core.protectNTFS false
      git -C "$r" checkout --force 2>/dev/null || true
    done
    echo "[i] 之后请用: export ZSRC=~/opi/src"
  fi
fi

# 走本地代理（Windows 侧代理需打开“允许局域网连接”）
if [[ -n "${PROXY:-}" ]]; then
  git config --global http.proxy  "http://$PROXY"
  git config --global https.proxy "http://$PROXY"
  echo "[i] git 代理: $PROXY"
fi
echo '[i] 完成。下一步: bash scripts/build-kernel-bsp.sh'
