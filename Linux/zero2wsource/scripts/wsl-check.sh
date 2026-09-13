#!/bin/bash
echo "user=$(id -un) uid=$(id -u)"
echo "distro=$(. /etc/os-release; echo $PRETTY_NAME)"
echo "kernel=$(uname -r)"
for t in make gcc g++ bison flex bc dtc aarch64-linux-gnu-gcc arm-linux-gnueabi-gcc rsync git cpio fakeroot debootstrap python3 lzop xz; do
  if p=$(command -v "$t" 2>/dev/null); then echo "HAVE  $t  -> $p"; else echo "NEED  $t"; fi
done
echo "nproc=$(nproc)  mem=$(free -g | awk '/^Mem/{print $2}')GB"
df -h "$HOME" | tail -1
echo "--- /mnt/d visible? ---"
ls -d /mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource 2>/dev/null || echo "no /mnt/d"
