#!/usr/bin/env bash
set -euo pipefail
IMG="${1:-/home/dfkvm/opi/images/extracted/Orangepizero2w_1.0.4_debian_bookworm_server_linux6.1.31/Orangepizero2w_1.0.4_debian_bookworm_server_linux6.1.31.img}"
LOOP="$(losetup --find --show --partscan "$IMG")"
ROOT=/mnt/zero2w-root
cleanup() {
    mountpoint -q "$ROOT" && umount "$ROOT" || true
    losetup -d "$LOOP" 2>/dev/null || true
}
trap cleanup EXIT
mkdir -p "$ROOT"
mount "${LOOP}p2" "$ROOT"
install -d -m 700 "$ROOT/etc/NetworkManager/system-connections"
cat >"$ROOT/etc/NetworkManager/system-connections/default-wifi.nmconnection" <<'EOF'
[connection]
id=default-wifi
uuid=efbb1ba1-67c8-4e3c-a9a0-cf941cb4be4f
type=wifi
interface-name=wlan0
autoconnect=true
autoconnect-priority=100

[wifi]
mode=infrastructure
ssid=9A级酒店

[wifi-security]
key-mgmt=wpa-psk
psk=hhz211454

[ipv4]
method=auto

[ipv6]
method=auto
addr-gen-mode=default

[proxy]
EOF
chmod 600 "$ROOT/etc/NetworkManager/system-connections/default-wifi.nmconnection"
chown root:root "$ROOT/etc/NetworkManager/system-connections/default-wifi.nmconnection"
install -d -m 755 "$ROOT/etc/NetworkManager/conf.d"
printf '[connection]\nwifi.powersave=2\n' >"$ROOT/etc/NetworkManager/conf.d/20-wifi-powersave.conf"
chmod 644 "$ROOT/etc/NetworkManager/conf.d/20-wifi-powersave.conf"
grep -v '^psk=' "$ROOT/etc/NetworkManager/system-connections/default-wifi.nmconnection"
sync
