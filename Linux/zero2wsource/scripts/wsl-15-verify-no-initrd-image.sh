#!/usr/bin/env bash
# Read-only structural and policy gate for the assembled Zero2W no-initrd image.
# Profiles: forensic (validated vendor boot), production (headless),
# sub10-development (early radio preload) and sub10-ap-development (normal
# access point on wlan0, USB wired SSH preserved). Every profile re-runs the
# whole common gate, so a later stage cannot silently weaken an earlier one.
set -euo pipefail

WIN="${WIN:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}"
IMAGE_INPUT="${1:-$WIN/build-out-image/zero2w-carplay-debian-no-initrd-p1-candidate.img}"
KERNEL_DIR="${KERNEL_DIR:-$WIN/build-out-minimal/kernel}"
BOOT_CMD="${BOOT_CMD:-$WIN/boot/no-initrd/boot.cmd}"
IMAGE_PROFILE="${IMAGE_PROFILE:-forensic}"
KREL=6.1.31-opi-min
ROOT_UUID=3260ecff-7ec8-4312-aff7-ca8f473ec261
case "$IMAGE_PROFILE" in
  forensic|production|sub10-development|sub10-ap-development) ;;
  *) echo "invalid IMAGE_PROFILE: $IMAGE_PROFILE" >&2; exit 2 ;;
esac

IMAGE=$(realpath --canonicalize-existing -- "$IMAGE_INPUT") || { echo "image does not exist" >&2; exit 2; }
case "$IMAGE" in /dev/*|*PhysicalDrive*) echo "refusing physical/block-device path: $IMAGE" >&2; exit 2 ;; esac
[ -f "$IMAGE" ] && [ ! -b "$IMAGE" ] || { echo "not a regular image file: $IMAGE" >&2; exit 2; }
[ -f "$KERNEL_DIR/artifact-manifest.sha256" ] && [ -f "$BOOT_CMD" ] || { echo "missing reviewed artifacts" >&2; exit 2; }
( cd "$KERNEL_DIR" && sha256sum -c artifact-manifest.sha256 >/dev/null )

TMP=$(mktemp -d /tmp/zero2w-image-gate.XXXXXX)
LOOP=""
cleanup() {
  mountpoint -q "$TMP/boot" && umount "$TMP/boot" || true
  mountpoint -q "$TMP/root" && umount "$TMP/root" || true
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
  rm -rf -- "$TMP"
}
trap cleanup EXIT INT TERM
mkdir -p "$TMP/boot" "$TMP/root"
LOOP=$(losetup --read-only --find --show --partscan "$IMAGE")
P1="${LOOP}p1"; P2="${LOOP}p2"
[ "$(lsblk -bndo START "$P1" | xargs)" = 8192 ]
[ "$(lsblk -bndo SIZE "$P1" | xargs)" = 268435456 ]
[ "$(lsblk -bndo START "$P2" | xargs)" = 532480 ]
[ "$(lsblk -bndo SIZE "$P2" | xargs)" = 2613051392 ]
[ "$(blkid -s TYPE -o value "$P1")" = vfat ]
[ "$(blkid -s TYPE -o value "$P2")" = ext4 ]
[ "$(blkid -s UUID -o value "$P2")" = "$ROOT_UUID" ]
ROOT_PARTUUID=$(blkid -s PARTUUID -o value "$P2")
[ -n "$ROOT_PARTUUID" ]
fsck.vfat -n "$P1" >/dev/null
e2fsck -fn "$P2" >/dev/null
mount -o ro "$P1" "$TMP/boot"
mount -o ro,noload "$P2" "$TMP/root"

cmp -s "$TMP/boot/Image" "$KERNEL_DIR/Image"
cmp -s "$TMP/boot/config-$KREL" "$KERNEL_DIR/kernel.config"
cmp -s "$TMP/boot/System.map-$KREL" "$KERNEL_DIR/System.map"
cmp -s "$TMP/boot/dtb/allwinner/sun50i-h618-orangepi-zero2w.dtb" "$KERNEL_DIR/dtb/sun50i-h618-orangepi-zero2w.dtb"
mkimage -l "$TMP/boot/boot.scr" >/dev/null
dumpimage -T script -p 0 -o "$TMP/boot.component" "$TMP/boot/boot.scr" >/dev/null
tail -c +9 "$TMP/boot.component" > "$TMP/decompiled.cmd"
cmp -s "$TMP/decompiled.cmd" "$BOOT_CMD"
cmp -s "$TMP/boot/boot.cmd" "$BOOT_CMD"
! grep -Eq '^[[:space:]]*load[[:space:]].*uInitrd' "$TMP/decompiled.cmd"
grep -Eq '^[[:space:]]*booti[[:space:]]+\$\{kernel_addr_r\}[[:space:]]+-[[:space:]]+\$\{fdt_addr_r\}[[:space:]]*$' "$TMP/decompiled.cmd"
grep -Fqx 'setenv rootdev "/dev/mmcblk0p2"' "$TMP/decompiled.cmd"
grep -Fq 'part uuid mmc ${devnum}:2 rootpartuuid' "$TMP/decompiled.cmd"
grep -Fq 'setenv rootdev "PARTUUID=${rootpartuuid}"' "$TMP/decompiled.cmd"
grep -qx "rootdev=PARTUUID=$ROOT_PARTUUID" "$TMP/boot/orangepiEnv.txt"
! grep -q '^rootdev=UUID=' "$TMP/boot/orangepiEnv.txt"
grep -qx 'rootfstype=ext4' "$TMP/boot/orangepiEnv.txt"
grep -qx 'console=serial' "$TMP/boot/orangepiEnv.txt"
grep -Eq "^UUID=$ROOT_UUID[[:space:]]+/[[:space:]]+ext4" "$TMP/root/etc/fstab"

[ ! -L "$TMP/root/lib/modules" ]
( cd "$TMP/root" && sha256sum -c "$KERNEL_DIR/modules-$KREL.sha256" >/dev/null )
[ "$(find "$TMP/root/lib/modules/$KREL" -type f -name '*.ko' | wc -l)" = 867 ]

CFG="$TMP/boot/config-$KREL"
required_y=(MMC MMC_BLOCK MMC_SUNXI EXT4_FS DEVTMPFS DEVTMPFS_MOUNT I2C I2C_CHARDEV I2C_MV64XXX GPIOLIB GPIO_CDEV GPIO_SYSFS BRCMFMAC BRCMFMAC_SDIO BT BT_BCM BT_HCIUART BT_HCIUART_BCM USB_GADGET USB_CONFIGFS USB_CONFIGFS_F_FS USB_F_FS VIDEO_SUNXI_CEDRUS SND_SUN50IW9_CODEC SUSPEND PM_SLEEP CC_OPTIMIZE_FOR_PERFORMANCE)
for symbol in "${required_y[@]}"; do grep -qx "CONFIG_${symbol}=y" "$CFG" || { echo "missing built-in CONFIG_$symbol" >&2; exit 1; }; done
for symbol in SUNXI_ADDR_MGT WLAN_UWE5622 SPRDWL_NG; do
  grep -qx "CONFIG_${symbol}=m" "$CFG" || { echo "missing required module CONFIG_$symbol" >&2; exit 1; }
done
for symbol in BLK_DEV_INITRD DRM DRM_SUN4I DRM_SUN8I_DW_HDMI DRM_DW_HDMI DRM_PANFROST FB VT SND_SOC_HDMI_CODEC; do
  ! grep -Eq "^CONFIG_${symbol}=[ym]" "$CFG" || { echo "forbidden CONFIG_$symbol enabled" >&2; exit 1; }
done

[ -x "$TMP/root/usr/sbin/i2ctransfer" ]
[ -L "$TMP/root/etc/systemd/system/multi-user.target.wants/ssh.service" ]
grep -Eq '^[[:space:]]*PermitRootLogin[[:space:]]+yes([[:space:]]|$)' "$TMP/root/etc/ssh/sshd_config"
grep -Eq '^[[:space:]]*PasswordAuthentication[[:space:]]+yes([[:space:]]|$)' "$TMP/root/etc/ssh/sshd_config"
root_hash=$(grep '^root:' "$TMP/root/etc/shadow" | cut -d: -f2)
[ -n "$root_hash" ] && [ "$root_hash" != '!' ] && [ "$root_hash" != '*' ]
perl -e 'exit(crypt($ARGV[0], $ARGV[1]) eq $ARGV[1] ? 0 : 1)' root "$root_hash"

WIFI="$TMP/root/etc/NetworkManager/system-connections/default-wifi.nmconnection"
[ "$(stat -c %a "$WIFI")" = 600 ]
grep -qx 'ssid=9A级酒店' "$WIFI"
grep -qx 'psk=hhz211454' "$WIFI"
grep -qx 'autoconnect=true' "$WIFI"
grep -qx 'autoconnect-priority=100' "$WIFI"
if [ "$IMAGE_PROFILE" = sub10-ap-development ]; then
  [ ! -e "$TMP/root/etc/systemd/system/multi-user.target.wants/NetworkManager.service" ]
else
  [ -L "$TMP/root/etc/systemd/system/multi-user.target.wants/NetworkManager.service" ]
fi
DIAG="$TMP/root/usr/local/sbin/zero2w-headless-diagnostics"
LOADER="$TMP/root/usr/local/sbin/zero2w-load-uwe5622"
[ -x "$DIAG" ]
[ -x "$LOADER" ]
[ -L "$TMP/root/etc/systemd/system/multi-user.target.wants/zero2w-load-uwe5622.service" ]
[ -d "$TMP/root/var/log/journal" ]
grep -Rqs 'Zero2W-Diag' "$DIAG" "$TMP/root/etc/NetworkManager" || { echo "diagnostic AP missing" >&2; exit 1; }
grep -Fq 'After=local-fs.target systemd-modules-load.service zero2w-load-uwe5622.service NetworkManager.service' "$TMP/root/etc/systemd/system/zero2w-headless-diagnostics.service"
grep -Fq 'call NMRADIO_WIFI_ON' "$DIAG"
grep -Fq 'call_out NM_FIND_WIFI' "$DIAG"
for file in "$TMP/root/etc/modules" "$TMP/root"/etc/modules-load.d/*.conf; do
  [ -f "$file" ] || continue
  ! grep -Eq '^(uwe5622_bsp_sdio|sprdwl_ng|sprdbt_tty)$' "$file" || { echo "eager UWE5622 module load remains in $file" >&2; exit 1; }
done

if [ "$IMAGE_PROFILE" = forensic ]; then
  [ -L "$TMP/root/etc/systemd/system/multi-user.target.wants/zero2w-headless-diagnostics.service" ]
  grep -Fqx 'BSP=uwe5622_bsp_sdio' "$LOADER"
  grep -Fqx 'WL=sprdwl_ng' "$LOADER"
  ! grep -Eq '^[^#]*modprobe[^#]*sprdbt_tty' "$LOADER"
  grep -Fq 'Before=NetworkManager.service' "$TMP/root/etc/systemd/system/zero2w-load-uwe5622.service"
  grep -Fq 'timeout -k 5 "$limit" modprobe -v "$mod"' "$LOADER"
else
  [ -f "$TMP/root/etc/zero2w-production-boot-v1" ]
  [ "$(readlink "$TMP/root/etc/systemd/system/default.target")" = /lib/systemd/system/multi-user.target ]
  [ "$(readlink "$TMP/root/etc/systemd/system/serial-getty@ttyS0.service")" = /dev/null ]
  [ "$(readlink "$TMP/root/etc/systemd/system/getty@tty1.service")" = /dev/null ]
  [ ! -e "$TMP/root/etc/systemd/system/multi-user.target.wants/zero2w-headless-diagnostics.service" ]
  if [ "$IMAGE_PROFILE" = sub10-ap-development ]; then
    # A normal AP profile must not run a second AP controller behind hostapd's
    # back: the delayed diagnostic timer is unlinked, its unit files stay.
    [ ! -e "$TMP/root/etc/systemd/system/timers.target.wants/zero2w-headless-diagnostics.timer" ]
  else
    [ -L "$TMP/root/etc/systemd/system/timers.target.wants/zero2w-headless-diagnostics.timer" ]
  fi
  if [ "$IMAGE_PROFILE" = sub10-ap-development ]; then
    [ ! -e "$TMP/root/etc/systemd/system/timers.target.wants/zero2w-load-bluetooth.timer" ]
  else
    [ -L "$TMP/root/etc/systemd/system/timers.target.wants/zero2w-load-bluetooth.timer" ]
  fi
  grep -Fqx 'DefaultDependencies=no' "$TMP/root/etc/systemd/system/zero2w-load-uwe5622.service"
  grep -Fqx 'ExecStart=/sbin/modprobe sprdwl_ng' "$TMP/root/etc/systemd/system/zero2w-load-uwe5622.service"
  ! grep -Fq 'Before=NetworkManager.service' "$TMP/root/etc/systemd/system/zero2w-load-uwe5622.service"
  grep -Fqx 'ExecStart=/sbin/modprobe sprdbt_tty' "$TMP/root/etc/systemd/system/zero2w-load-bluetooth.service"
  grep -Fqx 'OnBootSec=20' "$TMP/root/etc/systemd/system/zero2w-load-bluetooth.timer"
  grep -Fqx 'OnBootSec=30' "$TMP/root/etc/systemd/system/zero2w-headless-diagnostics.timer"
  grep -Fqx 'kernel.pid_max = 32768' "$TMP/root/etc/sysctl.d/99-zero2w-headless.conf"
  [ ! -x "$TMP/root/etc/rc.local" ]
  disabled_units=(orangepi-ramlog.service orangepi-zram-config.service bootsplash-hide-when-booted.service bootsplash-show-on-shutdown.service console-setup.service keyboard-setup.service lircd.service lircd-setup.service lircd.socket networking.service dnsmasq.service hostapd.service openvpn.service nfs-client.target rpcbind.service rpcbind.socket rsyslog.service sysstat.service vnstat.service unattended-upgrades.service lm-sensors.service e2scrub_reap.service)
  for unit in "${disabled_units[@]}"; do
    ! find "$TMP/root/etc/systemd/system" -type l -name "$unit" -print -quit | grep -q . || { echo "production-disabled unit remains enabled: $unit" >&2; exit 1; }
  done
  if [ "$IMAGE_PROFILE" = sub10-development ] || [ "$IMAGE_PROFILE" = sub10-ap-development ]; then
    [ -f "$TMP/root/etc/zero2w-sub10-development-v1" ]
    [ -L "$TMP/root/etc/systemd/system/sysinit.target.wants/zero2w-preload-uwe5622.service" ]
    [ -L "$TMP/root/etc/systemd/system/sysinit.target.wants/zero2w-load-bluetooth.service" ]
    [ "$(readlink "$TMP/root/etc/systemd/system/systemd-binfmt.service")" = /dev/null ]
    [ "$(readlink "$TMP/root/etc/systemd/system/proc-sys-fs-binfmt_misc.automount")" = /dev/null ]
    grep -Fqx 'DefaultDependencies=no' "$TMP/root/etc/systemd/system/zero2w-preload-uwe5622.service"
    grep -Fqx 'After=-.mount' "$TMP/root/etc/systemd/system/zero2w-preload-uwe5622.service"
    grep -Fqx 'Before=sysinit.target shutdown.target' "$TMP/root/etc/systemd/system/zero2w-preload-uwe5622.service"
    grep -Fqx 'ExecStart=/sbin/modprobe sprdwl_ng' "$TMP/root/etc/systemd/system/zero2w-preload-uwe5622.service"
    grep -Fqx 'TimeoutStartSec=8' "$TMP/root/etc/systemd/system/zero2w-preload-uwe5622.service"
    grep -Fqx 'After=zero2w-preload-uwe5622.service' "$TMP/root/etc/systemd/system/zero2w-load-bluetooth.service"
    grep -Fq 'ExecCondition=/bin/sh -c' "$TMP/root/etc/systemd/system/zero2w-load-bluetooth.service"
    grep -Fq 'sprdwl_ng' "$TMP/root/etc/systemd/system/zero2w-load-bluetooth.service"
    grep -Fqx 'ExecStart=/sbin/modprobe sprdbt_tty' "$TMP/root/etc/systemd/system/zero2w-load-bluetooth.service"
  fi

  if [ "$IMAGE_PROFILE" = sub10-ap-development ]; then
    # --- Sub-10 AP development profile: normal access point on wlan0 ---
    ap_fail() { echo "sub10-ap-development gate: $*" >&2; exit 1; }
    ap_line() { grep -Fqx -- "$2" "$1" || ap_fail "missing exact line '$2' in ${1#"$TMP/root"}"; }
    ap_has() { grep -Fq -- "$2" "$1" || ap_fail "missing '$2' in ${1#"$TMP/root"}"; }
    ap_no() { ! grep -Eq -- "$2" "$1" || ap_fail "forbidden '$2' in ${1#"$TMP/root"}"; }

    UNITS=$TMP/root/etc/systemd/system
    MARKER=$TMP/root/etc/zero2w-sub10-ap-development-v1
    PRESERVES=$TMP/root/etc/zero2w-sub10-ap-development-v1.preserves
    USB_PROVIDERS=$TMP/root/etc/zero2w-sub10-ap-development-v1.usb-providers
    EXCLUSIONS=$TMP/root/etc/zero2w-sub10-ap-development-v1.exclusions
    HOSTAPD_CONF=$TMP/root/etc/zero2w/ap/hostapd-wlan0.conf
    DNSMASQ_CONF=$TMP/root/etc/zero2w/ap/dnsmasq-wlan0.conf
    NM_WLAN0_CONF=$TMP/root/etc/NetworkManager/conf.d/30-zero2w-ap-wlan0-unmanaged.conf
    AP_ADDRESS_SCRIPT=$TMP/root/usr/local/sbin/zero2w-ap-address
    AP_READY_SCRIPT=$TMP/root/usr/local/sbin/zero2w-ap-ready
    AP_DHCP_READY_SCRIPT=$TMP/root/usr/local/sbin/zero2w-ap-dhcp-ready

    # credentials, channel and address: exactly one authoritative statement
    [ -f "$MARKER" ] || ap_fail "profile marker missing"
    for line in 'profile=sub10-ap-development-v1' \
                'status=local-structure-validated-hardware-pending' \
                'ap_interface=wlan0' 'ap_ssid=Zero2WCar' 'ap_security=wpa2-psk' \
                'ap_channel=7' 'ap_ipv4=192.168.50.1/24' \
                'ap_controller=zero2w-hostapd.service' \
                'dhcp_server=zero2w-ap-dhcp.service' \
                'ready_barrier=zero2w-carplay-ready.target' \
                'diagnostic_ap_timer=disabled-unit-retained' \
                'network_manager=disabled-unit-retained' \
                'bluetooth_timer=disabled-unit-retained' \
                'usb_wired_ssh=installed-reviewed-rndis'; do
      ap_line "$MARKER" "$line"
    done
    ap_no "$MARKER" '(passphrase|psk|password)='

    [ -f "$HOSTAPD_CONF" ] || ap_fail "hostapd config missing"
    [ "$(stat -c %a "$HOSTAPD_CONF")" = 600 ] || ap_fail "hostapd config must be mode 600"
    for line in 'interface=wlan0' 'driver=nl80211' 'ctrl_interface=/run/hostapd' \
                'ctrl_interface_group=0' 'ssid=Zero2WCar' 'hw_mode=g' 'channel=7' \
                'wpa=2' 'wpa_key_mgmt=WPA-PSK' 'wpa_passphrase=12345678' \
                'wpa_pairwise=CCMP' 'rsn_pairwise=CCMP'; do
      ap_line "$HOSTAPD_CONF" "$line"
    done
    for key in interface driver ssid hw_mode channel wpa wpa_key_mgmt wpa_passphrase; do
      [ "$(grep -c "^${key}=" "$HOSTAPD_CONF")" = 1 ] || ap_fail "duplicate '$key' in hostapd config"
    done
    ap_no "$HOSTAPD_CONF" 'usb0|192\.168\.77'

    [ -f "$DNSMASQ_CONF" ] || ap_fail "dnsmasq config missing"
    for line in 'interface=wlan0' 'bind-interfaces' \
                'dhcp-range=192.168.50.10,192.168.50.250,255.255.255.0,12h' \
                'dhcp-option=3,192.168.50.1' 'dhcp-option=6,192.168.50.1'; do
      ap_line "$DNSMASQ_CONF" "$line"
    done
    ap_line "$DNSMASQ_CONF" 'no-resolv'
    ap_no "$DNSMASQ_CONF" 'usb0|192\.168\.77'

    # enablement, ordering and the readiness boundary
    ap_units=(zero2w-ap-address.service zero2w-hostapd.service zero2w-ap-dhcp.service
              zero2w-ap-dhcp-ready.service zero2w-ap-ready.service
              zero2w-carplay-ready.target)
    for unit in "${ap_units[@]}"; do
      [ -f "$UNITS/$unit" ] || ap_fail "unit missing: $unit"
      [ -L "$UNITS/multi-user.target.wants/$unit" ] || ap_fail "unit not enabled: $unit"
      [ "$(readlink "$UNITS/multi-user.target.wants/$unit")" = "../$unit" ] \
        || ap_fail "enablement link of $unit must be a relative ../$unit link"
    done
    ap_line "$UNITS/zero2w-ap-address.service" 'Requires=zero2w-preload-uwe5622.service'
    ap_line "$UNITS/zero2w-ap-address.service" 'After=zero2w-preload-uwe5622.service'
    ap_line "$UNITS/zero2w-ap-address.service" 'ExecStart=/usr/local/sbin/zero2w-ap-address'
    ap_line "$UNITS/zero2w-ap-address.service" 'RemainAfterExit=yes'
    ap_line "$UNITS/zero2w-ap-address.service" 'TimeoutStartSec=30'
    ap_line "$UNITS/zero2w-hostapd.service" 'Requires=zero2w-preload-uwe5622.service zero2w-ap-address.service'
    ap_line "$UNITS/zero2w-hostapd.service" 'After=zero2w-preload-uwe5622.service zero2w-ap-address.service'
    ap_line "$UNITS/zero2w-hostapd.service" 'RuntimeDirectory=hostapd'
    ap_line "$UNITS/zero2w-ap-ready.service" 'Requires=zero2w-hostapd.service'
    ap_line "$UNITS/zero2w-ap-ready.service" 'After=zero2w-hostapd.service'
    ap_line "$UNITS/zero2w-ap-ready.service" 'ExecStart=/usr/local/sbin/zero2w-ap-ready'
    ap_line "$UNITS/zero2w-ap-ready.service" 'RemainAfterExit=yes'
    ap_line "$UNITS/zero2w-ap-ready.service" 'TimeoutStartSec=40'
    # Initialisation-complete includes DHCP availability: the barrier must not
    # be reached while clients can associate but cannot get a lease. It reaches
    # that through the bounded probe, not through the bare "dnsmasq was exec'd"
    # service state; zero2w-ap-dhcp.service still arrives transitively, via the
    # probe's own Requires=, so requiring it here again would only weaken the
    # boundary back to a formality.
    ap_line "$UNITS/zero2w-carplay-ready.target" 'Requires=zero2w-ap-ready.service zero2w-ap-dhcp-ready.service zero2w-load-bluetooth.service bluetooth.service'
    ap_line "$UNITS/zero2w-carplay-ready.target" 'After=zero2w-ap-ready.service zero2w-ap-dhcp-ready.service zero2w-load-bluetooth.service bluetooth.service'
    BT_OVERRIDE=$UNITS/bluetooth.service.d/zero2w-transport.conf
    [ -f "$BT_OVERRIDE" ] && [ "$(stat -c %a "$BT_OVERRIDE")" = 644 ] \
      || ap_fail "BlueZ transport ordering override missing or has wrong mode"
    ap_line "$BT_OVERRIDE" 'Requires=zero2w-load-bluetooth.service'
    ap_line "$BT_OVERRIDE" 'After=zero2w-load-bluetooth.service'
    ap_no "$UNITS/zero2w-carplay-ready.target" '^(Requires|After)=.*zero2w-ap-dhcp\.service'
    # The DHCP probe is a oneshot after the server, bounded like the AP probe.
    ap_line "$UNITS/zero2w-ap-dhcp-ready.service" 'Requires=zero2w-ap-dhcp.service'
    ap_line "$UNITS/zero2w-ap-dhcp-ready.service" 'After=zero2w-ap-dhcp.service'
    ap_line "$UNITS/zero2w-ap-dhcp-ready.service" 'ExecStart=/usr/local/sbin/zero2w-ap-dhcp-ready'
    ap_line "$UNITS/zero2w-ap-dhcp-ready.service" 'RemainAfterExit=yes'
    ap_line "$UNITS/zero2w-ap-dhcp-ready.service" 'TimeoutStartSec=40'
    ap_no "$UNITS/zero2w-ap-dhcp-ready.service" '^(After|Requires|Wants|BindsTo|PartOf)=.*hostapd'
    # A restarting daemon with no start limit can spin across a whole boot:
    # both AP daemons are bounded identically, or the barrier can be reached on
    # a unit that is still thrashing.
    for unit in zero2w-hostapd.service zero2w-ap-dhcp.service; do
      ap_line "$UNITS/$unit" 'StartLimitIntervalSec=30'
      ap_line "$UNITS/$unit" 'StartLimitBurst=3'
    done
    # DHCP is bound to the AP interface only and must never wait for the AP to
    # come up, otherwise the interface address would be the only dependency.
    ap_line "$UNITS/zero2w-ap-dhcp.service" 'Requires=zero2w-ap-address.service'
    ap_line "$UNITS/zero2w-ap-dhcp.service" 'After=zero2w-ap-address.service'
    ap_no "$UNITS/zero2w-ap-dhcp.service" '^(After|Requires|Wants|BindsTo|PartOf)=.*hostapd'
    ap_no "$UNITS/zero2w-ap-dhcp.service" '^(After|Requires|Wants)=.*(NetworkManager|networking\.service|wpa_supplicant)'
    for unit in "${ap_units[@]}"; do
      ap_no "$UNITS/$unit" '^(After|Requires|Wants|BindsTo|Upstreams)=.*(NetworkManager|networking\.service|wpa_supplicant|systemd-networkd)'
    done

    # ExecStart must name a real executable of the reviewed daemon, and it must
    # load the dedicated config, never the vendor hostapd/dnsmasq configuration.
    for pair in 'zero2w-hostapd.service:hostapd' 'zero2w-ap-dhcp.service:dnsmasq'; do
      unit=${pair%%:*}
      want=${pair##*:}
      daemon=$(sed -n 's/^ExecStart=\([^ ]*\).*/\1/p' "$UNITS/$unit" | head -1)
      [ -n "$daemon" ] || ap_fail "no ExecStart binary in $unit"
      [ -f "$TMP/root$daemon" ] && [ -x "$TMP/root$daemon" ] \
        || ap_fail "ExecStart $daemon of $unit is not an executable in the image"
      [ "$(basename "$daemon")" = "$want" ] || ap_fail "$unit must run $want, found $daemon"
    done
    ap_has "$UNITS/zero2w-hostapd.service" '/etc/zero2w/ap/hostapd-wlan0.conf'
    ap_no "$UNITS/zero2w-hostapd.service" '/etc/hostapd/'
    ap_has "$UNITS/zero2w-ap-dhcp.service" '--conf-file=/etc/zero2w/ap/dnsmasq-wlan0.conf'
    ap_no "$UNITS/zero2w-ap-dhcp.service" '/etc/dnsmasq\.conf|/etc/dnsmasq\.d/'

    # the three helper scripts do the bounded waiting and probing
    for script in "$AP_ADDRESS_SCRIPT" "$AP_READY_SCRIPT" "$AP_DHCP_READY_SCRIPT"; do
      [ -x "$script" ] || ap_fail "missing helper $script"
      [ "$(stat -c %a "$script")" = 755 ] || ap_fail "helper $script must be mode 755"
      ap_no "$script" 'usb0|192\.168\.77'
    done
    ap_line "$AP_ADDRESS_SCRIPT" 'IFACE=wlan0'
    ap_line "$AP_ADDRESS_SCRIPT" 'ADDRESS=192.168.50.1/24'
    ap_line "$AP_ADDRESS_SCRIPT" 'WAIT_STEPS=100'
    ap_line "$AP_ADDRESS_SCRIPT" 'WAIT_STEP_S=0.2'
    # Bounded retry semantics: the netdev existence check, the link up, the
    # address replace and the exact-address verification all have to live
    # inside the one WAIT_STEPS loop, so an interface that is still settling is
    # retried instead of latching a permanent failure onto the units that
    # require this one.
    awk '/^for[[:space:]].*WAIT_STEPS/ { inside = 1; next } inside && /^done/ { inside = 0 } inside' \
      "$AP_ADDRESS_SCRIPT" >"$TMP/ap-address-loop"
    [ -s "$TMP/ap-address-loop" ] \
      || ap_fail "zero2w-ap-address has no bounded WAIT_STEPS retry loop"
    for needle in '/sys/class/net/' 'ip link set' 'ip -4 addr replace' 'grep -qF "$ADDRESS"' 'sleep "$WAIT_STEP_S"'; do
      grep -Fq -- "$needle" "$TMP/ap-address-loop" \
        || ap_fail "zero2w-ap-address does not retry '$needle' inside the bounded loop"
    done
    ! grep -Fq 'exit 1' "$TMP/ap-address-loop" \
      || ap_fail "zero2w-ap-address fails inside the retry loop; only an expired limit may fail it"
    awk '/^done/ { after = 1 } after && /^[[:space:]]*exit 1$/ { found = 1 } END { exit !found }' \
      "$AP_ADDRESS_SCRIPT" \
      || ap_fail "zero2w-ap-address has no failure exit after the retry limit expires"
    ap_line "$AP_READY_SCRIPT" 'IFACE=wlan0'
    ap_line "$AP_READY_SCRIPT" 'TRIES=40'
    ap_has "$AP_READY_SCRIPT" 'hostapd_cli'
    ap_has "$AP_READY_SCRIPT" 'ENABLED'
    ap_has "$AP_READY_SCRIPT" 'type AP'

    # DHCP readiness: the probe confirms the server this profile started, not
    # the unit state. Both the process bound to the dedicated config and the
    # socket a DHCP DISCOVER lands on have to be observed inside one bounded
    # loop, or zero2w-ap-dhcp-ready.service would be the formality it replaces.
    ap_line "$AP_DHCP_READY_SCRIPT" 'IFACE=wlan0'
    ap_line "$AP_DHCP_READY_SCRIPT" 'UDP_PORT=67'
    ap_line "$AP_DHCP_READY_SCRIPT" 'TRIES=40'
    ap_line "$AP_DHCP_READY_SCRIPT" 'DNSMASQ_CONF=/etc/zero2w/ap/dnsmasq-wlan0.conf'
    ap_has "$AP_DHCP_READY_SCRIPT" 'pgrep -f'
    ap_has "$AP_DHCP_READY_SCRIPT" '-k --conf-file=$DNSMASQ_CONF'
    ap_has "$AP_DHCP_READY_SCRIPT" '"$SOCK_PROBE" -H -lunp "sport = :$UDP_PORT"'
    ap_has "$AP_DHCP_READY_SCRIPT" 'grep -Fq "pid=$pid,"'
    awk '/^for[[:space:]].*TRIES/ { inside = 1; next } inside && /^done/ { inside = 0 } inside' \
      "$AP_DHCP_READY_SCRIPT" >"$TMP/ap-dhcp-loop"
    [ -s "$TMP/ap-dhcp-loop" ] \
      || ap_fail "zero2w-ap-dhcp-ready has no bounded TRIES probe loop"
    for needle in 'probe && break' 'sleep "$TRIAL_S"'; do
      grep -Fq -- "$needle" "$TMP/ap-dhcp-loop" \
        || ap_fail "zero2w-ap-dhcp-ready does not retry '$needle' inside the bounded loop"
    done
    # The two tools the probe embeds are the ones the builder discovered in this
    # image, by absolute path: never a PATH lookup at boot, and never a tool
    # that is not actually in the rootfs the gate is reading.
    daemon_bin=$(sed -n 's/^DNSMASQ_BIN=\([^ ]*\).*/\1/p' "$AP_DHCP_READY_SCRIPT" | head -1)
    socket_bin=$(sed -n 's/^SOCK_PROBE=\([^ ]*\).*/\1/p' "$AP_DHCP_READY_SCRIPT" | head -1)
    [ -n "$daemon_bin" ] || ap_fail "the DHCP probe does not record DNSMASQ_BIN"
    [ -n "$socket_bin" ] || ap_fail "the DHCP probe does not record SOCK_PROBE"
    [ "$(basename "$daemon_bin")" = dnsmasq ] \
      || ap_fail "the DHCP probe must name dnsmasq, found $daemon_bin"
    [ "$(basename "$socket_bin")" = ss ] \
      || ap_fail "SOCK_PROBE must be ss with process ownership output, found $socket_bin"
    for probe_path in "$daemon_bin" "$socket_bin"; do
      case "$probe_path" in
        /*) ;;
        *) ap_fail "the DHCP probe must embed an absolute path: $probe_path" ;;
      esac
      [ -f "$TMP/root$probe_path" ] && [ -x "$TMP/root$probe_path" ] \
        || ap_fail "the DHCP probe names $probe_path, which is not an executable in the image"
    done

    # no second manager for wlan0
    [ -f "$NM_WLAN0_CONF" ] || ap_fail "NetworkManager unmanaged rule missing"
    [ "$(stat -c %a "$NM_WLAN0_CONF")" = 644 ] || ap_fail "unmanaged rule must be mode 644"
    ap_line "$NM_WLAN0_CONF" 'unmanaged-devices=interface-name:wlan0'
    ap_no "$NM_WLAN0_CONF" 'usb0|192\.168\.77'
    [ ! -e "$UNITS/multi-user.target.wants/wpa_supplicant.service" ]
    [ ! -e "$UNITS/multi-user.target.wants/NetworkManager.service" ] \
      || ap_fail 'NetworkManager is enabled and would reclaim wlan0'
    [ ! -e "$UNITS/timers.target.wants/zero2w-load-bluetooth.timer" ] \
      || ap_fail 'delayed Bluetooth timer is enabled and would create an ordering cycle'
    ! find "$UNITS" -type l -name 'wpa_supplicant*' -print -quit | grep -q . \
      || ap_fail 'wpa_supplicant is enabled and would contend for wlan0'
    if find "$UNITS" -type l -name 'systemd-networkd.service' -print -quit | grep -q .; then
      for network in "$TMP/root"/etc/systemd/network/*.network; do
        [ -f "$network" ] || continue
        grep -Eq '^[[:space:]]*Name=[[:space:]]*(wlan0|wlan\*|\*)[[:space:]]*$' "$network" \
          && ap_fail "systemd-networkd would claim wlan0: ${network#"$TMP/root"}"
      done
    fi

    # the diagnostic AP stays available but unarmed; its enablement link is the
    # single inherited path exempted from the fingerprint set, verified below
    [ ! -e "$UNITS/timers.target.wants/zero2w-headless-diagnostics.timer" ]
    # The exemption below covers exactly one link, so no other enablement of the
    # same timer may survive: it would arm a second AP controller on wlan0.
    ! find "$UNITS" -type l -name 'zero2w-headless-diagnostics.timer' -print -quit | grep -q . \
      || ap_fail 'the diagnostic AP timer is still enabled through another link'
    [ -f "$UNITS/zero2w-headless-diagnostics.timer" ] || ap_fail "diagnostic timer unit file must be retained"
    [ -f "$UNITS/zero2w-headless-diagnostics.service" ] || ap_fail "diagnostic service unit file must be retained"

    # Inherited SSH and collected configuration paths remain byte/link exact.
    # The source image has no USB debug network; this profile installs the
    # reviewed RNDIS implementation below and verifies it independently.
    [ -f "$PRESERVES" ] || ap_fail "preservation manifest missing"
    preserved_count=0
    while IFS=$'\t' read -r kind token rel; do
      case "$kind" in
        F)
          [ -f "$TMP/root/$rel" ] || ap_fail "preserved provisioning file is gone: $rel"
          [ "$(sha256sum "$TMP/root/$rel" | cut -d' ' -f1)" = "$token" ] \
            || ap_fail "preserved provisioning file changed: $rel"
          ;;
        L)
          [ -L "$TMP/root/$rel" ] || ap_fail "preserved provisioning link is gone: $rel"
          [ "$(readlink "$TMP/root/$rel")" = "$token" ] \
            || ap_fail "preserved provisioning link rewired: $rel -> $(readlink "$TMP/root/$rel") (was $token)"
          ;;
        *) ap_fail "unknown preservation record kind: $kind" ;;
      esac
      preserved_count=$((preserved_count + 1))
    done < "$PRESERVES"
    [ "$preserved_count" -ge 10 ] || ap_fail "preservation manifest too small: $preserved_count entries"
    awk -F'\t' '$3 == "etc/ssh/sshd_config" { found = 1 } END { exit !found }' "$PRESERVES" \
      || ap_fail "preservation manifest does not cover sshd_config"

    [ -s "$USB_PROVIDERS" ] || ap_fail "reviewed USB provisioning path list missing"
    cat >"$TMP/expected-usb-providers" <<'USB_PATHS'
etc/NetworkManager/conf.d/90-zero2w-usb-gadget-unmanaged.conf
etc/systemd/system/multi-user.target.wants/zero2w-usb-dhcp.service
etc/systemd/system/multi-user.target.wants/zero2w-usb-rndis.service
etc/systemd/system/zero2w-usb-dhcp.service
etc/systemd/system/zero2w-usb-rndis.service
usr/local/sbin/zero2w-usb-rndis
USB_PATHS
    cmp -s "$USB_PROVIDERS" "$TMP/expected-usb-providers" \
      || ap_fail "USB provisioning path list differs from the reviewed set"
    while IFS= read -r provider; do
      [ -n "$provider" ] || continue
      if [ ! -e "$TMP/root/$provider" ] && [ ! -L "$TMP/root/$provider" ]; then
        ap_fail "reviewed USB provisioning path is missing: $provider"
      fi
      ! awk -F'\t' -v want="$provider" '$3 == want { found = 1 } END { exit !found }' "$PRESERVES" \
        || ap_fail "new USB provider was incorrectly claimed as inherited: $provider"
    done <"$USB_PROVIDERS"

    USB_RNDIS=$TMP/root/usr/local/sbin/zero2w-usb-rndis
    USB_RNDIS_UNIT=$UNITS/zero2w-usb-rndis.service
    USB_DHCP_UNIT=$UNITS/zero2w-usb-dhcp.service
    USB_NM=$TMP/root/etc/NetworkManager/conf.d/90-zero2w-usb-gadget-unmanaged.conf
    [ -x "$USB_RNDIS" ] && [ "$(stat -c %a "$USB_RNDIS")" = 755 ] \
      || ap_fail "USB RNDIS helper missing or has wrong mode"
    for line in 'G=/sys/kernel/config/usb_gadget/zero2w-rndis' \
                "    printf '02:00:00:00:77:02' > functions/rndis.usb0/dev_addr" \
                "    printf '02:00:00:00:77:01' > functions/rndis.usb0/host_addr" \
                '    ip address replace 192.168.77.2/24 dev usb0' \
                '    ip link set usb0 up'; do
      ap_line "$USB_RNDIS" "$line"
    done
    ap_no "$USB_RNDIS" 'wlan0|192\.168\.50'
    [ "$(stat -c %a "$USB_RNDIS_UNIT")" = 644 ] || ap_fail "USB RNDIS unit must be mode 644"
    [ "$(stat -c %a "$USB_DHCP_UNIT")" = 644 ] || ap_fail "USB DHCP unit must be mode 644"
    ap_line "$USB_RNDIS_UNIT" 'After=sys-kernel-config.mount NetworkManager.service'
    ap_line "$USB_RNDIS_UNIT" 'Before=zero2w-usb-dhcp.service'
    ap_line "$USB_RNDIS_UNIT" 'ExecStart=/usr/local/sbin/zero2w-usb-rndis up'
    ap_line "$USB_DHCP_UNIT" 'Requires=zero2w-usb-rndis.service'
    ap_line "$USB_DHCP_UNIT" 'After=zero2w-usb-rndis.service'
    ap_has "$USB_DHCP_UNIT" '--interface=usb0 --bind-dynamic'
    ap_has "$USB_DHCP_UNIT" '--dhcp-range=192.168.77.10,192.168.77.20,255.255.255.0,1h'
    ap_no "$USB_DHCP_UNIT" 'wlan0|192\.168\.50'
    [ "$(stat -c %a "$USB_NM")" = 644 ] || ap_fail "USB NetworkManager rule must be mode 644"
    ap_line "$USB_NM" 'unmanaged-devices=interface-name:usb0'
    for unit in zero2w-usb-rndis.service zero2w-usb-dhcp.service; do
      [ -L "$UNITS/multi-user.target.wants/$unit" ] || ap_fail "USB unit not enabled: $unit"
      [ "$(readlink "$UNITS/multi-user.target.wants/$unit")" = "../$unit" ] \
        || ap_fail "USB unit enablement is not the reviewed relative link: $unit"
    done
    # Exactly four inherited enablement links are intentionally absent: the
    # diagnostic AP, WPA/NetworkManager auto-start, and the obsolete delayed
    # Bluetooth timer. Their unit files and recovery configuration remain.
    [ -f "$EXCLUSIONS" ] || ap_fail "intentional exclusion list missing"
    cat >"$TMP/expected-exclusions" <<'EXCLUSION_PATHS'
etc/systemd/system/timers.target.wants/zero2w-headless-diagnostics.timer
etc/systemd/system/multi-user.target.wants/wpa_supplicant.service
etc/systemd/system/multi-user.target.wants/NetworkManager.service
etc/systemd/system/timers.target.wants/zero2w-load-bluetooth.timer
EXCLUSION_PATHS
    cmp -s "$EXCLUSIONS" "$TMP/expected-exclusions" \
      || ap_fail "intentional exclusion list differs from the four reviewed paths"
    while IFS= read -r rel; do
      [ -n "$rel" ] || continue
      case "$rel" in
        etc/systemd/system/timers.target.wants/zero2w-headless-diagnostics.timer|etc/systemd/system/multi-user.target.wants/wpa_supplicant.service|etc/systemd/system/multi-user.target.wants/NetworkManager.service|etc/systemd/system/timers.target.wants/zero2w-load-bluetooth.timer) ;;
        *) ap_fail "unapproved intentional exclusion entry: $rel" ;;
      esac
      # "absent" means neither a file nor a dangling link: the exemption exists
      # precisely because this profile removes the enablement link.
      if [ -e "$TMP/root/$rel" ] || [ -L "$TMP/root/$rel" ]; then
        ap_fail "exempted inherited path is still present: $rel"
      fi
      if grep -qxF -- "$rel" "$USB_PROVIDERS"; then
        ap_fail "an exempted path provides usb0 provisioning: $rel"
      fi
      if awk -F'\t' -v want="$rel" '$3 == want { found = 1 } END { exit !found }' "$PRESERVES"; then
        ap_fail "an exempted path is still fingerprinted as preserved: $rel"
      fi
    done <"$EXCLUSIONS"
    [ -L "$UNITS/multi-user.target.wants/ssh.service" ] \
      || ap_fail 'ssh.service enablement link vanished'

    # nothing outside the zero2w AP unit family may appear in /etc/systemd/system
    awk -F'\t' '$1 == "L" { print $3 }' "$PRESERVES" >"$TMP/preserved-links"
    while IFS= read -r link; do
      rel=etc/systemd/system/${link#"$UNITS/"}
      case "$rel" in */zero2w-*) continue ;; esac
      grep -qxF "$rel" "$TMP/preserved-links" \
        || ap_fail "unexpected new systemd link: $rel"
    done < <(find "$UNITS" -type l)
  fi
fi
grep -aFq 'loopcheck state not ready' "$TMP/root/lib/modules/$KREL/kernel/drivers/net/wireless/uwe5622/unisocwcn/uwe5622_bsp_sdio.ko"
grep -aFq 'register platform device unisoc wifi failed: %d' "$TMP/root/lib/modules/$KREL/kernel/drivers/net/wireless/uwe5622/unisocwifi/sprdwl_ng.ko"
grep -aFq 'addr named %s is not initialized' "$TMP/root/lib/modules/$KREL/kernel/drivers/misc/sunxi-addr/sunxi_addr.ko"
dep_line=$(grep '^kernel/drivers/net/wireless/uwe5622/unisocwifi/sprdwl_ng\.ko:' "$TMP/root/lib/modules/$KREL/modules.dep")
[[ "$dep_line" == *sunxi-addr/sunxi_addr.ko* && "$dep_line" == *uwe5622_bsp_sdio.ko* ]] || { echo "sprdwl_ng dependencies incomplete" >&2; exit 1; }
[ -f "$TMP/root/lib/firmware/brcm/brcmfmac43456-sdio.bin" ]
[ -f "$TMP/root/lib/firmware/brcm/brcmfmac43456-sdio.txt" ]

hash=$(sha256sum "$IMAGE" | awk '{print $1}')
echo "NO_INITRD_IMAGE_VERIFY_OK profile=$IMAGE_PROFILE"
echo "image=$IMAGE"
echo "size=$(stat -c %s "$IMAGE")"
echo "sha256=$hash"
echo "root_uuid=$ROOT_UUID root_partuuid=$ROOT_PARTUUID kernel=$KREL modules=867"
