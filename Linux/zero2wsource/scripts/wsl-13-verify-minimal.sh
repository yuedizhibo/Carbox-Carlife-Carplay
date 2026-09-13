#!/usr/bin/env bash
# ============================================================
# 核对极简内核产物: 板载驱动必须 built-in / 无关项必须关 / 内存与启动关键值
#   bash scripts/wsl-13-verify-minimal.sh            # 两个变体都查
#   VARIANT=tiny bash scripts/wsl-13-verify-minimal.sh
# 退出码: 0 = 全部通过
# ============================================================
set -uo pipefail
VARS="${VARIANT:-minimal tiny}"
FULL_IMG="${FULL_IMG:-$HOME/opi/out/kernel/Image}"

# 板载设备驱动/功能: 必须 =y
YES=(ARCH_SUNXI GPIO_SYSFS GPIO_CDEV OF_GPIO OF_OVERLAY PINCTRL_SUN50I_H616 PINCTRL_SUN50I_H616_R
 SUNXI_CCU SUN50I_H616_CCU SUNXI_RSB NVMEM_SUNXI_SID SUNXI_WATCHDOG SUN8I_THERMAL I2C_MV64XXX
 I2C_CHARDEV SPI_SUN4I SPI_SPIDEV SPI_MASTER IR_SUNXI LEDS_GPIO KEYBOARD_GPIO KEYBOARD_SUN4I_LRADC
 MFD_AXP20X_RSB MFD_AXP20X_I2C MFD_AC200 MFD_SUN6I_PRCM REGULATOR_AXP20X REGULATOR_FIXED_VOLTAGE
 MMC_SUNXI MMC_BLOCK MTD MTD_SPI_NOR MTD_OF_PARTS SPI_MEM SUN50I_DE2_BUS
 STAGING STAGING_MEDIA VIDEO_SUNXI VIDEO_SUNXI_CEDRUS
 SND_SUN50IW9_CODEC SND_SUN50I_CODEC_ANALOG SND_SOC_AW8738 SND_SOC_SUNXI_MACH
 BRCMFMAC BRCMFMAC_SDIO MAC80211 CFG80211 CFG80211_WEXT RFKILL BT BT_BCM BT_HCIUART
 BT_HCIUART_BCM BT_HCIUART_SERDEV SUNXI_GMAC SUN4I_EMAC MDIO_SUN4I PHYLIB
 USB_EHCI_HCD USB_EHCI_HCD_PLATFORM USB_OHCI_HCD_PLATFORM USB_MUSB_HDRC USB_MUSB_SUNXI
 USB_GADGET USB_CONFIGFS USB_CONFIGFS_F_FS USB_F_FS USB_STORAGE USB_UAS
 SERIAL_8250 SERIAL_8250_CONSOLE RTC_DRV_SUN6I CPU_FREQ ARM_ALLWINNER_SUN50I_CPUFREQ_NVMEM
 THERMAL PM_SLEEP HWSPINLOCK SUN6I_MSGBOX IIO EXT4_FS VFAT_FS TMPFS OVERLAY_FS
 NETFILTER NF_NAT NF_CONNTRACK NFT_MASQ IP_NF_IPTABLES IP_MULTICAST IP_MROUTE IP_PIMSM_V2
 BRIDGE VLAN_8021Q IPV6 CRYPTO_AES CRYPTO_CMAC KEYS)

# 必须保持为模块：厂商将地址管理器延后到 SID 初始化完成后再 probe。
MOD=(SUNXI_ADDR_MGT)

# 板上没有 / 不要的: 必须不是 y 也不是 m
NO=(CAN CAN_RAW ISDN WAN HAMRADIO IEEE802154 WIMAX MHI RPMSG STAGING_RTL8723BS
 DRM DRM_SUN4I DRM_SUN8I_DW_HDMI DRM_DW_HDMI DRM_PANFROST DRM_I915 DRM_AMDGPU DRM_VKMS
 SND_SOC_HDMI_CODEC FB VT VIRTIO_NET VIRTIO_PCI XEN KVM ACPI EFI FTRACE FUNCTION_TRACER
 KPROBES UPROBES PERF_EVENTS DEBUG_INFO DEBUG_VM GCOV KCOV KSM ZRAM MEMCG CPUSETS BLK_CGROUP
 BLK_DEV_INITRD RD_GZIP BPF_SYSCALL MODVERSIONS MODULE_SIG SECURITY_LOCKDOWN_LSM
 MEDIA_SUBDRV_AUTOSELECT DVB_CORE MEDIA_DIGITAL_TV_SUPPORT MEDIA_RADIO_SUPPORT MEDIA_SDR_SUPPORT
 SERIO LOGO UEVENT_HELPER INPUT_MOUSEDEV INPUT_UINPUT TRANSPARENT_HUGEPAGE
 W1 TCG NFC TEE IP_VS IP_SET NET_CLS_U32 NET_SCH_SFQ BRIDGE_EBT_BROUTE
 SND_HDA SND_SEQUENCER USB_SERIAL)

# 允许被 select/default y 拉回的小项(只提示, 不算失败)
SOFT=(IIO_BUFFER IIO_KFIFO_BUF IIO_TRIGGERED_BUFFER STAGING DEBUG_FS MEDIA_CONTROLLER HUGETLB_PAGE HW_RANDOM)  # arm64 默认/select 拉回, 合计 <30KB

# 只用来在 System.map 里做 built-in 证据抽查
MAPSYM=(brcmf cedrus ac200 axp20x sunxi_musb mv64xxx sun4i_spi hci_uart
 sun8i_ths sunxi_sid aw8738)

rc=0
for V in $VARS; do
  D="$HOME/opi/out-$V/kernel"; C="$D/kernel.config"; M="$D/System.map"
  if [ ! -f "$C" ]; then echo "!! 没有 $C (先跑 wsl-11)"; rc=1; continue; fi
  echo "================= 变体 $V ================="
  echo "  版本后缀 : $(grep -m1 '^CONFIG_LOCALVERSION=' "$C")"
  echo "  规模     : =y $(grep -cE '=y$' "$C")  =m $(grep -cE '=m$' "$C")  Image $(numfmt --to=iec --suffix=B "$(stat -c %s "$D/Image")")"
  echo "  内存关键 : $(grep -hoE 'CONFIG_(CMA_SIZE_MBYTES|CMA_AREAS|HZ|NR_CPUS|LOG_BUF_SHIFT|BASE_SMALL)=[^ ]*' "$C" | tr '\n' ' ')"
  echo "  启动关键 : -Os=$(grep -c '^CONFIG_CC_OPTIMIZE_FOR_SIZE=y' "$C") MODULES=$(grep -c '^CONFIG_MODULES=y' "$C") INITRD=$(grep -c '^CONFIG_BLK_DEV_INITRD=y' "$C") PANIC_TIMEOUT=$(grep -m1 '^CONFIG_PANIC_TIMEOUT=' "$C" | cut -d= -f2)"

  bad=""
  for s in "${YES[@]}"; do grep -q "^CONFIG_${s}=y$" "$C" || bad="$bad $s"; done
  if [ -n "$bad" ]; then echo "  !! 板载项没有 built-in:$bad"; rc=1
  else echo "  ✅ 板载项全部 =y (${#YES[@]} 项)"; fi

  bad=""
  for s in "${MOD[@]}"; do grep -q "^CONFIG_${s}=m$" "$C" || bad="$bad $s"; done
  if [ -n "$bad" ]; then echo "  !! 厂商时序模块没有保持 =m:$bad"; rc=1
  else echo "  ✅ 厂商时序模块全部 =m (${#MOD[@]} 项)"; fi

  bad=""
  for s in "${NO[@]}"; do grep -qE "^CONFIG_${s}=[ym]" "$C" && bad="$bad $s"; done
  if [ -n "$bad" ]; then echo "  !! 这些项没关掉:$bad"; rc=1
  else echo "  ✅ 无关项全部为 n (${#NO[@]} 项)"; fi

  soft=""
  for s in "${SOFT[@]}"; do grep -qE "^CONFIG_${s}=[ym]" "$C" && soft="$soft $s"; done
  if [ -n "$soft" ]; then echo "  ℹ️  被 select/default y 拉回(代价很小, 彻底砍法见 TUNING.md §8):$soft"; fi

  if [ -f "$M" ]; then
    z=0; miss=""
    for s in "${MAPSYM[@]}"; do n=$(grep -c "$s" "$M"); [ "${n:-0}" -eq 0 ] && { z=$((z+1)); miss="$miss $s"; }
      printf "      %-14s %4s 个符号\n" "$s" "$n"; done
    if [ "$z" -gt 0 ]; then echo "  !! System.map 里找不到:$miss -> 有驱动没进内核"; rc=1
    else echo "  ✅ System.map 抽查 ${#MAPSYM[@]} 组驱动符号全部存在(built-in 确认)"; fi
  fi
done

if [ -f "$FULL_IMG" ] && [ -f "$HOME/opi/out-minimal/kernel/Image" ] && [ -f "$HOME/opi/out-tiny/kernel/Image" ]; then
  awk -v a="$(stat -c %s "$FULL_IMG")" -v b="$(stat -c %s "$HOME/opi/out-minimal/kernel/Image")" \
      -v c="$(stat -c %s "$HOME/opi/out-tiny/kernel/Image")" \
      'BEGIN{printf "================= 体积对比 =================\n  full=%.1f MiB   minimal=%.1f MiB (-%.1f%%)   tiny=%.1f MiB (-%.1f%%)\n", a/1048576, b/1048576, 100*(a-b)/a, c/1048576, 100*(a-c)/a}'
fi
if [ $rc -eq 0 ]; then echo "VERIFY_MINIMAL_OK"; else echo "VERIFY_MINIMAL_FAILED"; fi
exit $rc
