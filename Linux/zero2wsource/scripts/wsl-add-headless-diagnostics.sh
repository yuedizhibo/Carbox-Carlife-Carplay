#!/usr/bin/env bash
# Inject the headless forensic boot diagnostics into a Zero2W image file.
#
# Rationale for the staged loader: the UWE5622 failure happens inside
# sprdwl_probe()/start_marlin(), which systemd-modules-load drives with no
# timeout, no ordering guarantee and no out-of-band record. Loading the chain
# step by step from user space, marker per marker, is the only way to see the
# real return code on a board that has no serial console attached.
set -euo pipefail
IMG="${1:-/home/dfkvm/opi/images/extracted/Orangepizero2w_1.0.4_debian_bookworm_server_linux6.1.31/Orangepizero2w_1.0.4_debian_bookworm_server_linux6.1.31.img}"
LOOP="$(losetup --find --show --partscan "$IMG")"
ROOT=/mnt/zero2w-root
BOOT=/mnt/zero2w-boot
cleanup(){ mountpoint -q "$BOOT" && umount "$BOOT" || true; mountpoint -q "$ROOT" && umount "$ROOT" || true; losetup -d "$LOOP" 2>/dev/null || true; }
trap cleanup EXIT
mkdir -p "$ROOT" "$BOOT"
mount "${LOOP}p2" "$ROOT"
mount "${LOOP}p1" "$BOOT"

# Do not bind the profile to wlan0: predictable interface naming can differ.
sed -i '/^interface-name=/d' "$ROOT/etc/NetworkManager/system-connections/default-wifi.nmconnection"
chmod 600 "$ROOT/etc/NetworkManager/system-connections/default-wifi.nmconnection"

# Take the UWE5622 chain away from the eager module loaders. Deleting the exact
# lines (never rewriting the files) keeps every unrelated vendor entry, and makes
# repeated injection of this script a no-op.
UWE_MODULE_PATTERN='^(uwe5622_bsp_sdio|sprdwl_ng|sprdbt_tty)$'
strip_uwe_module_entries() {
    local file
    for file in "$ROOT/etc/modules" "$ROOT"/etc/modules-load.d/*.conf; do
        [ -f "$file" ] || continue
        if grep -Eq "$UWE_MODULE_PATTERN" "$file"; then
            sed -i -E "/$UWE_MODULE_PATTERN/d" "$file"
            printf 'STRIPPED_UWE_MODULE_ENTRIES file=%s\n' "${file#"$ROOT"}"
        fi
    done
}
strip_uwe_module_entries

# ---------------------------------------------------------------------------
# Staged UWE5622 loader
# ---------------------------------------------------------------------------
cat >"$ROOT/usr/local/sbin/zero2w-load-uwe5622" <<'LOADER'
#!/bin/bash
# Load the UWE5622 Wi-Fi chain one module at a time with hard timeouts, and
# mirror every marker to both the FAT partition and the journal so a kernel
# fault in the middle of a probe still leaves a readable last-known-step.
#
# sprdbt_tty is deliberately not loaded: it shares the sdiohal bus and the WCN
# subsystem with Wi-Fi, so loading it here would contaminate the experiment.
set -u
LOG=/boot/ZERO2W-MODULE-LOAD.txt
BSP=uwe5622_bsp_sdio
WL=sprdwl_ng
BSP_TIMEOUT=45
WL_TIMEOUT=60
SNAP_DMESG_LINES=50

stamp() {
    printf '%s uptime=%s' \
        "$(date -Is 2>/dev/null || echo NO_CLOCK)" \
        "$(cut -d ' ' -f1 /proc/uptime 2>/dev/null || echo NA)"
}

# A read-only /boot must not blind us: keep the same markers in the journal.
mark() {
    printf '%s %s\n' "$(stamp)" "$*"
    printf '%s %s\n' "$(stamp)" "$*" >>"$LOG" 2>/dev/null || true
    sync
}

append() {
    printf '%s\n' "$*" >>"$LOG" 2>/dev/null || true
}

loaded_now() {
    awk -v want="$1" '$1 == want { found = 1 } END { exit !found }' /proc/modules 2>/dev/null
}

sdio_snapshot() {
    append '--- sdio devices ---'
    for dev in /sys/bus/sdio/devices/*; do
        [ -d "$dev" ] || continue
        line="SDIO $(basename "$dev")"
        for attr in vendor device class; do
            line="$line $attr=$(cat "$dev/$attr" 2>/dev/null || echo none)"
        done
        if [ -L "$dev/driver" ]; then
            line="$line driver=$(basename "$(readlink -f "$dev/driver" 2>/dev/null || echo unknown)")"
        else
            line="$line driver=none"
        fi
        append "$line"
    done
    sync
}

modules_snapshot() {
    append '--- /proc/modules (wireless subset) ---'
    awk '{print $1}' /proc/modules 2>/dev/null \
        | grep -Ei 'sprd|uwe|sunxi_addr|cfg80211|mac80211|rfkill|bluetooth' >>"$LOG" 2>/dev/null || true
    append "MODULE_TOTAL=$(wc -l < /proc/modules 2>/dev/null || echo 0)"
    sync
}

dmesg_snapshot() {
    append "--- dmesg tail after $1 ---"
    timeout -k 5 15 dmesg 2>/dev/null | tail -n "$SNAP_DMESG_LINES" >>"$LOG" 2>/dev/null || true
    sync
}

link_snapshot() {
    append '--- ip -br link ---'
    timeout -k 5 10 ip -br link >>"$LOG" 2>&1 || append "IP_LINK rc=$?"
    sync
}

try_module() {
    mod=$1
    limit=$2
    if loaded_now "$mod"; then
        mark "STEP=$mod RESULT=ALREADY_LOADED note=loaded_before_this_service_by_udev_or_builtin"
        return 0
    fi
    mark "STEP=$mod RESULT=BEGIN timeout_s=$limit"
    sdio_snapshot
    sync
    timeout -k 5 "$limit" modprobe -v "$mod" >>"$LOG" 2>&1
    rc=$?
    append "MODPROBE_RC=$rc mod=$mod"
    mark "STEP=$mod RESULT=MODPROBE_RC rc=$rc"
    dmesg_snapshot "$mod"
    modules_snapshot
    link_snapshot
    if loaded_now "$mod"; then
        mark "STEP=$mod RESULT=LOADED rc=$rc"
        return 0
    fi
    mark "STEP=$mod RESULT=NOT_IN_PROC_MODULES rc=$rc"
    return 1
}

: >"$LOG" 2>/dev/null || true
rc_all=0
mark "STEP=LOADER_START kernel=$(uname -r 2>/dev/null || echo NO_UNAME) log=$LOG"
sdio_snapshot
modules_snapshot
link_snapshot

if try_module "$BSP" "$BSP_TIMEOUT"; then
    try_module "$WL" "$WL_TIMEOUT" || rc_all=2
else
    rc_all=1
    mark "STEP=$WL RESULT=SKIPPED reason=bsp_not_loaded"
fi

if loaded_now "$WL"; then
    mark "STEP=LOADER_DONE RESULT=WLAN_MODULE_PRESENT rc=$rc_all"
elif loaded_now "$BSP"; then
    mark "STEP=LOADER_DONE RESULT=BSP_ONLY_WLAN_MISSING rc=$rc_all"
else
    mark "STEP=LOADER_DONE RESULT=NO_UWE_MODULES_LOADED rc=$rc_all"
fi
exit "$rc_all"
LOADER
chmod 755 "$ROOT/usr/local/sbin/zero2w-load-uwe5622"

cat >"$ROOT/etc/systemd/system/zero2w-load-uwe5622.service" <<'LOADER_UNIT'
[Unit]
Description=Staged UWE5622 Wi-Fi module loader (forensic)
Documentation=file:/boot/ZERO2W-MODULE-LOAD.txt
After=local-fs.target systemd-modules-load.service
Before=NetworkManager.service
Wants=systemd-modules-load.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/zero2w-load-uwe5622
RemainAfterExit=yes
# Internal BSP/WLAN watchdogs plus snapshots can take up to about 170 seconds.
TimeoutStartSec=210
Slice=system.slice

[Install]
WantedBy=multi-user.target
LOADER_UNIT
mkdir -p "$ROOT/etc/systemd/system/multi-user.target.wants"
ln -sfn ../zero2w-load-uwe5622.service "$ROOT/etc/systemd/system/multi-user.target.wants/zero2w-load-uwe5622.service"

# ---------------------------------------------------------------------------
# Headless diagnostics
# ---------------------------------------------------------------------------
cat >"$ROOT/usr/local/sbin/zero2w-headless-diagnostics" <<'SCRIPT'
#!/bin/bash
# Headless out-of-band evidence collector.
#
# Ordering rule: kernel/module evidence is written and flushed BEFORE any radio
# userspace call, because every observed failure so far stopped the previous
# revision of this script at an untimed nmcli invocation.
set -u
STATUS=/boot/ZERO2W-STATUS.txt
LOADER_LOG=/boot/ZERO2W-MODULE-LOAD.txt
OUT=''
RC=0
# CALL_RC is the status of the last bounded command; `call` cannot use $? because
# its own closing marker overwrites it.
CALL_RC=0

: >"$STATUS" 2>/dev/null || true
exec >>"$STATUS" 2>&1
sync

stamp() {
    printf '%s uptime=%s' \
        "$(date -Is 2>/dev/null || echo NO_CLOCK)" \
        "$(cut -d ' ' -f1 /proc/uptime 2>/dev/null || echo NA)"
}

say() {
    printf '%s %s\n' "$(stamp)" "$*"
}

# call: run one command under a hard timeout and always leave a BEGIN/DONE pair
# on disk, so a hang is attributable to a specific invocation.
call() {
    label=$1
    limit=$2
    shift 2
    printf 'CHECK %s BEGIN timeout_s=%s' "$label" "$limit"
    printf ' %s' "$@"
    printf '\n'
    sync
    timeout -k 5 "$limit" "$@"
    CALL_RC=$?
    say "CHECK $label DONE rc=$CALL_RC"
    return "$CALL_RC"
}

# call_out: same, but capture stdout for markers such as the Wi-Fi device scan.
call_out() {
    label=$1
    limit=$2
    shift 2
    OUT=''
    RC=0
    printf 'CHECK %s BEGIN timeout_s=%s' "$label" "$limit"
    printf ' %s' "$@"
    printf '\n'
    sync
    OUT=$(timeout -k 5 "$limit" "$@" 2>/dev/null)
    RC=$?
    CALL_RC=$RC
    say "CHECK $label DONE rc=$RC out=$(printf '%s' "$OUT" | tr '\n' '|' | cut -c1-200)"
    return "$RC"
}

# sect: run a named producer function as the head of a bounded pipeline. Keeping
# the pipeline in bash (instead of sh -c) means quoting cannot silently corrupt an
# evidence command, and the producer's exit status is still recorded.
sect() {
    label=$1
    producer=$2
    lines=$3
    printf -- '=== %s ===\n' "$label"
    sync
    "$producer" 2>&1 | tail -n "$lines"
    src=${PIPESTATUS[0]}
    say "=== $label END rc=$src ==="
    sync
}

# Loader markers are timestamp-prefixed, so the STEP= field is never at col 1.
p_loader_markers() { timeout -k 5 15 grep -E 'STEP=' "$LOADER_LOG" 2>/dev/null; }
p_loader_log() { timeout -k 5 15 tail -n 200 "$LOADER_LOG" 2>/dev/null; }
p_wcn_dmesg() { timeout -k 5 20 dmesg 2>/dev/null | grep -Ei 'sprd|uwe5622|WCN|marlin|loopcheck|unisoc|ADDR_MGT|addr_parse|get_custom_mac|cfg80211|mmc[0-9]|sdio|firmware|Call trace|Oops|BUG:|pc :|internal error'; }
p_dmesg_tail() { timeout -k 5 20 dmesg 2>/dev/null; }
p_modules() { timeout -k 5 15 cat /proc/modules 2>/dev/null | awk '{print $1, $2, $3, $4}' | grep -Ei 'sprd|uwe|sunxi_addr|cfg80211|mac80211|rfkill|bluetooth'; }
p_ip_link() { timeout -k 5 15 ip -br link 2>&1; }
p_ip_addr() { timeout -k 5 15 ip -brief address 2>&1; }
p_wiphy() { timeout -k 5 15 ls -1 /sys/class/ieee80211 2>&1; }
p_failed_units() { timeout -k 5 20 systemctl --failed --no-pager --full 2>&1; }
p_modules_load_unit() { timeout -k 5 20 systemctl status systemd-modules-load.service --no-pager -l 2>&1; }
p_loader_unit() { timeout -k 5 20 systemctl status zero2w-load-uwe5622.service --no-pager -l 2>&1; }
p_loader_journal() { timeout -k 5 25 journalctl -b -u systemd-modules-load.service -u zero2w-load-uwe5622.service --no-pager -n 120 2>&1; }
p_sdio_devices() {
    for dev in /sys/bus/sdio/devices/*; do
        [ -d "$dev" ] || continue
        drv=none
        if [ -L "$dev/driver" ]; then
            drv=$(basename "$(readlink -f "$dev/driver" 2>/dev/null)" 2>/dev/null || echo unknown)
        fi
        printf 'SDIO %s vendor=%s device=%s class=%s driver=%s\n' \
            "$(basename "$dev")" "$(cat "$dev/vendor" 2>/dev/null)" \
            "$(cat "$dev/device" 2>/dev/null)" "$(cat "$dev/class" 2>/dev/null)" "$drv"
    done
}

printf '=== ZERO2W BOOT STATUS ===\n'
say 'PHASE=HEADER'
printf 'STATE=USERSPACE_REACHED\nKERNEL=%s\nBOOT_ID=%s\n' "$(uname -r)" "$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
printf 'UPTIME=%s\n' "$(cut -d ' ' -f1 /proc/uptime 2>/dev/null)"
sync

# ---- forensic section first: never depends on radio userspace working ----
printf '\n=== STAGED MODULE LOADER EVIDENCE ===\n'
sect LOADER_MARKERS p_loader_markers 130
sect LOADER_LOG_TAIL p_loader_log 60

printf '\n=== UWE/WCN KERNEL LOG ===\n'
sect WCN_DMESG p_wcn_dmesg 160
sect DMESG_TAIL p_dmesg_tail 200

printf '\n=== MODULES ===\n'
sect MODULES p_modules 80
say "MODULE_TOTAL=$(wc -l < /proc/modules 2>/dev/null || echo NA)"
sect SDIO_DEVICES p_sdio_devices 40

printf '\n=== IP LINK ===\n'
sect IP_LINK p_ip_link 40
sect WIRELESS_HW p_wiphy 20

printf '\n=== FAILED UNITS ===\n'
sect FAILED_UNITS p_failed_units 30

printf '\n=== MODULE-LOAD STATUS ===\n'
sect MODULES_LOAD_UNIT p_modules_load_unit 40
sect LOADER_UNIT p_loader_unit 40
sect LOADER_JOURNAL p_loader_journal 100
sync

# ---- LED heartbeat (kept after the evidence, it is not diagnostic data) ----
for led in /sys/class/leds/*; do
    [ -e "$led/trigger" ] || continue
    if grep -qw heartbeat "$led/trigger"; then
        echo heartbeat >"$led/trigger" 2>/dev/null || true
        printf 'LED_HEARTBEAT=%s\n' "$(basename "$led")"
        break
    fi
done

# ---- radio bring-up, every call bounded and checkpointed ----
if command -v rfkill >/dev/null 2>&1; then
    call RFKILL_UNBLOCK 10 rfkill unblock all
else
    say 'CHECK RFKILL_UNBLOCK DONE rc=NO_BINARY'
fi

iface=''
if command -v nmcli >/dev/null 2>&1; then
    call NMRADIO_WIFI_ON 15 nmcli radio wifi on
    call NMCONN_RELOAD 15 nmcli connection reload
    for _ in $(seq 1 6); do
        call_out NM_FIND_WIFI 8 nmcli -t -f DEVICE,TYPE device status
        if [ "$RC" = 0 ]; then
            iface=$(printf '%s\n' "$OUT" | awk -F: '$2=="wifi"{print $1;exit}')
        fi
        [ -n "$iface" ] && break
        sleep 2
    done
else
    say 'NMCLI=NO_BINARY'
fi
printf 'WIFI_INTERFACE=%s\n' "${iface:-NOT_FOUND}"
sync

if [ -n "$iface" ]; then
    # Autoconnect may already have succeeded; otherwise request it explicitly.
    for _ in 1 2; do
        call_out NM_STATE 10 nmcli -g GENERAL.STATE device show "$iface"
        case "$OUT" in 100*) break;; esac
        call NM_UP_DEFAULT 25 nmcli --wait 10 connection up id default-wifi ifname "$iface"
        sleep 5
    done
    call_out NM_STATE_FINAL 10 nmcli -g GENERAL.STATE device show "$iface"
    if [[ "$OUT" == 100* ]]; then
        printf 'STATE=WIFI_CONNECTED\n'
        call NM_ADDRESS 10 nmcli -t -f GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY device show "$iface"
    else
        printf 'STATE=WIFI_FAILED_STARTING_FALLBACK_AP\n'
        call NM_AP_DELETE 15 nmcli connection delete zero2w-diagnostic
        call NM_AP_ADD 20 nmcli connection add type wifi ifname "$iface" con-name zero2w-diagnostic autoconnect no ssid Zero2W-Diag
        call NM_AP_MODIFY 20 nmcli connection modify zero2w-diagnostic 802-11-wireless.mode ap 802-11-wireless.band bg wifi-sec.key-mgmt wpa-psk wifi-sec.psk zero2w1234 ipv4.method shared ipv4.addresses 192.168.50.1/24 ipv6.method disabled
        call NM_AP_UP 25 nmcli --wait 15 connection up zero2w-diagnostic
        if [ "$CALL_RC" = 0 ]; then
            printf 'STATE=FALLBACK_AP_ACTIVE\nSSID=Zero2W-Diag\nIP=192.168.50.1\n'
        else
            printf 'STATE=FALLBACK_AP_FAILED\n'
        fi
    fi
else
    printf 'STATE=WIFI_INTERFACE_MISSING\n'
fi

# ---- closing network evidence ----
printf '\n=== NMCLI ===\n'
call NM_DEVICE_STATUS 15 nmcli device status
call NM_ACTIVE_CONNECTIONS 15 nmcli connection show --active
printf '\n=== IP ADDRESS ===\n'
sect IP_ADDRESS p_ip_addr 40
printf '\n=== RFKILL ===\n'
command -v rfkill >/dev/null 2>&1 && call RFKILL_LIST 10 rfkill list
say 'PHASE=DIAGNOSTICS_DONE'
sync
SCRIPT
chmod 755 "$ROOT/usr/local/sbin/zero2w-headless-diagnostics"

cat >"$ROOT/etc/systemd/system/zero2w-headless-diagnostics.service" <<'UNIT'
[Unit]
Description=Zero2W headless boot and Wi-Fi diagnostics
Documentation=file:/boot/ZERO2W-STATUS.txt
After=local-fs.target systemd-modules-load.service zero2w-load-uwe5622.service NetworkManager.service
Wants=NetworkManager.service zero2w-load-uwe5622.service
ConditionPathExists=/usr/bin/nmcli

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/zero2w-headless-diagnostics
RemainAfterExit=yes
# Every radio operation is bounded; allow the complete worst-case evidence path.
TimeoutStartSec=360

[Install]
WantedBy=multi-user.target
UNIT
mkdir -p "$ROOT/etc/systemd/system/multi-user.target.wants"
ln -sfn ../zero2w-headless-diagnostics.service "$ROOT/etc/systemd/system/multi-user.target.wants/zero2w-headless-diagnostics.service"

# Preserve useful logs across boots, bounded to avoid filling the card.
mkdir -p "$ROOT/var/log/journal" "$ROOT/etc/systemd/journald.conf.d"
cat >"$ROOT/etc/systemd/journald.conf.d/20-zero2w-diagnostics.conf" <<'EOF'
[Journal]
Storage=persistent
SystemMaxUse=32M
RuntimeMaxUse=16M
EOF

# Serial is the definitive console for a display-less kernel.
sed -i 's/^console=.*/console=serial/' "$BOOT/orangepiEnv.txt"
grep -q '^extraargs=' "$BOOT/orangepiEnv.txt" || echo 'extraargs=systemd.show_status=true' >>"$BOOT/orangepiEnv.txt"
# Both FAT evidence files start empty on every image build.
rm -f "$BOOT/ZERO2W-STATUS.txt" "$BOOT/ZERO2W-MODULE-LOAD.txt"
sync
printf 'DIAGNOSTICS_INSTALLED\n'
grep -v '^psk=' "$ROOT/etc/NetworkManager/system-connections/default-wifi.nmconnection"
cat "$ROOT/etc/systemd/system/zero2w-load-uwe5622.service"
cat "$ROOT/etc/systemd/system/zero2w-headless-diagnostics.service"
