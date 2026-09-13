#!/usr/bin/env bash
# Assemble a disposable Zero2W image around the already-built 6.1.31-opi-min
# native-ext4 kernel. This script accepts regular image files only, never devices.
set -euo pipefail

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WIN=${WIN:-$(CDPATH= cd -- "$HERE/.." && pwd)}
SOURCE_IMAGE_RAW=${1:?usage: $0 SOURCE-IMAGE OUTPUT-IMAGE}
OUTPUT_IMAGE_RAW=${2:?usage: $0 SOURCE-IMAGE OUTPUT-IMAGE}
KERNEL_DIR=${KERNEL_DIR:-$WIN/build-out-minimal/kernel}
BOOT_CMD=$WIN/boot/no-initrd/boot.cmd
KREL=6.1.31-opi-min
MODULE_ARCHIVE="modules-$KREL.tar.gz"
MODULE_MANIFEST="modules-$KREL.sha256"

fail_unsafe_path() { echo "refusing unsafe $1: $2" >&2; exit 2; }
reject_device_path() {
    case "$2" in
        /dev|/dev/*|*PhysicalDrive*) fail_unsafe_path "$1" "$2" ;;
    esac
}
canonical_regular_input() {
    local raw=$1 canonical
    reject_device_path source "$raw"
    canonical=$(realpath -e -- "$raw") || fail_unsafe_path source "$raw"
    reject_device_path source "$canonical"
    [ -f "$canonical" ] && [ ! -b "$canonical" ] || fail_unsafe_path source "$raw"
    printf '%s\n' "$canonical"
}
canonical_new_output() {
    local raw=$1 parent base canonical_parent
    reject_device_path output "$raw"
    [ ! -e "$raw" ] && [ ! -L "$raw" ] || fail_unsafe_path output "$raw (pre-existing output or symlink)"
    parent=$(dirname -- "$raw")
    base=$(basename -- "$raw")
    [ "$base" != . ] && [ "$base" != .. ] && [ -d "$parent" ] || fail_unsafe_path output "$raw"
    canonical_parent=$(realpath -e -- "$parent") || fail_unsafe_path output "$raw"
    reject_device_path output "$canonical_parent"
    [ -d "$canonical_parent" ] || fail_unsafe_path output "$raw"
    printf '%s/%s\n' "$canonical_parent" "$base"
}
assert_safe_output() {
    local expected=$1 actual
    [ ! -L "$expected" ] && [ -f "$expected" ] && [ ! -b "$expected" ] || fail_unsafe_path output "$expected"
    actual=$(realpath -e -- "$expected") || fail_unsafe_path output "$expected"
    [ "$actual" = "$expected" ] || fail_unsafe_path output "$expected (canonical path changed)"
    reject_device_path output "$actual"
}
verify_bundle() {
    local manifest_hash
    [ -f "$KERNEL_DIR/Image" ] && [ -f "$KERNEL_DIR/dtb/sun50i-h618-orangepi-zero2w.dtb" ] && \
      [ -f "$KERNEL_DIR/kernel.config" ] && [ -f "$KERNEL_DIR/$MODULE_ARCHIVE" ] && \
      [ -f "$KERNEL_DIR/$MODULE_MANIFEST" ] && [ -f "$KERNEL_DIR/artifact-manifest.sha256" ] && \
      [ -f "$KERNEL_DIR/provenance.txt" ] || { echo "missing ABI-bound minimal kernel bundle in $KERNEL_DIR" >&2; exit 2; }
    grep -qx 'format=zero2w-kernel-bundle-v1' "$KERNEL_DIR/provenance.txt"
    grep -qx "kernel_release=$KREL" "$KERNEL_DIR/provenance.txt"
    manifest_hash=$(sha256sum "$KERNEL_DIR/artifact-manifest.sha256" | awk '{print $1}')
    grep -qx "manifest_sha256=$manifest_hash" "$KERNEL_DIR/provenance.txt"
    ( cd "$KERNEL_DIR" && sha256sum -c artifact-manifest.sha256 )
    tar -tzf "$KERNEL_DIR/$MODULE_ARCHIVE" | while IFS= read -r entry; do
        case "$entry" in
            "lib/modules/$KREL"|"lib/modules/$KREL"/*) ;;
            *) echo "unsafe module archive entry: $entry" >&2; exit 1 ;;
        esac
    done
}

for tool in mkimage dumpimage losetup mount umount blkid cmp findmnt realpath sha256sum tar diff ln gzip sed; do
    command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done
SOURCE_IMAGE=$(canonical_regular_input "$SOURCE_IMAGE_RAW")
OUTPUT_IMAGE=$(canonical_new_output "$OUTPUT_IMAGE_RAW")
[ "$SOURCE_IMAGE" != "$OUTPUT_IMAGE" ] || fail_unsafe_path output "same as source"
verify_bundle
[ -f "$BOOT_CMD" ] || { echo "missing reviewed no-initrd boot.cmd" >&2; exit 2; }

# This is the explicit evidence allowing native root rather than a new initramfs.
for setting in CONFIG_MMC_SUNXI CONFIG_MMC_BLOCK CONFIG_EXT4_FS CONFIG_DEVTMPFS CONFIG_DEVTMPFS_MOUNT CONFIG_SERIAL_8250_CONSOLE; do
    grep -qx "${setting}=y" "$KERNEL_DIR/kernel.config" || { echo "required built-in setting missing: $setting" >&2; exit 3; }
done
grep -qx '# CONFIG_BLK_DEV_INITRD is not set' "$KERNEL_DIR/kernel.config" || { echo "kernel is not the reviewed no-initrd artifact" >&2; exit 3; }
grep -qx 'CONFIG_LOCALVERSION="-opi-min"' "$KERNEL_DIR/kernel.config" || { echo "unexpected kernel release" >&2; exit 3; }
! grep -Eq '^[[:space:]]*load[[:space:]].*uInitrd' "$BOOT_CMD"
grep -Eq '^[[:space:]]*booti[[:space:]]+\$\{kernel_addr_r\}[[:space:]]+-[[:space:]]+\$\{fdt_addr_r\}[[:space:]]*$' "$BOOT_CMD"

# The output is created atomically with a hard link: it cannot overwrite or follow
# an output symlink introduced after validation. Keep the temporary in the canonical parent.
OUTPUT_DIR=$(dirname -- "$OUTPUT_IMAGE")
OUTPUT_BASE=$(basename -- "$OUTPUT_IMAGE")
TMP_OUTPUT=$(mktemp "$OUTPUT_DIR/.${OUTPUT_BASE}.tmp.XXXXXX")
rm -f -- "$TMP_OUTPUT"
cp --reflink=auto -- "$SOURCE_IMAGE" "$TMP_OUTPUT"
[ ! -L "$TMP_OUTPUT" ] && [ -f "$TMP_OUTPUT" ] && [ ! -b "$TMP_OUTPUT" ] || fail_unsafe_path output "$TMP_OUTPUT"
ln -- "$TMP_OUTPUT" "$OUTPUT_IMAGE" || fail_unsafe_path output "$OUTPUT_IMAGE (created concurrently)"
rm -f -- "$TMP_OUTPUT"
assert_safe_output "$OUTPUT_IMAGE"
cmp -s "$SOURCE_IMAGE" "$OUTPUT_IMAGE" || { echo "output copy verification failed" >&2; exit 2; }

TMP=$(mktemp -d)
LOOP=
MODULE_STAGE=$TMP/modules
cleanup() {
    set +e
    mountpoint -q "$TMP/boot" && umount "$TMP/boot"
    mountpoint -q "$TMP/root" && umount "$TMP/root"
    [ -n "$LOOP" ] && losetup -d "$LOOP"
    rm -rf "$TMP"
}
trap cleanup EXIT
mkdir -p "$TMP/boot" "$TMP/root" "$MODULE_STAGE"
LOOP=$(losetup --find --show --partscan "$OUTPUT_IMAGE")
mount "${LOOP}p1" "$TMP/boot"
mount "${LOOP}p2" "$TMP/root"
BOOT=$TMP/boot ROOT=$TMP/root

# Do not replace the bootloader, partition table, diagnostics, SSH, or Wi-Fi profile.
ROOT_UUID=$(blkid -s UUID -o value "${LOOP}p2")
ROOT_PARTUUID=$(blkid -s PARTUUID -o value "${LOOP}p2")
[ -n "$ROOT_UUID" ] && [ -n "$ROOT_PARTUUID" ] || { echo "missing root UUID/PARTUUID" >&2; exit 4; }
# Filesystem UUID= needs userspace/initramfs resolution. This kernel has no
# initramfs, so publish the partition-table UUID that name_to_dev_t supports.
sed -i "s|^rootdev=.*$|rootdev=PARTUUID=$ROOT_PARTUUID|" "$BOOT/orangepiEnv.txt"
grep -qx "rootdev=PARTUUID=$ROOT_PARTUUID" "$BOOT/orangepiEnv.txt"
grep -Eq "^UUID=$ROOT_UUID[[:space:]]+/[[:space:]]+ext4" "$ROOT/etc/fstab" || { echo "fstab root UUID mismatch" >&2; exit 4; }
grep -qx 'rootfstype=ext4' "$BOOT/orangepiEnv.txt"
grep -qx 'console=serial' "$BOOT/orangepiEnv.txt"
[ -e "$ROOT/etc/systemd/system/multi-user.target.wants/zero2w-headless-diagnostics.service" ] || { echo "headless diagnostics not enabled" >&2; exit 4; }
[ -e "$ROOT/etc/ssh/sshd_config" ] || { echo "SSH configuration missing" >&2; exit 4; }
find "$ROOT/etc/NetworkManager/system-connections" -type f -name '*.nmconnection' -print -quit | grep -q . || { echo "Wi-Fi profile missing" >&2; exit 4; }

# Extract only the verified, release-bound payload, then replace the exact ABI tree.
tar -xzf "$KERNEL_DIR/$MODULE_ARCHIVE" -C "$MODULE_STAGE" --no-same-owner
[ -d "$MODULE_STAGE/lib/modules/$KREL" ] || { echo "module archive lacks $KREL" >&2; exit 4; }
! find "$MODULE_STAGE/lib/modules/$KREL" -type l -print -quit | grep -q . || { echo "module archive contains symlinks" >&2; exit 4; }
( cd "$MODULE_STAGE" && sha256sum -c "$KERNEL_DIR/$MODULE_MANIFEST" >/dev/null )
echo "staged module manifest: OK"
[ ! -L "$ROOT/lib/modules" ] || { echo "rootfs /lib/modules must not be a symlink" >&2; exit 4; }
rm -rf -- "$ROOT/lib/modules/$KREL"
mkdir -p "$ROOT/lib/modules"
cp -a -- "$MODULE_STAGE/lib/modules/$KREL" "$ROOT/lib/modules/"
diff -qr --no-dereference "$MODULE_STAGE/lib/modules/$KREL" "$ROOT/lib/modules/$KREL"
( cd "$ROOT" && sha256sum -c "$KERNEL_DIR/$MODULE_MANIFEST" >/dev/null )
echo "installed module manifest: OK"

# Retain inactive vendor files for rollback, but install one coherent active pair.
cp -f "$BOOT_CMD" "$BOOT/boot.cmd"
mkimage -C none -A arm -T script -d "$BOOT/boot.cmd" "$BOOT/boot.scr"
cp -f "$KERNEL_DIR/Image" "$BOOT/Image"
cp -f "$KERNEL_DIR/kernel.config" "$BOOT/config-$KREL"
cp -f "$KERNEL_DIR/System.map" "$BOOT/System.map-$KREL"
install -D -m 0644 "$KERNEL_DIR/dtb/sun50i-h618-orangepi-zero2w.dtb" "$BOOT/dtb/allwinner/sun50i-h618-orangepi-zero2w.dtb"
cmp -s "$BOOT/Image" "$KERNEL_DIR/Image"
cmp -s "$BOOT/dtb/allwinner/sun50i-h618-orangepi-zero2w.dtb" "$KERNEL_DIR/dtb/sun50i-h618-orangepi-zero2w.dtb"
mkimage -l "$BOOT/boot.scr"
# U-Boot script images prepend an 8-byte script-size table to component 0.
dumpimage -T script -p 0 -o "$TMP/boot.component" "$BOOT/boot.scr" >/dev/null
tail -c +9 "$TMP/boot.component" > "$TMP/boot.cmd.decompiled"
cmp -s "$BOOT/boot.cmd" "$TMP/boot.cmd.decompiled"
! grep -Eq '^[[:space:]]*load[[:space:]].*uInitrd' "$TMP/boot.cmd.decompiled"
grep -Eq '^[[:space:]]*booti[[:space:]]+\$\{kernel_addr_r\}[[:space:]]+-[[:space:]]+\$\{fdt_addr_r\}[[:space:]]*$' "$TMP/boot.cmd.decompiled"
sync
umount "$BOOT"; umount "$ROOT"; losetup -d "$LOOP"; LOOP=
assert_safe_output "$OUTPUT_IMAGE"

# Bounded read-only post-assembly inspection; no physical device is ever opened.
LOOP=$(losetup --read-only --find --show --partscan "$OUTPUT_IMAGE")
mount -o ro "${LOOP}p1" "$TMP/boot"
mount -o ro "${LOOP}p2" "$TMP/root"
[ "$(blkid -s UUID -o value "${LOOP}p2")" = "$ROOT_UUID" ]
[ "$(blkid -s PARTUUID -o value "${LOOP}p2")" = "$ROOT_PARTUUID" ]
grep -qx "rootdev=PARTUUID=$ROOT_PARTUUID" "$TMP/boot/orangepiEnv.txt"
cmp -s "$TMP/boot/Image" "$KERNEL_DIR/Image"
cmp -s "$TMP/boot/dtb/allwinner/sun50i-h618-orangepi-zero2w.dtb" "$KERNEL_DIR/dtb/sun50i-h618-orangepi-zero2w.dtb"
[ ! -L "$TMP/root/lib/modules" ]
( cd "$TMP/root" && sha256sum -c "$KERNEL_DIR/$MODULE_MANIFEST" >/dev/null )
echo "read-only module manifest: OK"
diff -qr --no-dereference "$MODULE_STAGE/lib/modules/$KREL" "$TMP/root/lib/modules/$KREL"
findmnt -no SOURCE,OPTIONS "$TMP/boot"
findmnt -no SOURCE,OPTIONS "$TMP/root"
mkimage -l "$TMP/boot/boot.scr"
echo "NO_INITRD_IMAGE_ASSEMBLY_OK output=$OUTPUT_IMAGE kernel=$KREL root_uuid=$ROOT_UUID root_partuuid=$ROOT_PARTUUID"
