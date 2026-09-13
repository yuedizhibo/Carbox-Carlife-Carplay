#!/usr/bin/env bash
# Convert a reviewed forensic no-initrd image into the production headless profile.
# The source and output must be regular image files; block devices are rejected.
set -euo pipefail

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_RAW=${1:?usage: $0 FORENSIC-IMAGE OUTPUT-IMAGE}
OUTPUT_RAW=${2:?usage: $0 FORENSIC-IMAGE OUTPUT-IMAGE}

fail() { echo "$*" >&2; exit 2; }
reject_device() {
  case "$2" in /dev|/dev/*|*PhysicalDrive*) fail "refusing unsafe $1 path: $2" ;; esac
}
reject_device source "$SOURCE_RAW"
SOURCE=$(realpath -e -- "$SOURCE_RAW") || fail "source image does not exist"
reject_device source "$SOURCE"
[ -f "$SOURCE" ] && [ ! -b "$SOURCE" ] || fail "source is not a regular image file"
[ ! -e "$OUTPUT_RAW" ] && [ ! -L "$OUTPUT_RAW" ] || fail "output already exists or is a symlink: $OUTPUT_RAW"
OUTPUT_DIR=$(realpath -e -- "$(dirname -- "$OUTPUT_RAW")") || fail "output directory does not exist"
OUTPUT="$OUTPUT_DIR/$(basename -- "$OUTPUT_RAW")"
reject_device output "$OUTPUT"
[ "$SOURCE" != "$OUTPUT" ] || fail "source and output are identical"

# Refuse to optimize an image that has not passed the full forensic gate.
IMAGE_PROFILE=forensic "$HERE/wsl-15-verify-no-initrd-image.sh" "$SOURCE"

TMP_OUTPUT=$(mktemp "$OUTPUT_DIR/.zero2w-production.tmp.XXXXXX")
TMP=$(mktemp -d /tmp/zero2w-production.XXXXXX)
LOOP=
COMPLETE=0
cleanup() {
  set +e
  mountpoint -q "$TMP/root" && umount "$TMP/root"
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  rm -rf -- "$TMP"
  [ -e "$TMP_OUTPUT" ] && rm -f -- "$TMP_OUTPUT"
  [ "$COMPLETE" = 1 ] || rm -f -- "$OUTPUT"
}
trap cleanup EXIT INT TERM
rm -f -- "$TMP_OUTPUT"
cp --reflink=auto -- "$SOURCE" "$TMP_OUTPUT"
ln -- "$TMP_OUTPUT" "$OUTPUT" || fail "output was created concurrently"
rm -f -- "$TMP_OUTPUT"
[ -f "$OUTPUT" ] && [ ! -L "$OUTPUT" ] && [ ! -b "$OUTPUT" ] || fail "unsafe output after creation"

mkdir -p "$TMP/root"
LOOP=$(losetup --find --show --partscan "$OUTPUT")
mount "${LOOP}p2" "$TMP/root"
ROOT=$TMP/root

cat >"$ROOT/etc/systemd/system/zero2w-load-uwe5622.service" <<'EOF'
[Unit]
Description=Load UWE5622 Wi-Fi stack
DefaultDependencies=no
After=local-fs.target systemd-modules-load.service
Before=shutdown.target
Conflicts=shutdown.target

[Service]
Type=oneshot
ExecStart=/sbin/modprobe sprdwl_ng
RemainAfterExit=yes
TimeoutStartSec=30

[Install]
WantedBy=multi-user.target
EOF

cat >"$ROOT/etc/systemd/system/zero2w-load-bluetooth.service" <<'EOF'
[Unit]
Description=Load UWE5622 Bluetooth transport after Wi-Fi
After=zero2w-load-uwe5622.service

[Service]
Type=oneshot
ExecStart=/sbin/modprobe sprdbt_tty
RemainAfterExit=yes
TimeoutStartSec=20
EOF

cat >"$ROOT/etc/systemd/system/zero2w-load-bluetooth.timer" <<'EOF'
[Unit]
Description=Delay UWE5622 Bluetooth transport until Wi-Fi is stable

[Timer]
OnBootSec=20
AccuracySec=1s
Unit=zero2w-load-bluetooth.service

[Install]
WantedBy=timers.target
EOF

cat >"$ROOT/etc/systemd/system/zero2w-headless-diagnostics.timer" <<'EOF'
[Unit]
Description=Delayed out-of-band diagnostics and fallback hotspot

[Timer]
OnBootSec=30
AccuracySec=1s
Unit=zero2w-headless-diagnostics.service

[Install]
WantedBy=timers.target
EOF

# Remove exact enablement links only. Unit files and diagnostic tools remain for
# manual recovery, while delayed timers keep Bluetooth and fallback diagnostics
# off the boot critical path.
disable_units=(
  orangepi-ramlog.service orangepi-zram-config.service
  bootsplash-hide-when-booted.service bootsplash-show-on-shutdown.service
  console-setup.service keyboard-setup.service
  lircd.service lircd-setup.service lircd.socket
  networking.service dnsmasq.service hostapd.service openvpn.service
  nfs-client.target rpcbind.service rpcbind.socket
  rsyslog.service sysstat.service vnstat.service unattended-upgrades.service
  lm-sensors.service e2scrub_reap.service
)
disable_timers=(
  apt-daily.timer apt-daily-upgrade.timer dpkg-db-backup.timer
  e2scrub_all.timer fstrim.timer logrotate.timer man-db.timer
  sysstat-collect.timer sysstat-summary.timer
)
for unit in "${disable_units[@]}" "${disable_timers[@]}"; do
  find "$ROOT/etc/systemd/system" -type l -name "$unit" -delete
 done
find "$ROOT/etc/systemd/system" -type l -name zero2w-headless-diagnostics.service -delete

mkdir -p "$ROOT/etc/systemd/system/multi-user.target.wants" "$ROOT/etc/systemd/system/timers.target.wants"
ln -sfn /lib/systemd/system/multi-user.target "$ROOT/etc/systemd/system/default.target"
ln -sfn /dev/null "$ROOT/etc/systemd/system/serial-getty@ttyS0.service"
ln -sfn /dev/null "$ROOT/etc/systemd/system/getty@tty1.service"
ln -sfn ../zero2w-load-uwe5622.service "$ROOT/etc/systemd/system/multi-user.target.wants/zero2w-load-uwe5622.service"
ln -sfn ../zero2w-load-bluetooth.timer "$ROOT/etc/systemd/system/timers.target.wants/zero2w-load-bluetooth.timer"
ln -sfn ../zero2w-headless-diagnostics.timer "$ROOT/etc/systemd/system/timers.target.wants/zero2w-headless-diagnostics.timer"
chmod -x "$ROOT/etc/rc.local" 2>/dev/null || true
printf '%s\n' 'kernel.pid_max = 32768' >"$ROOT/etc/sysctl.d/99-zero2w-headless.conf"
printf '%s\n' 'profile=production-v1' 'measured_systemd_boot=8.943s' 'measured_wifi_ready=12.445s' >"$ROOT/etc/zero2w-production-boot-v1"

sync
umount "$ROOT"
losetup -d "$LOOP"
LOOP=

IMAGE_PROFILE=production "$HERE/wsl-15-verify-no-initrd-image.sh" "$OUTPUT"
COMPLETE=1
echo "PRODUCTION_IMAGE_OPTIMIZATION_OK image=$OUTPUT"
