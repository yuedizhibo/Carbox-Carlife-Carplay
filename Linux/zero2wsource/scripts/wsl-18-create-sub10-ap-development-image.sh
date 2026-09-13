#!/usr/bin/env bash
# Sub-10 AP development image (V2): turn the validated sub10-development image
# into a *normal* access point, so the board stops depending on a station
# network that may not exist.
#
# Reviewed parameters for this profile (fixed, not tunables):
#   SSID        Zero2WCar
#   security    WPA2-PSK, passphrase 12345678
#   channel     7 (2.4 GHz)
#   wlan0       192.168.50.1/24, static
#
# Design notes:
# * hostapd and dnsmasq are driven by dedicated units and a dedicated config
#   file each. NetworkManager is told to leave wlan0 unmanaged, so AP start-up
#   never waits for a station scan, a DHCP timeout or wpa_supplicant.
# * hostapd requires zero2w-preload-uwe5622.service, and the address unit waits
#   (bounded) for /sys/class/net/wlan0 to exist: "module inserted" is not the
#   readiness boundary, "netdev exists and carries 192.168.50.1" is.
# * dnsmasq is ordered only after the address unit, never after hostapd: a
#   client cannot request a lease before the beacon exists, so DHCP must not
#   sit on the AP critical path.
# * "unit started" is not a readiness boundary anywhere in this profile:
#   zero2w-ap-dhcp.service being active only proves dnsmasq was exec'd, so
#   zero2w-ap-dhcp-ready.service waits (bounded) until the dnsmasq holding the
#   dedicated config is alive and UDP port 67 is really in the listening table.
# * zero2w-carplay-ready.target is the barrier a future runtime unit will wait
#   on. It depends on the AP probe, on the AP DHCP probe and on the early
#   Bluetooth load: initialisation is not complete while a client can associate
#   but cannot get an address, nor while the DHCP server merely appears to run.
#   There is no business executable yet, so the barrier pulls in nothing else.
# * The validated input image does not contain the USB debug network that was
#   configured on the test board after flashing. This stage installs that exact
#   reviewed RNDIS design explicitly: usb0 is 192.168.77.2/24, the host receives
#   192.168.77.10-20, NetworkManager leaves usb0 unmanaged, and sshd remains
#   byte-for-byte unchanged. The stage refuses an input that already contains a
#   usb0/192.168.77 provider, so it never overwrites an unknown wired setup.
#   wsl-15 verifies the complete installed helper, units, configuration and
#   enablement links in addition to replaying the inherited SSH/config manifest.
# * The delayed diagnostic AP timer is unlinked in this profile (a second AP
#   controller on wlan0 would fight hostapd); all diagnostic unit files, the
#   loader and the evidence collector stay on the card for manual use. The
#   inherited wpa_supplicant multi-user auto-start link is also removed; its
#   D-Bus alias and station profile remain for manual recovery. These two exact
#   inherited links are named in the explicit exclusion list. Every other
#   collected inherited byte and link stays under manifest verification.
#
# Local-only tool: it operates on regular image files through losetup. It
# refuses /dev/*, PhysicalDrive, symlinks, block devices, existing outputs and
# a mutable source, and it deletes a partial output when it fails.
set -euo pipefail

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_RAW=${1:?usage: $0 SUB10-DEVELOPMENT-IMAGE OUTPUT-SUB10-AP-IMAGE}
OUTPUT_RAW=${2:?usage: $0 SUB10-DEVELOPMENT-IMAGE OUTPUT-SUB10-AP-IMAGE}

AP_IFACE=wlan0
AP_SSID=Zero2WCar
AP_PSK=12345678
AP_CHANNEL=7
AP_ADDRESS=192.168.50.1/24
AP_HOSTAPD_CONF=/etc/zero2w/ap/hostapd-wlan0.conf
AP_DNSMASQ_CONF=/etc/zero2w/ap/dnsmasq-wlan0.conf
AP_MARKER=etc/zero2w-sub10-ap-development-v1
AP_PRESERVES=etc/zero2w-sub10-ap-development-v1.preserves
AP_USB_PROVIDERS=etc/zero2w-sub10-ap-development-v1.usb-providers
AP_EXCLUSIONS=etc/zero2w-sub10-ap-development-v1.exclusions
NM_UNMANAGED_CONF=/etc/NetworkManager/conf.d/30-zero2w-ap-wlan0-unmanaged.conf

fail() { echo "$*" >&2; exit 2; }
reject_device() {
  case "$2" in /dev|/dev/*|*PhysicalDrive*) fail "refusing unsafe $1 path: $2" ;; esac
}
reject_device source "$SOURCE_RAW"
if [ -L "$SOURCE_RAW" ]; then fail "refusing symlinked source image: $SOURCE_RAW"; fi
SOURCE=$(realpath -e -- "$SOURCE_RAW") || fail "source image does not exist"
reject_device source "$SOURCE"
[ -f "$SOURCE" ] && [ ! -b "$SOURCE" ] && [ ! -L "$SOURCE" ] || fail "source is not a regular image file"
[ ! -e "$OUTPUT_RAW" ] && [ ! -L "$OUTPUT_RAW" ] || fail "output already exists or is a symlink: $OUTPUT_RAW"
OUTPUT_DIR=$(realpath -e -- "$(dirname -- "$OUTPUT_RAW")") || fail "output directory does not exist"
OUTPUT="$OUTPUT_DIR/$(basename -- "$OUTPUT_RAW")"
reject_device output "$OUTPUT"
[ "$SOURCE" != "$OUTPUT" ] || fail "source and output are identical"

# The input of this stage is the already validated development image; the gate
# runs read-only and must pass before anything is copied.
IMAGE_PROFILE=sub10-development "$HERE/wsl-15-verify-no-initrd-image.sh" "$SOURCE"

# Immutability evidence for the source: metadata fingerprint, compared again at
# the end. Nothing here ever opens the source for writing.
source_fingerprint() { stat -c '%s %i %a %.19Y %.19W' -- "$SOURCE"; }
SOURCE_FP_BEFORE=$(source_fingerprint)

# Every resource this stage can acquire is declared, and the unwinder is
# installed, before the first allocation: if a mktemp itself fails, the trap
# must still be able to run over empty variables. Cleanup turns -e and -u off on
# purpose - a step that cannot apply (nothing mounted yet, no loop device yet)
# must not abort the rest of the unwinding nor mask the failure that triggered
# it, and expanding a variable that is legitimately still empty is not an error
# here. A partial output is never left behind for the next run to mistake for a
# finished image.
TMP_OUTPUT=
TMP=
LOOP=
COMPLETE=0
cleanup() {
  set +e
  set +u
  [ -n "$TMP" ] && mountpoint -q "$TMP/root" && umount "$TMP/root"
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  [ -n "$TMP" ] && rm -rf -- "$TMP"
  [ -n "$TMP_OUTPUT" ] && rm -f -- "$TMP_OUTPUT"
  [ "$COMPLETE" = 1 ] || { [ -n "$OUTPUT" ] && rm -f -- "$OUTPUT"; }
}
trap cleanup EXIT INT TERM
TMP_OUTPUT=$(mktemp "$OUTPUT_DIR/.zero2w-sub10-ap.tmp.XXXXXX")
TMP=$(mktemp -d /tmp/zero2w-sub10-ap.XXXXXX)
rm -f -- "$TMP_OUTPUT"
cp --reflink=auto -- "$SOURCE" "$TMP_OUTPUT"
ln -- "$TMP_OUTPUT" "$OUTPUT" || fail "output was created concurrently"
rm -f -- "$TMP_OUTPUT"
[ -f "$OUTPUT" ] && [ ! -L "$OUTPUT" ] && [ ! -b "$OUTPUT" ] || fail "unsafe output after creation"

mkdir -p "$TMP/root"
LOOP=$(losetup --find --show --partscan "$OUTPUT")
mount "${LOOP}p2" "$TMP/root"
ROOT=$TMP/root

# ---------------------------------------------------------------------------
# 1. Fingerprint the USB/SSH provisioning we must not disturb. Taken before any
#    write, so the manifest describes the inherited (source) state. What it
#    guarantees is byte and link fidelity, and completeness, for the paths the
#    collection below actually gathered; it makes no claim about USB/SSH paths
#    this stage never looked at.
# ---------------------------------------------------------------------------
MANIFEST=$TMP/preserves
: >"$MANIFEST"
MANIFEST_LIST=$TMP/paths
: >"$MANIFEST_LIST"

# Directories whose whole content is provisioning or radio-module state.
preserved_roots=(
  etc/ssh
  etc/NetworkManager
  etc/systemd/network
  etc/network
  etc/modprobe.d
  etc/modules-load.d
)
for rel in "${preserved_roots[@]}"; do
  [ -d "$ROOT/$rel" ] || continue
  find "$ROOT/$rel" \( -type f -o -type l \) -print0 >>"$MANIFEST_LIST"
done
for rel in etc/modules etc/fstab; do
  if [ -f "$ROOT/$rel" ]; then printf '%s\0' "$ROOT/$rel" >>"$MANIFEST_LIST"; fi
done
# Every /etc/systemd/system symlink: enablement, aliases, overrides and masks.
# A later stage that disables or rewires a USB/SSH unit changes this set.
find "$ROOT/etc/systemd/system" -type l -print0 >>"$MANIFEST_LIST"
# The source stage predates the working USB debug network. Refuse to overwrite
# any unknown provider; this profile adds one reviewed implementation from a
# known-empty baseline and then publishes the exact installed path list for the
# output gate.
USB_PROVIDER_LIST=$TMP/usb-providers
: >"$USB_PROVIDER_LIST"
while IFS= read -r -d '' provider; do
  fail "source image already contains usb0/192.168.77 provisioning: ${provider#"$ROOT/"}"
done < <(grep -RIlZ -E 'usb0|192\.168\.77' \
           "$ROOT/etc" "$ROOT/usr/local" "$ROOT/usr/bin" "$ROOT/usr/sbin" \
           "$ROOT/usr/lib/systemd" "$ROOT/usr/lib/NetworkManager" 2>/dev/null || true)
cat >"$USB_PROVIDER_LIST" <<'USB_PATHS'
etc/NetworkManager/conf.d/90-zero2w-usb-gadget-unmanaged.conf
etc/systemd/system/multi-user.target.wants/zero2w-usb-dhcp.service
etc/systemd/system/multi-user.target.wants/zero2w-usb-rndis.service
etc/systemd/system/zero2w-usb-dhcp.service
etc/systemd/system/zero2w-usb-rndis.service
usr/local/sbin/zero2w-usb-rndis
USB_PATHS
usb_providers=$(wc -l <"$USB_PROVIDER_LIST")

# The two fail-closed exceptions to the preservation rule: the delayed
# diagnostic AP timer and wpa_supplicant multi-user auto-start. The D-Bus alias,
# unit files and station profile remain. Exact paths, rather than patterns, keep
# every other collected inherited path under byte/link equality verification.
intentional_exclusions=(
  etc/systemd/system/timers.target.wants/zero2w-headless-diagnostics.timer
  etc/systemd/system/multi-user.target.wants/wpa_supplicant.service
  etc/systemd/system/multi-user.target.wants/NetworkManager.service
  etc/systemd/system/timers.target.wants/zero2w-load-bluetooth.timer
)
is_intentional_exclusion() {
  local rel=$1 want
  for want in "${intentional_exclusions[@]}"; do
    if [ "$rel" = "$want" ]; then return 0; fi
  done
  return 1
}
for rel in "${intentional_exclusions[@]}"; do
  # Membership in the fingerprint set is "file or symlink", so test both: a
  # dangling enablement link is still a link the gate would otherwise require.
  if [ ! -e "$ROOT/$rel" ] && [ ! -L "$ROOT/$rel" ]; then
    fail "intentional exclusion matches nothing in the inherited image: $rel"
  fi
  case "$rel" in
    etc/ssh|etc/ssh/*|etc/systemd/system/*ssh*|etc/NetworkManager|etc/NetworkManager/*|etc/systemd/network|etc/systemd/network/*|etc/network|etc/network/*|etc/modprobe.d|etc/modprobe.d/*|etc/modules-load.d|etc/modules-load.d/*|etc/modules|etc/fstab) fail "intentional exclusion may not exempt inherited provisioning: $rel" ;;
  esac
  if grep -qxF -- "$rel" "$USB_PROVIDER_LIST"; then
    fail "intentional exclusion may not exempt a usb0 provisioning provider: $rel"
  fi
done

sort -z -u "$MANIFEST_LIST" | while IFS= read -r -d '' path; do
  rel=${path#"$ROOT/"}
  case "$rel" in "$AP_MARKER"|"$AP_PRESERVES"|"$AP_USB_PROVIDERS"|"$AP_EXCLUSIONS"|etc/zero2w-sub10-ap-development-v1*) continue ;; esac
  if is_intentional_exclusion "$rel"; then continue; fi
  if [ -L "$path" ]; then
    printf 'L\t%s\t%s\n' "$(readlink -- "$path")" "$rel"
  elif [ -f "$path" ]; then
    printf 'F\t%s\t%s\n' "$(sha256sum -- "$path" | cut -d' ' -f1)" "$rel"
  fi
done >"$MANIFEST"
[ -s "$MANIFEST" ] || fail "empty preservation manifest"
awk -F'\t' '$3 == "etc/ssh/sshd_config" { found = 1 } END { exit !found }' "$MANIFEST" \
  || fail "manifest does not cover sshd_config"
# Cross-check the exemption against the manifest itself: an exempted path must
# not be fingerprinted. wsl-15 later replays every collected manifest record
# against the built image and re-checks this same list. This proves fidelity of
# the declared collection; it intentionally makes no claim about regular files
# outside the roots and provider search above.
for rel in "${intentional_exclusions[@]}"; do
  if awk -F'\t' -v want="$rel" '$3 == want { found = 1 } END { exit !found }' "$MANIFEST"; then
    fail "intentional exclusion is still fingerprinted as preserved: $rel"
  fi
done
printf 'PRESERVED_PATHS count=%s usb_provider_files=%s intentional_exclusions=%s\n' \
  "$(wc -l <"$MANIFEST")" "$usb_providers" "${#intentional_exclusions[@]}"

# ---------------------------------------------------------------------------
# 2. Binaries this profile needs must already be in the image; the stage does
#    not install packages, so record the absolute paths instead of guessing.
# ---------------------------------------------------------------------------
root_exec() {
  local name=$1 dir
  for dir in /usr/sbin /usr/bin /sbin /bin; do
    [ -f "$ROOT/$dir/$name" ] && [ -x "$ROOT/$dir/$name" ] || continue
    printf '/%s/%s\n' "$dir" "$name"
    return 0
  done
  return 1
}
HOSTAPD_BIN=$(root_exec hostapd) || fail "hostapd is not present in the source image"
DNSMASQ_BIN=$(root_exec dnsmasq) || fail "dnsmasq is not present in the source image"
HOSTAPD_CLI_BIN=$(root_exec hostapd_cli || true)
IW_BIN=$(root_exec iw || true)
# The DHCP readiness probe needs ss with process ownership output. UDP 67 is
# also used by the independent USB DHCP service, so checking only that some
# socket exists would be a false-positive; the probe must tie the socket to the
# exact AP dnsmasq PID.
SOCK_PROBE_BIN=$(root_exec ss) || fail "ss is not present in the source image; AP DHCP socket ownership could not be verified"
printf 'AP_TOOLS hostapd=%s dnsmasq=%s hostapd_cli=%s iw=%s socket_probe=%s\n' \
  "$HOSTAPD_BIN" "$DNSMASQ_BIN" "${HOSTAPD_CLI_BIN:-absent}" "${IW_BIN:-absent}" "$SOCK_PROBE_BIN"

# ---------------------------------------------------------------------------
# 3. Configuration files.
# ---------------------------------------------------------------------------
install -d -m 755 "$ROOT/etc/zero2w" "$ROOT/etc/zero2w/ap"
umask 077
cat >"$ROOT$AP_HOSTAPD_CONF" <<EOF
# Zero2W Sub-10 development access point. Managed by zero2w-hostapd.service.
# Deliberately the only AP controller on $AP_IFACE in this profile.
interface=$AP_IFACE
driver=nl80211
ctrl_interface=/run/hostapd
ctrl_interface_group=0
ssid=$AP_SSID
hw_mode=g
channel=$AP_CHANNEL
wpa=2
wpa_key_mgmt=WPA-PSK
wpa_passphrase=$AP_PSK
wpa_pairwise=CCMP
rsn_pairwise=CCMP
EOF
umask 022
chmod 600 "$ROOT$AP_HOSTAPD_CONF"
chown root:root "$ROOT$AP_HOSTAPD_CONF"

cat >"$ROOT$AP_DNSMASQ_CONF" <<EOF
# Dedicated DHCP/DNS for the Zero2W development AP. Passed with --conf-file= so
# the vendor /etc/dnsmasq.conf and /etc/dnsmasq.d stay unused and untouched.
# Bound to $AP_IFACE only, so it cannot answer on the USB gadget interface.
interface=$AP_IFACE
bind-interfaces
dhcp-range=192.168.50.10,192.168.50.250,255.255.255.0,12h
dhcp-option=3,192.168.50.1
dhcp-option=6,192.168.50.1
dhcp-leasefile=/run/dnsmasq-zero2w-ap.leases
no-resolv
server=223.5.5.5
server=114.114.114.114
EOF
chmod 600 "$ROOT$AP_DNSMASQ_CONF"
chown root:root "$ROOT$AP_DNSMASQ_CONF"

# Keep NetworkManager (and therefore its wpa_supplicant/P2P instances) off
# wlan0. The station profile stays on the card untouched for manual use.
install -d -m 755 "$ROOT/etc/NetworkManager/conf.d"
cat >"$ROOT$NM_UNMANAGED_CONF" <<EOF
# Generated by $(basename "$0"): the Sub-10 AP profile owns $AP_IFACE.
[keyfile]
unmanaged-devices=interface-name:$AP_IFACE
EOF
chmod 644 "$ROOT$NM_UNMANAGED_CONF"
chown root:root "$ROOT$NM_UNMANAGED_CONF"

# Install the exact USB RNDIS debug network validated on the running board.
# It is deliberately independent of wlan0 and remains a recovery/SSH path if
# AP development fails.
cat >"$ROOT/usr/local/sbin/zero2w-usb-rndis" <<'USB_RNDIS'
#!/bin/sh
set -eu
G=/sys/kernel/config/usb_gadget/zero2w-rndis
case "${1:-}" in
  up)
    mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
    mkdir -p "$G"
    cd "$G"
    printf '0x1d6b' > idVendor
    printf '0x0104' > idProduct
    printf '0x0200' > bcdUSB
    printf '0x0100' > bcdDevice
    printf '0xEF' > bDeviceClass
    printf '0x02' > bDeviceSubClass
    printf '0x01' > bDeviceProtocol
    mkdir -p strings/0x409
    tr -d '\n' </etc/machine-id > strings/0x409/serialnumber
    printf 'Zero2W' > strings/0x409/manufacturer
    printf 'Zero2W USB RNDIS Debug' > strings/0x409/product
    mkdir -p configs/c.1/strings/0x409
    printf 'RNDIS Debug Network' > configs/c.1/strings/0x409/configuration
    printf '250' > configs/c.1/MaxPower
    mkdir -p functions/rndis.usb0
    printf '02:00:00:00:77:02' > functions/rndis.usb0/dev_addr
    printf '02:00:00:00:77:01' > functions/rndis.usb0/host_addr
    mkdir -p os_desc
    printf '1' > os_desc/use
    printf '0xcd' > os_desc/b_vendor_code
    printf 'MSFT100' > os_desc/qw_sign
    printf 'RNDIS' > functions/rndis.usb0/os_desc/interface.rndis/compatible_id
    printf '5162001' > functions/rndis.usb0/os_desc/interface.rndis/sub_compatible_id
    [ -L configs/c.1/rndis.usb0 ] || ln -s functions/rndis.usb0 configs/c.1/
    [ -L os_desc/c.1 ] || ln -s configs/c.1 os_desc/
    udc=$(ls /sys/class/udc | head -n 1)
    [ -n "$udc" ]
    printf '%s' "$udc" > UDC
    i=0
    while ! ip link show usb0 >/dev/null 2>&1; do
      i=$((i + 1)); [ "$i" -lt 50 ] || exit 1; sleep 0.1
    done
    ip address replace 192.168.77.2/24 dev usb0
    ip link set usb0 up
    ;;
  down)
    if [ -d "$G" ]; then
      cd "$G"
      printf '' > UDC 2>/dev/null || true
    fi
    ;;
  *)
    echo 'usage: zero2w-usb-rndis up|down' >&2
    exit 2
    ;;
esac
USB_RNDIS
chmod 755 "$ROOT/usr/local/sbin/zero2w-usb-rndis"
chown root:root "$ROOT/usr/local/sbin/zero2w-usb-rndis"

cat >"$ROOT/etc/systemd/system/zero2w-usb-rndis.service" <<'USB_RNDIS_UNIT'
[Unit]
Description=Zero2W USB-C RNDIS debug network gadget
After=sys-kernel-config.mount NetworkManager.service
Before=zero2w-usb-dhcp.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/zero2w-usb-rndis up
ExecStop=/usr/local/sbin/zero2w-usb-rndis down

[Install]
WantedBy=multi-user.target
USB_RNDIS_UNIT
chmod 644 "$ROOT/etc/systemd/system/zero2w-usb-rndis.service"

cat >"$ROOT/etc/systemd/system/zero2w-usb-dhcp.service" <<EOF
[Unit]
Description=DHCP for Zero2W USB-C debug network
Requires=zero2w-usb-rndis.service
After=zero2w-usb-rndis.service

[Service]
Type=simple
ExecStart=$DNSMASQ_BIN --keep-in-foreground --conf-file=/dev/null --port=0 --interface=usb0 --bind-dynamic --dhcp-authoritative --dhcp-range=192.168.77.10,192.168.77.20,255.255.255.0,1h --dhcp-option=3 --dhcp-option=6
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
chmod 644 "$ROOT/etc/systemd/system/zero2w-usb-dhcp.service"

cat >"$ROOT/etc/NetworkManager/conf.d/90-zero2w-usb-gadget-unmanaged.conf" <<'USB_NM'
[keyfile]
unmanaged-devices=interface-name:usb0
USB_NM
chmod 644 "$ROOT/etc/NetworkManager/conf.d/90-zero2w-usb-gadget-unmanaged.conf"

# BlueZ must start only after the vendor transport module has created hci0.
install -d -m 755 "$ROOT/etc/systemd/system/bluetooth.service.d"
cat >"$ROOT/etc/systemd/system/bluetooth.service.d/zero2w-transport.conf" <<'BT_OVERRIDE'
[Unit]
Requires=zero2w-load-bluetooth.service
After=zero2w-load-bluetooth.service
BT_OVERRIDE
chmod 644 "$ROOT/etc/systemd/system/bluetooth.service.d/zero2w-transport.conf"

# ---------------------------------------------------------------------------
# 4. Helper scripts.
# ---------------------------------------------------------------------------
cat >"$ROOT/usr/local/sbin/zero2w-ap-address" <<'SCRIPT'
#!/bin/bash
# Give the AP interface its fixed development address.
#
# The UWE5622 netdev shows up asynchronously after sprdwl_ng finishes probing
# the SDIO card, so this bounded wait - not "the module was inserted" - is the
# readiness boundary that hostapd is allowed to depend on. Bringing a freshly
# probed interface up, assigning the address and reading it back can each fail
# once and succeed a moment later, so every step - netdev existence, link up,
# address replace and the exact-address verification - is retried inside the one
# bounded loop below, and only the expired limit fails the unit. Nothing is
# latched into a permanent failure while the driver is still settling, because
# hostapd, dnsmasq, the DHCP probe and the CarPlay barrier all require this
# unit.
set -u
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IFACE=wlan0
ADDRESS=192.168.50.1/24
WAIT_STEPS=100
WAIT_STEP_S=0.2

log() { printf 'uptime=%s %s\n' "$(cut -d ' ' -f1 /proc/uptime 2>/dev/null || echo NA)" "$*"; }

step='never-attempted'
attempt=0
for _ in $(seq 1 "$WAIT_STEPS"); do
    attempt=$((attempt + 1))
    if [ ! -e "/sys/class/net/$IFACE" ]; then
        step='no_netdev'
        sleep "$WAIT_STEP_S"
        continue
    fi
    if ! ip link set "$IFACE" up 2>/dev/null; then
        step='link_up_failed'
        sleep "$WAIT_STEP_S"
        continue
    fi
    # replace, not add: re-running this step over an address that is already
    # there is how the loop converges, so it has to stay idempotent.
    if ! ip -4 addr replace "$ADDRESS" dev "$IFACE" 2>/dev/null; then
        step='addr_set_failed'
        sleep "$WAIT_STEP_S"
        continue
    fi
    if ip -4 -br addr show dev "$IFACE" 2>/dev/null | grep -qF "$ADDRESS"; then
        log "RESULT=AP_INTERFACE_READY iface=$IFACE address=$ADDRESS attempts=$attempt"
        exit 0
    fi
    step='addr_missing'
    sleep "$WAIT_STEP_S"
done

# The limit expired: only here does this unit fail, and it fails on purpose.
log "RESULT=AP_ADDRESS_NOT_READY iface=$IFACE address=$ADDRESS step=$step attempts=$attempt steps_limit=$WAIT_STEPS step_sleep_s=$WAIT_STEP_S"
exit 1
SCRIPT
chmod 755 "$ROOT/usr/local/sbin/zero2w-ap-address"
chown root:root "$ROOT/usr/local/sbin/zero2w-ap-address"

cat >"$ROOT/usr/local/sbin/zero2w-ap-ready" <<'SCRIPT'
#!/bin/bash
# Bounded probe: is the access point actually operating?
#
# zero2w-ap-ready.service is a oneshot that only exits 0 here, which makes
# zero2w-carplay-ready.target a barrier on a broadcasting AP rather than a
# "unit started" formality. Probes are tried strongest-first:
#   hostapd_cli status state=ENABLED (needs ctrl_interface=/run/hostapd)
#   iw dev wlan0 info -> type AP
#   a live hostapd process (no wireless CLI installed at all)
set -u
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IFACE=wlan0
TRIES=40
TRIAL_S=0.5
STAMP=/run/zero2w-carplay-ready.env
last='never-probed'

probe() {
    local state
    if command -v hostapd_cli >/dev/null 2>&1; then
        state=$(hostapd_cli -i "$IFACE" status 2>/dev/null | sed -n 's/^state=//p')
        if [ "$state" = 'ENABLED' ]; then
            last="hostapd_state=ENABLED"
            return 0
        fi
        last="hostapd_state=${state:-no-reply}"
        return 1
    fi
    if command -v iw >/dev/null 2>&1; then
        if iw dev "$IFACE" info 2>/dev/null | grep -q '^type AP'; then
            last='iw_type=AP'
            return 0
        fi
        last='iw_reports_no_ap'
        return 1
    fi
    if pgrep -x hostapd >/dev/null 2>&1; then
        last='hostapd_running_without_wireless_cli'
        return 0
    fi
    last='hostapd_not_running'
    return 1
}

for _ in $(seq 1 "$TRIES"); do
    probe && break
    sleep "$TRIAL_S"
done

if ! probe; then
    printf 'uptime=%s RESULT=AP_NOT_READY iface=%s last=%s\n' \
        "$(cut -d ' ' -f1 /proc/uptime 2>/dev/null || echo NA)" "$IFACE" "$last"
    exit 1
fi

uptime=$(cut -d ' ' -f1 /proc/uptime 2>/dev/null || echo NA)
printf 'uptime=%s RESULT=AP_READY iface=%s %s\n' "$uptime" "$IFACE" "$last"
{
    printf 'ZERO2W_AP_READY=1\n'
    printf 'ZERO2W_AP_IFACE=%s\n' "$IFACE"
    printf 'ZERO2W_AP_IPV4=192.168.50.1/24\n'
    printf 'ZERO2W_AP_UPTIME=%s\n' "$uptime"
    printf 'ZERO2W_AP_PROBE=%s\n' "$last"
} >"$STAMP" 2>/dev/null || true
exit 0
SCRIPT
chmod 755 "$ROOT/usr/local/sbin/zero2w-ap-ready"
chown root:root "$ROOT/usr/local/sbin/zero2w-ap-ready"

# The DHCP probe cannot be one fully literal script: it has to name the dnsmasq
# and the listening-socket checker this builder discovered in the source image.
# So the constants are rendered by an expanded heredoc and the logic that must
# land in the image verbatim follows in a quoted one.
{
  cat <<EOF
#!/bin/bash
# Bounded probe: does the development AP really hand out addresses?
#
# zero2w-ap-dhcp-ready.service is a oneshot that only exits 0 here, which is
# what puts a serving DHCP server on the CarPlay barrier instead of a dnsmasq
# that was merely exec'd. A bad config, an interface that came up late or a UDP
# port somebody else took all leave the unit active and the clients un-served,
# so two observed facts decide, and both must hold:
#   the dnsmasq started with /etc/zero2w/ap/dnsmasq-wlan0.conf is running
#   UDP port 67 is owned by that exact process, where a DISCOVER lands
# IFACE, DNSMASQ_BIN, SOCK_PROBE and DNSMASQ_CONF are the interface and the
# executables this builder discovered in the source image, embedded by absolute
# path so the probe never resolves its own tools through PATH at boot.
set -u
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IFACE=$AP_IFACE
UDP_PORT=67
DNSMASQ_BIN=$DNSMASQ_BIN
SOCK_PROBE=$SOCK_PROBE_BIN
DNSMASQ_CONF=$AP_DNSMASQ_CONF
TRIES=40
TRIAL_S=0.5
EOF
  cat <<'SCRIPT'
last='never-probed'

log() { printf 'uptime=%s %s\n' "$(cut -d ' ' -f1 /proc/uptime 2>/dev/null || echo NA)" "$*"; }

probe() {
    # Match our server by its dedicated config, then require ss to report that
    # same PID as the owner of UDP/67. This cannot be satisfied by the separate
    # USB dnsmasq, which also legitimately listens on port 67.
    local pids pid sockets
    pids=$(pgrep -f -- "^$DNSMASQ_BIN -k --conf-file=$DNSMASQ_CONF([[:space:]]|$)" 2>/dev/null || true)
    if [ -z "$pids" ]; then
        last='dnsmasq_not_running'
        return 1
    fi
    sockets=$("$SOCK_PROBE" -H -lunp "sport = :$UDP_PORT" 2>/dev/null || true)
    for pid in $pids; do
        if printf '%s\n' "$sockets" | grep -Fq "pid=$pid,"; then
            last="udp${UDP_PORT}_owned_by_pid=$pid"
            return 0
        fi
    done
    last='udp_port_not_owned_by_ap_dnsmasq'
    return 1
}

for _ in $(seq 1 "$TRIES"); do
    probe && break
    sleep "$TRIAL_S"
done

if ! probe; then
    log "RESULT=AP_DHCP_NOT_READY iface=$IFACE port=$UDP_PORT last=$last"
    exit 1
fi

log "RESULT=AP_DHCP_READY iface=$IFACE port=$UDP_PORT $last"
exit 0
SCRIPT
} >"$ROOT/usr/local/sbin/zero2w-ap-dhcp-ready"
chmod 755 "$ROOT/usr/local/sbin/zero2w-ap-dhcp-ready"
chown root:root "$ROOT/usr/local/sbin/zero2w-ap-dhcp-ready"

# ---------------------------------------------------------------------------
# 5. Units.
# ---------------------------------------------------------------------------
UNIT_DIR=$ROOT/etc/systemd/system
cat >"$UNIT_DIR/zero2w-ap-address.service" <<EOF
[Unit]
Description=Zero2W development AP interface address ($AP_IFACE $AP_ADDRESS)
Documentation=file:$AP_HOSTAPD_CONF
DefaultDependencies=no
Requires=zero2w-preload-uwe5622.service
After=zero2w-preload-uwe5622.service
Before=shutdown.target
Conflicts=shutdown.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/zero2w-ap-address
RemainAfterExit=yes
# Matches the bounded interface wait inside the script, with slack for fsck.
TimeoutStartSec=30

[Install]
WantedBy=multi-user.target
EOF

cat >"$UNIT_DIR/zero2w-hostapd.service" <<EOF
[Unit]
Description=Zero2W development access point (direct hostapd on $AP_IFACE)
Documentation=file:$AP_HOSTAPD_CONF
Requires=zero2w-preload-uwe5622.service zero2w-ap-address.service
After=zero2w-preload-uwe5622.service zero2w-ap-address.service
# No NetworkManager dependency on purpose: station scans must not gate the AP.
StartLimitIntervalSec=30
StartLimitBurst=3

[Service]
Type=simple
RuntimeDirectory=hostapd
RuntimeDirectoryMode=0755
ExecStart=$HOSTAPD_BIN $AP_HOSTAPD_CONF
Restart=on-failure
RestartSec=3
TimeoutStartSec=30

[Install]
WantedBy=multi-user.target
EOF

cat >"$UNIT_DIR/zero2w-ap-dhcp.service" <<EOF
[Unit]
Description=Zero2W development AP DHCP and DNS (dedicated dnsmasq)
Documentation=file:$AP_DNSMASQ_CONF
Requires=zero2w-ap-address.service
After=zero2w-ap-address.service
# Deliberately unordered with respect to hostapd: dnsmasq binds $AP_IFACE and
# waits for clients, so it must never sit on the AP readiness critical path.
# A dnsmasq that cannot start on this config is a permanent error, not a
# transient one, so its restart loop is bounded exactly like hostapd's. When
# the limit trips, zero2w-ap-dhcp-ready.service fails with it and the barrier
# stops instead of claiming an initialised board.
StartLimitIntervalSec=30
StartLimitBurst=3

[Service]
Type=simple
ExecStart=$DNSMASQ_BIN -k --conf-file=$AP_DNSMASQ_CONF
Restart=on-failure
RestartSec=3
TimeoutStartSec=30

[Install]
WantedBy=multi-user.target
EOF

cat >"$UNIT_DIR/zero2w-ap-dhcp-ready.service" <<EOF
[Unit]
Description=Zero2W development AP DHCP readiness probe (dnsmasq + UDP 67 on $AP_IFACE)
Documentation=file:$AP_DNSMASQ_CONF
Requires=zero2w-ap-dhcp.service
After=zero2w-ap-dhcp.service
# The unit above going active only means dnsmasq was exec'd. This oneshot is the
# boundary: it waits (bounded) for the process holding the dedicated config and
# for UDP 67 in the listening socket table. Ordered after the DHCP server only,
# never after hostapd, so the beacon stays off the DHCP critical path.

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/zero2w-ap-dhcp-ready
RemainAfterExit=yes
# Matches the bounded probe inside the script, with slack for the tool runs.
TimeoutStartSec=40

[Install]
WantedBy=multi-user.target
EOF

cat >"$UNIT_DIR/zero2w-ap-ready.service" <<EOF
[Unit]
Description=Zero2W access point readiness barrier
Documentation=file:$AP_HOSTAPD_CONF
Requires=zero2w-hostapd.service
After=zero2w-hostapd.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/zero2w-ap-ready
RemainAfterExit=yes
TimeoutStartSec=40

[Install]
WantedBy=multi-user.target
EOF

cat >"$UNIT_DIR/zero2w-carplay-ready.target" <<EOF
[Unit]
Description=Zero2W CarPlay runtime readiness barrier (access point + DHCP + Bluetooth)
Requires=zero2w-ap-ready.service zero2w-ap-dhcp-ready.service zero2w-load-bluetooth.service bluetooth.service
After=zero2w-ap-ready.service zero2w-ap-dhcp-ready.service zero2w-load-bluetooth.service bluetooth.service
# DHCP belongs to the barrier even though it is unordered against hostapd, and
# it belongs to it through the probe rather than through zero2w-ap-dhcp.service:
# a client that can associate but cannot get a lease has not reached an
# initialised board, and neither has one whose DHCP server only runs on paper.
# The service still arrives transitively, through the probe's Requires=.
# There is no CarPlay executable in the image yet. When one exists it should
# declare After=zero2w-carplay-ready.target and
# Requires=zero2w-carplay-ready.target instead of probing the network or the
# radio itself.

[Install]
WantedBy=multi-user.target
EOF

mkdir -p "$UNIT_DIR/multi-user.target.wants"
for unit in zero2w-ap-address.service zero2w-hostapd.service zero2w-ap-dhcp.service \
            zero2w-ap-dhcp-ready.service zero2w-ap-ready.service \
            zero2w-carplay-ready.target zero2w-usb-rndis.service \
            zero2w-usb-dhcp.service; do
  ln -sfn "../$unit" "$UNIT_DIR/multi-user.target.wants/$unit"
done

# ---------------------------------------------------------------------------
# 6. Drop exactly the four enablement links named in intentional_exclusions.
#    NetworkManager/WPA and timer unit files remain for manual station recovery;
#    the AP path itself needs none of them.
# ---------------------------------------------------------------------------
rm -f "$UNIT_DIR/timers.target.wants/zero2w-headless-diagnostics.timer" \
      "$UNIT_DIR/multi-user.target.wants/wpa_supplicant.service" \
      "$UNIT_DIR/multi-user.target.wants/NetworkManager.service" \
      "$UNIT_DIR/timers.target.wants/zero2w-load-bluetooth.timer"
[ ! -e "$UNIT_DIR/timers.target.wants/zero2w-headless-diagnostics.timer" ] \
  || fail "diagnostic timer is still enabled and would contend for $AP_IFACE"
[ ! -e "$UNIT_DIR/multi-user.target.wants/wpa_supplicant.service" ] \
  || fail "wpa_supplicant is still enabled at multi-user.target"
[ ! -e "$UNIT_DIR/multi-user.target.wants/NetworkManager.service" ] \
  || fail "NetworkManager is still enabled at multi-user.target"
[ ! -e "$UNIT_DIR/timers.target.wants/zero2w-load-bluetooth.timer" ] \
  || fail "the delayed Bluetooth timer is still enabled"
# The exemption above covers one path only: if the same timer were enabled
# through another link, that link stays fingerprinted as preserved and this
# profile would leave a second AP controller armed for wlan0.
if find "$UNIT_DIR" -type l -name zero2w-headless-diagnostics.timer -print -quit | grep -q .; then
  fail "diagnostic timer is still enabled through a link outside the exemption list"
fi
[ -f "$UNIT_DIR/zero2w-headless-diagnostics.timer" ] || fail "diagnostic timer unit file must stay for manual recovery"
[ -f "$UNIT_DIR/zero2w-headless-diagnostics.service" ] || fail "diagnostic service unit file must stay for manual recovery"
[ -x "$ROOT/usr/local/sbin/zero2w-headless-diagnostics" ] || fail "diagnostic collector missing"

# ---------------------------------------------------------------------------
# 7. Marker, preservation manifest and exemption list.
# ---------------------------------------------------------------------------
printf '%s\n' \
  'profile=sub10-ap-development-v1' \
  'status=local-structure-validated-hardware-pending' \
  'rollback=zero2w-carplay-debian-no-initrd-sub10-development.img' \
  "ap_interface=$AP_IFACE" \
  "ap_ssid=$AP_SSID" \
  'ap_security=wpa2-psk' \
  "ap_channel=$AP_CHANNEL" \
  "ap_ipv4=$AP_ADDRESS" \
  'ap_dhcp_range=192.168.50.10,192.168.50.250,255.255.255.0,12h' \
  'ap_controller=zero2w-hostapd.service' \
  'dhcp_server=zero2w-ap-dhcp.service' \
  'ready_barrier=zero2w-carplay-ready.target' \
  'diagnostic_ap_timer=disabled-unit-retained' \
  'network_manager=disabled-unit-retained' \
  'bluetooth_timer=disabled-unit-retained' \
  'usb_wired_ssh=installed-reviewed-rndis' \
  >"$ROOT/$AP_MARKER"
install -m 644 -o root -g root "$MANIFEST" "$ROOT/$AP_PRESERVES"
install -m 644 -o root -g root "$USB_PROVIDER_LIST" "$ROOT/$AP_USB_PROVIDERS"
# The exemption list travels with the image: wsl-15 re-checks that it names
# exactly the diagnostic timer link and nothing that provides usb0/SSH.
printf '%s\n' "${intentional_exclusions[@]}" >"$TMP/exclusions"
install -m 644 -o root -g root "$TMP/exclusions" "$ROOT/$AP_EXCLUSIONS"
while IFS= read -r provider; do
  [ -n "$provider" ] || continue
  if [ ! -e "$ROOT/$provider" ] && [ ! -L "$ROOT/$provider" ]; then
    fail "reviewed USB provisioning path was not installed: $provider"
  fi
done <"$USB_PROVIDER_LIST"

sync
umount "$ROOT"
losetup -d "$LOOP"
LOOP=

IMAGE_PROFILE=sub10-ap-development "$HERE/wsl-15-verify-no-initrd-image.sh" "$OUTPUT"

[ "$(source_fingerprint)" = "$SOURCE_FP_BEFORE" ] || fail "source image was modified; refusing to report success"
COMPLETE=1
echo "SUB10_AP_DEVELOPMENT_IMAGE_OK image=$OUTPUT source=$SOURCE"
