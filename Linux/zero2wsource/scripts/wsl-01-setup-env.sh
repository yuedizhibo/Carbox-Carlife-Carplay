#!/bin/bash
# 在 WSL 里以 root 运行：安装 ARM64 交叉工具链与内核构建依赖
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  build-essential gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  gcc-arm-linux-gnueabi g++-arm-linux-gnueabi \
  bc bison flex libssl-dev libelf-dev device-tree-compiler \
  cpio rsync git kmod fakeroot debootstrap wget curl ca-certificates \
  libncurses-dev dwarves zstd xz-utils lz4 lzop file dpkg-dev
echo "=== 版本 ==="
aarch64-linux-gnu-gcc --version | head -1
arm-linux-gnueabi-gcc --version | head -1
dtc --version
make --version | head -1
echo "APT_ENV_READY"
