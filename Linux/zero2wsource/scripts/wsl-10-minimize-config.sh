#!/usr/bin/env bash
# ============================================================
# Orange Pi Zero 2W —— 生成"只含板载设备 + 省内存 + 快启动"的极简内核配置
# 在 WSL 内运行:  bash /mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource/scripts/wsl-10-minimize-config.sh
# 产物: Windows 侧 configs/zero2w-minimal_defconfig  +  configs/zero2w-minimal.config
# ============================================================
set -euo pipefail
SRC="${SRC:-$HOME/opi/src}"
WIN="${WIN:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}"
KER="$SRC/01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9"
CC="${CC:-aarch64-linux-gnu-}"
# 本项目默认使用无本地显示、保留 CarPlay 音视频与 MFI 链路的配置。
SPEC="${SPEC:-$WIN/configs/zero2w-headless-mfi.spec}"
BASE="${BASE:-linux_sunxi64_defconfig}"
cd "$KER"
[[ -d $KER ]] || { echo "找不到内核树 $KER"; exit 1; }
[[ -f $SPEC ]] || { echo "找不到 spec $SPEC"; exit 1; }

g() { grep -cE "$1" .config || true; }

echo "==> [1/5] 基底 $BASE"
make ARCH=arm64 CROSS_COMPILE="$CC" "$BASE" >/dev/null
Y0=$(g '=y$'); M0=$(g '=m$')
echo "    裁剪前:  =y $Y0   =m $M0   (共 $((Y0+M0)) 项)"

echo "==> [2/5] 应用 spec 裁剪"
python3 "$WIN/scripts/minimize_config.py" .config "$SPEC"

echo "==> [3/5] olddefconfig 收敛依赖"
make ARCH=arm64 CROSS_COMPILE="$CC" olddefconfig 2>&1 | grep -Ei "warning|error" | head -20 || true
Y1=$(g '=y$'); M1=$(g '=m$')
echo "    裁剪后:  =y $Y1   =m $M1   (共 $((Y1+M1)) 项, 减少 $(( (Y0+M0)-(Y1+M1) )) )"

echo "==> [4/5] 自检: 板载驱动必须保留 / 无关总线必须为 n"
BAD=""; MISS=""; NFAIL=""
chk() { # 名称 期望(y|m|n) 是否必须
  local s="$1" want="$2" must="${3:-1}" v="-"
  if   grep -qE "^CONFIG_${s}=y$" .config; then v=y
  elif grep -qE "^CONFIG_${s}=m$" .config; then v=m
  elif grep -qE "^CONFIG_${s} is not set|^# CONFIG_${s} is not set" .config; then v=n
  fi
  printf "  %-42s = %-3s\n" "$s" "$v"
  case "$want" in
    ym) if [ "$v" = y ] || [ "$v" = m ]; then :
        elif [ "$v" = - ]; then MISS="$MISS $s"
        else BAD="$BAD $s"; fi ;;
    y)  if [ "$v" = y ]; then :
        elif [ "$v" = - ]; then MISS="$MISS $s"
        else BAD="$BAD $s"; fi ;;
    n)  if [ "$v" = y ] || [ "$v" = m ]; then NFAIL="$NFAIL $s"; fi ;;
  esac
}
sec() { echo "  ---- $1 ----"; }
sec "SoC / GPIO / 低速总线"
for s in ARCH_SUNXI GPIO_SYSFS GPIO_CDEV OF_GPIO OF_OVERLAY PINCTRL_SUN50I_H616 PINCTRL_SUN50I_H616_R \
         SUNXI_CCU SUN50I_H616_CCU SUNXI_RSB NVMEM_SUNXI_SID SUNXI_ADDR_MGT SUNXI_WATCHDOG SUN8I_THERMAL I2C_MV64XXX \
         SPI_SUN4I SPI_SPIDEV IR_SUNXI LEDS_GPIO KEYBOARD_GPIO KEYBOARD_SUN4I_LRADC; do chk "$s" ym; done
sec "PMIC AXP313a / AC200"
for s in MFD_AXP20X_RSB MFD_AXP20X_I2C REGULATOR_AXP20X REGULATOR_FIXED_VOLTAGE MFD_AC200 REGULATOR; do chk "$s" ym; done
sec "存储 TF + SPI NOR"
for s in MMC_SUNXI MMC_BLOCK MTD MTD_SPI_NOR MTD_OF_PARTS SPI_MEM; do chk "$s" ym; done
sec "Headless 视频解码 / 音频（本地 HDMI/GPU 已关闭）"
for s in SUN50I_DE2_BUS VIDEO_SUNXI_CEDRUS SND_SUN50IW9_CODEC SND_SUN50I_CODEC_ANALOG SND_SOC_AW8738; do chk "$s" ym; done
sec "Wi-Fi / BT / 以太网"
for s in BRCMFMAC BRCMFMAC_SDIO MAC80211 CFG80211_WEXT BT_BCM BT_HCIUART_BCM BT_HCIUART_SERDEV \
         SUNXI_GMAC SUN4I_EMAC MDIO_SUN4I PHYLIB; do chk "$s" ym; done
sec "USB host + gadget(CarPlay 有线) / 串口 / RTC / 调频"
for s in USB_EHCI_HCD_PLATFORM USB_MUSB_SUNXI USB_CONFIGFS_F_FS USB_F_FS SERIAL_8250_CONSOLE \
         RTC_DRV_SUN6I ARM_ALLWINNER_SUN50I_CPUFREQ_NVMEM CPU_FREQ THERMAL PM_SLEEP; do chk "$s" ym; done
sec "网络功能面(CarPlay 依赖, 不许砍)"
for s in NETFILTER NF_NAT NF_CONNTRACK NFT_MASQ IP_NF_IPTABLES IP_MULTICAST IP_MROUTE IP_PIMSM_V2 \
         BRIDGE VLAN_8021Q IPV6_MROUTE EXT4_FS VFAT_FS TMPFS OVERLAY_FS; do chk "$s" ym; done
sec "必须为 n 的项"
for s in CAN CAN_RAW CAN_VX_CANIF ISDN WAN HAMRADIO IEEE802154 WIMAX MHI STAGING VIRTIO_VSOCKETS \
         XEN DRM DRM_SUN4I DRM_SUN8I_DW_HDMI DRM_DW_HDMI DRM_PANFROST DRM_I915 DRM_AMDGPU DRM_VKMS DRM_VIRTGPU \
         SND_SOC_HDMI_CODEC FB VT USB_GEMIO DEBUG_INFO FTRACE FUNCTION_TRACER \
         KPROBES UPROBES PERF_EVENTS DEBUG_FS BLK_DEV_INITRD RD_LZ4 RD_LZMA ZRAM KSM \
         CGROUP_HUGETLB CPUSETS MEMCG IOSCHED_BFQ MODULE_SIG ACPI EFI KVM LIVEPATCH \
         SOUND_PRIME SND_SOC_INTEL_SST_TOPLEVEL USB_SERIAL WILINK_PLATFORM MT7921E; do chk "$s" n; done

if [ -n "$BAD" ]; then echo; echo "!! 自检失败(依赖被砍掉或白名单没生效):$BAD"; exit 1; fi
if [ -n "$MISS" ]; then echo "   提示: 下列符号本内核树里不存在(已忽略):$MISS"; fi
if [ -n "$NFAIL" ]; then echo "   警告: 下列项本想关掉, 但被依赖/基底配置拉回来了:$NFAIL"; else echo "   无关项全部确认为 n"; fi

echo "==> [5/5] 存档 defconfig"
make ARCH=arm64 CROSS_COMPILE="$CC" savedefconfig >/dev/null
mkdir -p "$WIN/configs"
cp defconfig "$WIN/configs/zero2w-minimal_defconfig"
cp .config   "$WIN/configs/zero2w-minimal.config"
{
  echo "# Orange Pi Zero 2W 极简配置统计 (内核 $(make -s ARCH=arm64 kernelversion))"
  echo "生成时间 : $(date -Iseconds)"
  echo "基底     : $BASE"
  echo "=y       : $Y1   (裁剪前 $Y0)"
  echo "=m       : $M1   (裁剪前 $M0)"
  echo "合计     : $((Y1+M1))  (裁剪前 $((Y0+M0)),  -$(awk -v a=$((Y0+M0)) -v b=$((Y1+M1)) 'BEGIN{printf "%.0f%%", 100*(a-b)/a}'))"
  echo "defconfig行数 : $(wc -l < "$WIN/configs/zero2w-minimal_defconfig")"
} > "$WIN/configs/zero2w-minimal.stats.txt"
cat "$WIN/configs/zero2w-minimal.stats.txt"
echo MINIMAL_CONFIG_DONE
