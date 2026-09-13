#!/usr/bin/env bash
# 把编译好的 内核 + dtb (+ 模块/固件) 装到已跑官方镜像的 Zero2W，无需重刷 SD
# 用法:
#   BOARD=root@192.168.1.50 VARIANT=minimal bash scripts/deploy-to-board.sh
#   VARIANT: full(默认) | minimal | tiny     tiny/minimal 无模块 -> 自动跳过模块安装
set -euo pipefail
VARIANT="${VARIANT:-full}"
case "$VARIANT" in
  full) OUT="${OUT:-$HOME/opi/out}" ;;
  *)    OUT="${OUT:-$HOME/opi/out-$VARIANT}" ;;
esac
SRC="${SRC:-$HOME/opi/src}"
BOARD="${BOARD:?请设置 BOARD=root@<板子IP>}"
KDIR="$OUT/kernel"
DTB=sun50i-h618-orangepi-zero2w.dtb
FW="$SRC/03-wifi-bt-ap6256/orangepi-firmware"
STAGE=$(mktemp -d)
echo "==> 变体 $VARIANT, 产物目录 $KDIR"

echo "==> 1/5 组装产物与固件包"
mkdir -p "$STAGE/fw/brcm"
cp "$KDIR/Image" "$STAGE/"
cp "$KDIR/dtb/$DTB" "$STAGE/"
MODTAR=""
if [ -f "$OUT/zero2w-modules.tar.gz" ]; then
  cp "$OUT/zero2w-modules.tar.gz" "$STAGE/modules.tar.gz"; MODTAR=modules.tar.gz
elif [ -d "$KDIR/modules/lib" ]; then
  ( cd "$KDIR/modules" && tar czf "$STAGE/modules.tar.gz" lib ); MODTAR=modules.tar.gz
else
  echo "    本变体无模块(驱动全部 built-in) -> 跳过模块安装"
fi
for f in brcm/brcmfmac43455-sdio.bin brcm/brcmfmac43455-sdio.txt brcm/brcmfmac43455-sdio.clm_blob \
         brcm/BCM4345C5.hcd BCM4345C0.hcd nvram_ap6256.txt; do
  [ -f "$FW/$f" ] && cp -f "$FW/$f" "$STAGE/fw/$f" || true
done
tar czf "$STAGE/payload.tar.gz" -C "$STAGE" Image "$DTB" $MODTAR fw
du -h "$STAGE/payload.tar.gz" | awk '{print "    包大小 "$1}'

echo "==> 2/5 上传到 $BOARD"
ssh "$BOARD" 'mkdir -p /root/kdeploy && rm -rf /root/kdeploy/*'
scp -q "$STAGE/payload.tar.gz" "$BOARD:/root/kdeploy/"

echo "==> 3/5 安装模块 + AP6256 固件"
ssh "$BOARD" 'set -e
  cd /root/kdeploy && tar xzf payload.tar.gz
  if [ -f modules.tar.gz ]; then
    tar xzf modules.tar.gz -C /; depmod -a
    echo "  已装模块: $(ls /lib/modules | tr "\n" " ")"
  else
    echo "  无模块可装(全部内建); 建议移走旧模块树避免和 built-in 冲突"
    [ -d /lib/modules ] && mv /lib/modules /lib/modules.bak.$(date +%s) 2>/dev/null || true
  fi
  mkdir -p /lib/firmware/brcm
  cp -f fw/* /lib/firmware/ 2>/dev/null || true
  cp -f fw/brcm/* /lib/firmware/brcm/
  echo "  固件: $(ls /lib/firmware/brcm | tr "\n" " ")"'

echo "==> 4/5 替换 /boot 里的内核与 dtb（自动备份）"
ssh "$BOARD" 'set -e
  cd /root/kdeploy
  BOOT=$(awk "\$2==\"/boot\"{print \$2}" /proc/mounts | head -1)
  if [ -z "$BOOT" ]; then mkdir -p /mnt/boot; mount /dev/mmcblk0p1 /mnt/boot; BOOT=/mnt/boot; fi
  TS=$(date +%s)
  [ -f "$BOOT/Image" ] && cp -f "$BOOT/Image" "$BOOT/Image.bak.$TS"
  for d in "$BOOT/dtb/allwinner" "$BOOT/dtb" "$BOOT/overlay/dtb/allwinner"; do
    [ -d "$d" ] && cp -f sun50i-h618-orangepi-zero2w.dtb "$d/" && echo "  dtb -> $d"
  done
  cp -f Image "$BOOT/Image"
  grep -q "^kdeploy=" "$BOOT/orangepiEnv.txt" 2>/dev/null || echo "kdeploy=$TS" >> "$BOOT/orangepiEnv.txt"
  sync
  [ "$BOOT" = "/mnt/boot" ] && umount /mnt/boot || true
  echo "  备份后缀: .bak.$TS   (回退就 cp 回去)"'

echo "==> 5/5 重启并验证（含内存/启动耗时实测）"
read -rp "现在重启板子? [Y/n] " a || a=Y
case "${a:-Y}" in
 [Nn]*) echo "稍后: ssh $BOARD reboot" ;;
 *) ssh "$BOARD" 'nohup reboot >/dev/null 2>&1 &' || true
    echo "    等 35s 开机..."; sleep 35
    ssh "$BOARD" 'uname -a
      echo "--- 网络接口 ---"; ip -br link
      echo "--- Wi-Fi ---"; iw dev 2>/dev/null | head -3; timeout 8 iwlist wlan0 scan 2>/dev/null | grep -c ESSID
      echo "--- 蓝牙 ---"; hciconfig -a 2>/dev/null | head -3
      echo "--- GPIO ---"; gpioinfo 2>/dev/null | head -3 || ls /dev/gpiochip*
      echo "--- 显示/音频/GPU ---"; ls /dev/dri /dev/snd 2>/dev/null
      echo "--- 温度/调频 ---"; cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null; grep -m1 "" /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
      echo "--- 以太网(AC200 PHY) ---"; dmesg | grep -iE "ephy|gmac|ac200" | tail -5
      echo "=== 运行内存实测 ==="; grep -E "MemTotal|MemFree|MemAvailable|Slab|SUnreclaim|KernelStack|PageTables|CmaTotal" /proc/meminfo
      echo "=== 启动耗时实测 ==="; systemd-analyze 2>/dev/null; systemd-analyze blame 2>/dev/null | head -8
      echo "--- dmesg 异常 ---"; dmesg | grep -iE "error|fail|warn" | tail -12' ;;
esac
rm -rf "$STAGE"
echo "完成。回退: ssh $BOARD 'cp /boot/Image.bak.<TS> /boot/Image && reboot'"
