#!/bin/sh
# Install the real-CarPlay sidecar on an Orange Pi Zero 2W (Debian 12, arm64).
#
# PLAN ONLY by default: it prints every change and touches nothing. Pass
# --apply to perform it. Even with --apply this script never enables or starts
# the unit, never switches wlan0 and never runs hostapd; that stays an explicit
# operator step so the USB RNDIS debug link is never lost by accident.
#
#   ./install.sh /path/to/cp-native            # plan
#   ./install.sh /path/to/cp-native --apply    # install
set -eu

PREFIX=/root/zero2w
ENV_PATH=/etc/zero2w/cp-native.env
UNIT_PATH=/etc/systemd/system/zero2w-cp-native.service
PACKAGES="hostapd iw dnsmasq rfkill avahi-utils bluez alsa-utils i2c-tools"

HERE=$(cd "$(dirname "$0")" && pwd)
APPLY=0
SRC=""
for arg in "$@"; do
  case "$arg" in
    --apply) APPLY=1 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) SRC=$arg ;;
  esac
done
if [ -z "$SRC" ]; then
  SRC="$HERE/../target/aarch64-unknown-linux-musl/release/cp-native"
fi

run() {
  if [ "$APPLY" -eq 1 ]; then
    "$@"
  else
    printf 'plan: %s\n' "$*"
  fi
}

echo "== source =="
if [ ! -f "$SRC" ]; then
  echo "MISSING binary: $SRC"
  echo "build it first: Input/WirelessCarPlay/Engine/build/build.sh arm64"
  exit 1
fi
ls -l "$SRC"
# Never install a host/x86 or non-ELF file onto the board. `readelf` comes from
# the base binutils package on Debian; use its machine field instead of trusting
# a filename or the current build host.
if ! command -v readelf >/dev/null 2>&1; then
  echo "MISSING required install guard: readelf (binutils)" >&2
  exit 1
fi
if ! readelf -h "$SRC" 2>/dev/null | grep -Eq 'Class:[[:space:]]+ELF64' \
  || ! readelf -h "$SRC" 2>/dev/null | grep -Eq 'Machine:[[:space:]]+AArch64'; then
  echo "REFUSED: $SRC is not an ELF64 AArch64 Linux artifact" >&2
  exit 1
fi

echo "== packages =="
missing=""
for pkg in $PACKAGES; do
  if dpkg -s "$pkg" >/dev/null 2>&1; then
    echo "present: $pkg"
  else
    echo "MISSING: $pkg"
    missing="$missing $pkg"
  fi
done
if [ -n "$missing" ]; then
  # Package indexes may be stale on an image that has never been updated.
  run apt-get update
  run apt-get install -y $missing
fi

echo "== files =="
run mkdir -p "$PREFIX/bin"
run install -m 755 "$SRC" "$PREFIX/bin/cp-native"
run mkdir -p /etc/zero2w
if [ -f "$ENV_PATH" ]; then
  echo "kept: $ENV_PATH already exists (not overwritten)"
else
  run install -m 600 "$HERE/zero2w-cp-native.env" "$ENV_PATH"
fi
run install -m 644 "$HERE/zero2w-cp-native.service" "$UNIT_PATH"
run systemctl daemon-reload

echo "== preflight =="
if [ "$APPLY" -eq 1 ]; then
  "$PREFIX/bin/cp-native" preflight --env-file "$ENV_PATH" || echo "preflight reported blocking findings"
else
  printf 'plan: %s preflight --env-file %s\n' "$PREFIX/bin/cp-native" "$ENV_PATH"
fi

cat <<EOF

== next steps (operator, deliberate) ==
1. Set CP_COUNTRY in $ENV_PATH; an empty domain cannot host an AP.
2. Review the plan:      $PREFIX/bin/cp-native ap-plan --env-file $ENV_PATH
3. Check the debug link: systemctl is-active zero2w-usb-rndis.service zero2w-usb-dhcp.service
4. Verify MFi safety:    $PREFIX/bin/cp-native mfi-check --env-file $ENV_PATH
5. Only then start it:   systemctl enable --now zero2w-cp-native.service
   SSH stays on 192.168.77.2 over USB; wlan0's client address is released.
EOF
