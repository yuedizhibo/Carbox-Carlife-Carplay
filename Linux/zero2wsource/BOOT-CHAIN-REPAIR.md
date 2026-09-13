# Zero2W `6.1.31-opi-min` no-initrd boot bundle

The active kernel is deliberately `6.1.31-opi-min`, not the vendor
`6.1.31-sun50iw9` kernel.  Its emitted configuration proves the native root
path is built in:

```text
CONFIG_MMC_SUNXI=y
CONFIG_MMC_BLOCK=y
CONFIG_EXT4_FS=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
# CONFIG_BLK_DEV_INITRD is not set
```

The root filesystem is plain ext4 partition 2; it has no LUKS, RAID, LVM,
network-root, or early `/usr` requirement. Because `init/do_mounts.c` does not
resolve filesystem `UUID=` without userspace, `orangepiEnv.txt` uses the p2
partition-table identifier (`rootdev=PARTUUID=...`). U-Boot also derives p2's
PARTUUID dynamically and falls back to `/dev/mmcblk0p2`. Therefore
`boot/no-initrd/boot.cmd` intentionally does **not** load `uInitrd` and invokes:

```text
booti ${kernel_addr_r} - ${fdt_addr_r}
```

This removes the mixed vendor `6.1.31-sun50iw9` ramdisk handoff while retaining
all existing DT overlay/fixup handling, `rootwait`, native PARTUUID root
selection, and serial console. The ext4 filesystem UUID remains valid in
`/etc/fstab` after userspace starts. The vendor `uInitrd` may remain on the FAT partition as an
inactive rollback asset; it must never be reintroduced into this kernel's boot
command.

## Reproducible assembly (WSL only)

Install `u-boot-tools`, regenerate the headless config before every kernel
build, and run the assembly script against an image **file**:

```bash
sudo apt-get install -y u-boot-tools
cd /mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource
bash scripts/wsl-10-minimize-config.sh
VARIANT=minimal bash scripts/wsl-11-build-minimal.sh
bash scripts/wsl-12-finalize-minimal.sh
VARIANT=minimal bash scripts/wsl-13-verify-minimal.sh
sudo bash scripts/wsl-14-assemble-no-initrd-image.sh \
  build-out-image/zero2w-carplay-debian.img \
  build-out-image/zero2w-carplay-debian-no-initrd.img
```

The script rejects `/dev/*` and `PhysicalDrive` paths, copies the source first,
and uses only loop devices for the output file.  It checks the kernel config,
release-specific module tree, root UUID/partition configuration, serial/headless
configuration, diagnostics, SSH and Wi-Fi profile before installing the exact
custom `Image`, exact Zero2W DTB, reviewed `boot.cmd`, and regenerated
`boot.scr`, and the ABI-bound `6.1.31-opi-min` module tree from the same
manifested build. It then decompiles the script and performs a read-only image
check. It does not replace SPL/U-Boot, the partition table, firmware, SSH,
NetworkManager, or diagnostics.

`boot.scr` must be regenerated, never edited as bytes:

```bash
mkimage -C none -A arm -T script -d boot/no-initrd/boot.cmd /tmp/boot.scr
mkimage -l /tmp/boot.scr
dumpimage -T script -p 0 -o /tmp/boot.component /tmp/boot.scr
# dumpimage component 0 includes the 8-byte U-Boot script-size table.
tail -c +9 /tmp/boot.component > /tmp/boot.cmd.decompiled
cmp boot/no-initrd/boot.cmd /tmp/boot.cmd.decompiled
```

Run the independent read-only forensic gate after assembly:

```bash
sudo bash scripts/wsl-15-verify-no-initrd-image.sh \
  build-out-image/zero2w-carplay-debian-no-initrd.img
```

After hardware validation, create the production profile as a new image. This
keeps the source image immutable, moves diagnostics and Bluetooth to delayed
non-blocking timers, selects `multi-user.target`, removes the serial-getty
90-second timeout, and disables unrelated server/desktop services:

```bash
sudo bash scripts/wsl-16-optimize-production-image.sh \
  build-out-image/zero2w-carplay-debian-no-initrd-uwe-addr-order-fix-candidate.img \
  build-out-image/zero2w-carplay-debian-no-initrd-production.img

IMAGE_PROFILE=production sudo -E bash scripts/wsl-15-verify-no-initrd-image.sh \
  build-out-image/zero2w-carplay-debian-no-initrd-production.img
```

The production image retains the `Zero2W-Diag` fallback and forensic collector,
but starts it 30 seconds after boot so it cannot delay SSH or the boot target.
Bluetooth transport loads asynchronously after 20 seconds. On the validated
board, the resulting profile reached `multi-user.target` at 7.230 seconds,
started SSH at 8.075 seconds, completed systemd startup at 8.943 seconds, and
acquired Wi-Fi at 12.445 seconds.

After a test-card boot, UART0 at 115200 must show the final command line and
either `VFS: Mounted root` or the first MMC/ext4 error.  A successful image
structure check is not hardware boot proof.  Do not flash or inspect Windows
`PhysicalDrive2`; test-card flashing and UART capture are separate, operator-owned
steps.
