#!/usr/bin/env bash
set -euo pipefail
IMG="${1:-/home/dfkvm/opi/images/extracted/Orangepizero2w_1.0.4_debian_bookworm_server_linux6.1.31/Orangepizero2w_1.0.4_debian_bookworm_server_linux6.1.31.img}"
case "$IMG" in /dev/*|*PhysicalDrive*) echo "refusing a physical/block-device path" >&2; exit 2;; esac
[[ -f $IMG ]] || { echo "not an image file: $IMG" >&2; exit 2; }
L="$(losetup --read-only --find --show --partscan "$IMG")"
mkdir -p /mnt/zroot /mnt/zboot
cleanup(){ mountpoint -q /mnt/zboot && umount /mnt/zboot || true; mountpoint -q /mnt/zroot && umount /mnt/zroot || true; losetup -d "$L" 2>/dev/null || true; }
trap cleanup EXIT
mount -o ro "${L}p2" /mnt/zroot
mount -o ro "${L}p1" /mnt/zboot
echo "LOOP=$L"
echo ===BOOT===
find /mnt/zboot -maxdepth 2 -type f -printf '%P %s\n' | sort | head -80
echo ===ENV===
for f in /mnt/zboot/orangepiEnv.txt /mnt/zboot/armbianEnv.txt /mnt/zboot/extlinux/extlinux.conf /mnt/zboot/boot.cmd; do
  if [[ -f "$f" ]]; then echo "---$f"; cat "$f"; fi
done
echo ===NM===
ls -l /mnt/zroot/etc/systemd/system/multi-user.target.wants/NetworkManager.service /mnt/zroot/etc/NetworkManager/system-connections 2>&1 || true
grep -R -v '^psk=' /mnt/zroot/etc/NetworkManager/system-connections 2>/dev/null || true
echo ===MODULES===
ls /mnt/zroot/lib/modules
echo ===FSTAB===
cat /mnt/zroot/etc/fstab
echo ===KERNEL_CONFIG===
grep -E '^(CONFIG_(BRCMFMAC|BRCMFMAC_SDIO|CFG80211|MMC_SUNXI|MMC|EXT4_FS|SERIAL_8250|SERIAL_8250_CONSOLE|VT|DRM|FB)=)' /mnt/zboot/config-6.1.31-opi-min || true
echo ===ZERO2W_DTB===
find /mnt/zboot/dtb -type f -iname '*zero2w*' -printf '%p %s\n'
echo ===WIFI_MODULES===
find /mnt/zroot/lib/modules/6.1.31-opi-min -type f \( -iname '*brcm*' -o -iname '*cfg80211*' \) -printf '%P %s\n'
echo ===WIFI_FIRMWARE===
find /mnt/zroot/lib/firmware -type f \( -iname '*ap6256*' -o -iname '*43456*' \) -printf '%P %s\n' | head -30
