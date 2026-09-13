#!/usr/bin/env bash
# Create a development image that preloads Wi-Fi and Bluetooth before sysinit.
# This is intentionally separate from the hardware-validated production image.
set -euo pipefail

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_RAW=${1:?usage: $0 PRODUCTION-IMAGE OUTPUT-DEVELOPMENT-IMAGE}
OUTPUT_RAW=${2:?usage: $0 PRODUCTION-IMAGE OUTPUT-DEVELOPMENT-IMAGE}

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

IMAGE_PROFILE=production "$HERE/wsl-15-verify-no-initrd-image.sh" "$SOURCE"

TMP_OUTPUT=$(mktemp "$OUTPUT_DIR/.zero2w-sub10.tmp.XXXXXX")
TMP=$(mktemp -d /tmp/zero2w-sub10.XXXXXX)
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

cat >"$ROOT/etc/systemd/system/zero2w-preload-uwe5622.service" <<'EOF'
[Unit]
Description=Early preload of UWE5622 Wi-Fi stack
DefaultDependencies=no
After=-.mount
Before=sysinit.target shutdown.target
Conflicts=shutdown.target

[Service]
Type=oneshot
ExecStart=/sbin/modprobe sprdwl_ng
RemainAfterExit=yes
TimeoutStartSec=8

[Install]
WantedBy=sysinit.target
EOF

cat >"$ROOT/etc/systemd/system/zero2w-load-bluetooth.service" <<'EOF'
[Unit]
Description=Early load of UWE5622 Bluetooth transport
DefaultDependencies=no
After=zero2w-preload-uwe5622.service
Before=sysinit.target shutdown.target
Conflicts=shutdown.target

[Service]
Type=oneshot
ExecCondition=/bin/sh -c '/sbin/lsmod | grep -q "^sprdwl_ng "'
ExecStart=/sbin/modprobe sprdbt_tty
RemainAfterExit=yes
TimeoutStartSec=8

[Install]
WantedBy=sysinit.target
EOF

mkdir -p "$ROOT/etc/systemd/system/sysinit.target.wants"
ln -sfn ../zero2w-preload-uwe5622.service "$ROOT/etc/systemd/system/sysinit.target.wants/zero2w-preload-uwe5622.service"
ln -sfn ../zero2w-load-bluetooth.service "$ROOT/etc/systemd/system/sysinit.target.wants/zero2w-load-bluetooth.service"
ln -sfn /dev/null "$ROOT/etc/systemd/system/systemd-binfmt.service"
ln -sfn /dev/null "$ROOT/etc/systemd/system/proc-sys-fs-binfmt_misc.automount"
printf '%s\n' \
  'profile=sub10-development-v1' \
  'status=local-structure-validated-hardware-pending' \
  'rollback=zero2w-carplay-debian-no-initrd-production.img' \
  >"$ROOT/etc/zero2w-sub10-development-v1"

sync
umount "$ROOT"
losetup -d "$LOOP"
LOOP=

IMAGE_PROFILE=sub10-development "$HERE/wsl-15-verify-no-initrd-image.sh" "$OUTPUT"
COMPLETE=1
echo "SUB10_DEVELOPMENT_IMAGE_OK image=$OUTPUT"
