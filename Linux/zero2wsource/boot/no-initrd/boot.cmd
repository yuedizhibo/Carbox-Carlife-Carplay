# Zero2W 6.1.31-opi-min native-ext4 boot script.
# This kernel has CONFIG_BLK_DEV_INITRD=n.  Keep uInitrd out of this flow.
# Rebuild with: mkimage -C none -A arm -T script -d boot.cmd boot.scr

# default values
setenv load_addr "0x45000000"
setenv overlay_error "false"
# Native-kernel fallback: partition 2 is the ext4 root filesystem.
setenv rootdev "/dev/mmcblk0p2"
setenv verbosity "1"
setenv rootfstype "ext4"
setenv console "both"
setenv docker_optimizations "on"
setenv bootlogo "false"

# Print boot source
itest.b *0x10028 == 0x00 && echo "U-boot loaded from SD"
itest.b *0x10028 == 0x02 && echo "U-boot loaded from eMMC or secondary SD"
itest.b *0x10028 == 0x03 && echo "U-boot loaded from SPI"
echo "Boot script loaded from ${devtype} (6.1.31-opi-min, no initrd)"

if test -e ${devtype} ${devnum} ${prefix}orangepiEnv.txt; then
	load ${devtype} ${devnum} ${load_addr} ${prefix}orangepiEnv.txt
	env import -t ${load_addr} ${filesize}
fi

if test "${console}" = "display" || test "${console}" = "both"; then setenv consoleargs "console=ttyS0,115200 console=tty1"; fi
if test "${console}" = "serial"; then setenv consoleargs "console=ttyS0,115200"; fi
if test "${bootlogo}" = "true"; then
	setenv consoleargs "splash plymouth.ignore-serial-consoles ${consoleargs}"
else
	setenv consoleargs "splash=verbose ${consoleargs}"
fi

# Without an initramfs, Linux cannot resolve filesystem UUID=.  Ask U-Boot for
# the DOS/GPT PARTUUID of root partition 2, which the kernel resolves natively.
if test "${devtype}" = "mmc"; then
	part uuid mmc ${devnum}:1 partuuid
	if part uuid mmc ${devnum}:2 rootpartuuid; then
		setenv rootdev "PARTUUID=${rootpartuuid}"
	fi
fi
setenv bootargs "root=${rootdev} rootwait rootfstype=${rootfstype} ${consoleargs} consoleblank=0 loglevel=${verbosity} ubootpart=${partuuid} usb-storage.quirks=${usbstoragequirks} ${extraargs} ${extraboardargs}"
if test "${docker_optimizations}" = "on"; then setenv bootargs "${bootargs} cgroup_enable=memory swapaccount=1"; fi

load ${devtype} ${devnum} ${fdt_addr_r} ${prefix}dtb/${fdtfile}
fdt addr ${fdt_addr_r}
fdt resize 65536
for overlay_file in ${overlays}; do
	if load ${devtype} ${devnum} ${load_addr} ${prefix}dtb/allwinner/overlay/${overlay_prefix}-${overlay_file}.dtbo; then
		echo "Applying kernel provided DT overlay ${overlay_prefix}-${overlay_file}.dtbo"
		fdt apply ${load_addr} || setenv overlay_error "true"
	fi
done
for overlay_file in ${user_overlays}; do
	if load ${devtype} ${devnum} ${load_addr} ${prefix}overlay-user/${overlay_file}.dtbo; then
		echo "Applying user provided DT overlay ${overlay_file}.dtbo"
		fdt apply ${load_addr} || setenv overlay_error "true"
	fi
done
if test "${overlay_error}" = "true"; then
	echo "Error applying DT overlays, restoring original DT"
	load ${devtype} ${devnum} ${fdt_addr_r} ${prefix}dtb/${fdtfile}
else
	if load ${devtype} ${devnum} ${load_addr} ${prefix}dtb/allwinner/overlay/${overlay_prefix}-fixup.scr; then
		echo "Applying kernel provided DT fixup script (${overlay_prefix}-fixup.scr)"
		source ${load_addr}
	fi
	if test -e ${devtype} ${devnum} ${prefix}fixup.scr; then
		load ${devtype} ${devnum} ${load_addr} ${prefix}fixup.scr
		echo "Applying user provided fixup script (fixup.scr)"
		source ${load_addr}
	fi
fi

if test "${ethernet_phy}" = "rtl8211f"; then
	fdt set /soc/ethernet@5020000 allwinner,rx-delay-ps <3100>
	fdt set /soc/ethernet@5020000 allwinner,tx-delay-ps <700>
fi
if test "${ethernet_phy}" = "yt8531c"; then
	fdt set /soc/ethernet@5020000 allwinner,rx-delay-ps <0>
	fdt set /soc/ethernet@5020000 allwinner,tx-delay-ps <600>
fi

# Native root path: MMC, block and ext4 are built into 6.1.31-opi-min.
# Do not load the vendor 6.1.31-sun50iw9 uInitrd here.
load ${devtype} ${devnum} ${kernel_addr_r} ${prefix}Image
booti ${kernel_addr_r} - ${fdt_addr_r}
