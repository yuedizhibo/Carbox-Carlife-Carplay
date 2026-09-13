# Orange Pi Zero 2W 极简系统调优 —— 运行内存 + 启动速度

> 目标：驱动板上所有硬件的前提下，把内核和系统的**常驻内存**与**上电到可用时间**压到最低。
> 板子：Allwinner H616-H618-BGA-284（H618，4×Cortex-A53@1.5GHz，Mali-G31 MP2，1/2/4GB LPDDR4，
> AP6256 Wi-Fi6+BT5.0，AXP313a+AC200，100M 以太网，16MB SPI NOR，TF 卡，USB-C OTG，Micro-HDMI）。

---

## 0. 先看全局：时间/内存都花在哪

上电 → 可用，一共四段，砍掉的时间和内存量级完全不同：

| 阶段 | 典型耗时（Zero2W 现状） | 能压到 | 常驻内存 |
|---|---|---|---|
| BROM + SPL/ATF | 0.6 – 1.2 s | 0.4 s（DRAM 训练表裁剪） | — |
| **U-Boot** | **1.5 – 3.0 s**（官方镜像默认很肥） | **0.4 – 0.7 s** | — |
| **Kernel**（解压+probe+挂 root） | 1.5 – 4.0 s | **0.5 – 1.2 s** | 内核自身 12 – 25 MB |
| **init/systemd → 网络就绪** | **8 – 30 s** | **1.5 – 4 s** | systemd+udev 30 – 90 MB |

结论：**光精简内核，收益大约只占 20%；剩下 80% 在 U-Boot 配置和 rootfs/systemd 上。**
本文档第 1–2 节是内核（已做进 defconfig），第 3–6 节是你必须自己改的部分。

---

## 1. 内核层：`zero2w-minimal_defconfig` 已经做的事

生成方式（可反复重跑，改 `configs/zero2w-minimal.spec` 即可）：

```bash
bash scripts/wsl-10-minimize-config.sh     # 生成 configs/zero2w-minimal_defconfig + .config
VARIANT=minimal bash scripts/wsl-11-build-minimal.sh   # 或 VARIANT=tiny
bash scripts/wsl-13-verify-minimal.sh           # 严格校验: 板载必须 =y, 无关必须 n
bash scripts/wsl-12-finalize-minimal.sh           # gzip + 同步到 Windows build-out-*
BOARD=root@<IP> VARIANT=minimal bash scripts/deploy-to-board.sh   # 上板并实测
```

配置总量：**4877 项 → 2557 项（-48%）**，其中 `=m` 从 2910 → 897（模块 .ko 2907 → 878）；tiny 变体 `=m` 直接归零。
板载设备驱动**全部改成 `=y` 内建**（不再需要 modules 才能起网卡/Wi-Fi/显示/音频）。

### 1.1 关掉的部分（与 Zero2W 无关）
| 类别 | 手法 | 说明 |
|---|---|---|
| 其它 SoC 平台 | `!OFF ROCKCHIP_ MESON_ IMX_ QCOM_ TI_ MVEBU_ TEGRA_ EXYNOS_ …` | 整棵树里 2000+ 个无关驱动 |
| 无线网卡厂商 | `!OFF RTLWIFI_ MT76_ ATH_ RTW_ WILINK_ …`，只留 `BRCMFMAC_SDIO` | 板上是 BCM43455 |
| 显卡 | `!OFF DRM_`，只重开 `DRM_SUN4I`/`DRM_DW_HDMI`/`DRM_PANFROST` | 干掉 i915/amdgpu/vkms 等几十种 |
| 声卡 | `!OFF SND_`，只重开 `SND_SUN50IW9_CODEC`/`AW8738`/HDMI audio | |
| 传感器/HID/输入 | `!OFF SENSORS_ HID_ INPUT_TABLET INPUT_JOYSTICK RMI4_ KEYBOARD_ MOUSE_ TOUCHSCREEN_` | |
| 其它总线 | `!OFF MTD_ MMC_ SPI_ I2C_ PINCTRL_ CLK_ PHY_ REGULATOR_ MFD_ RTC_DRV_ GPIO_` 后按需白名单重开 | |
| **CAN 总线** | `CAN=n CAN_RAW=n`（自检里强制验证为 n） | 你明确不要 |
| 老式网络 | `!OFF PPP_ L2TP_ ATM_ AX25_ HAMRADIO_ ISDN_ WIMAX_ IEEE802154_` | |
| 虚拟化和固件 | `VIRTIO_ XEN_ HYPERV_ EFI=0 ACPI=0`（arm64 上这两套完全用不到） | |
| 其它文件系统 | `!OFF F2FS_ JFFS2_ UBIFS_ HFS_ REISERFS_ NILFS2_ OCFS2_ …`，只留 ext4/vfat/tmpfs | |

### 1.2 省内存的开关（真正减 `MemTotal` / 常驻 RSS）
| 选项 | 值 | 收益（估计） | 代价 |
|---|---|---|---|
| **`CMA_SIZE_MBYTES`** | **128 → 16** | **省 112 MB 可用内存**（1GB 板上就是 11%） | VPU/HDMI 大缓冲不够时分配失败 |
| `CMA_AREAS` | 7 → 2 | 少几 MB 记账 | — |
| `ION=n`（staging） | n | 省几 MB 无用保留池 | 老的多媒体用户态若用 /dev/ion 会失败 |
| `MEMCG=n` `CPUSETS=n` `BLK_CGROUP=n` | n | 每 cgroup 少一层控制器；systemd 不再建 memory cgroup | **Docker/LXC/Podman 不可用** |
| `SLUB_DEBUG=n` `SLAB_FREELIST_*=n` | n | 省数 MB slab 开销 + 一点 CPU | 失去 slab 调试/加固 |
| `HZ=100`（原 1000） | 100 | 定时器/时钟中断开销↓，空闲功耗↓ | 输入/多媒体时延粒度 10ms |
| `NR_CPUS=4`（原 8/16） | 4 | 每 per-CPU 结构×4 而非×N | — |
| `LOG_BUF_SHIFT=15`（原 16/17） | 32 KB | 省 128 KB+ 内核日志环 | 长启动日志会被挤掉 |
| `KALLSYMS_ALL=n` | n | 省 ~1.5 MB 常驻符号表 | 只能解析内核文本符号 |
| `MODULE_SIG=n` + `X509/PKCS7/ASN1=n` | n | 省 ~200 KB 代码 + 证书解析栈 | 不能校验模块签名 |
| `DEBUG_INFO/FTRACE/KPROBES/PERF/UBSAN/KCOV=n` | n | Image 明显变小，无 tracepoint 开销 | **不能做 ftrace/perf 调优**（要调时临时打开） |
| `SUSPEND/HIBERNATION/PM_SLEEP=n` | n | 省代码 + 省 suspend 相关 per-cpu | 不能挂起到 RAM/磁盘 |
| `BASE_SMALL=1` | 1 | 关掉 sysctl 表里用不到的项 | 少数 /proc/sys 项消失 |
| `CC_OPTIMIZE_FOR_SIZE=y`（-Os） | y | Image 小 8–15% | CPU 密集路径慢 2–5%（本设备不敏感） |
| `BLK_DEV_INITRD=n` | n | **省 20–40 MB**（Debian initrd 解在 RAM 里的 tmpfs）+ 省 0.3–0.8 s | rootfs 若依赖 initrd（LUKS/多分区/网络根）会起不来 |
| `ZRAM/KSM/SPLIT_PTE=n` | n | 省结构开销 | 内存吃紧时没有额外压缩交换 |

### 1.3 省启动时间的开关
| 选项 | 作用 |
|---|---|
| `fw_devlink=off`（**命令行**，不是 Kconfig） | 不再"等父设备就绪才 probe"，避免 regulator/clock 依赖链串行等待（常见几百 ms）。⚠️ 这棵 6.1 BSP 树里**没有** `CONFIG_FW_DEVLINK` 符号（`CONFIG_FW_DEVLINK=n` 写了也无效，已被我踩过），`FW_DEVLINK` 可配是 6.3+ 才有的 |
| `MODULES=n`（tiny 变体） | 完全不做模块加载/`modules.builtin.modinfo`，省 0.3–1 s 与 kmod 内存 |
| `WATCHDOG_NOWAYOUT=n` | 否则复位/重启要等看门狗超时 |
| `PANIC_TIMEOUT=5` | 内核 panic 时 5 s 自动重启，产品化需要 |
| `DEFAULT_IOSCHED="mq-deadline"` `IOSCHED_BFQ=n` | 少一个调度器的初始化与内存 |
| `CONFIG_CMDLINE="earlyprintk console=ttyS0,115200 loglevel=4 rootwait"`（`CMDLINE_FORCE=n`） | 出厂兜底；U-Boot 的 `bootargs` 仍然优先 |

### 1.4 有意**保留**的东西（不是漏砍）
| 保留项 | 原因 |
|---|---|
| `NETFILTER/NF_NAT/NF_CONNTRACK/NFT_MASQ/iptables/nftables` | CarPlay 要 `192.168.2.0/24` NAT + `172.16.42.0/24` 热点，砍了就废 |
| `BRIDGE` `VLAN_8021Q` | USB-RNDIS ↔ Wi-Fi 桥接 |
| `IP_MROUTE` `IP_MROUTE_MULTIPLE_TABLES` `IP_PIMSM_V2` `IPV6_MROUTE` `BRIDGE_IGMP_SNOOPING` | CarPlay 全程靠 **mDNS 组播（UDP 5353）**，跨接口组播路由没有就打不开连接 |
| `IP_ADVANCED_ROUTER` `IP_MULTIPLE_TABLES` `NFT_FIB` | 策略路由（多网卡场景必需） |
| `CRYPTO_*` 全套内建 | WPA2/WPA3、openssh、brcmfmac CCMP |
| `NAMESPACES=y`（只关 `USER_NS`） | systemd 需要；关过头会导致 systemd 起不来 |
| `CGROUPS=y`（只关 memcg/cpuset/blkio 控制器） | systemd 需要 cgroup2 挂载 |
| `STAGING=y` + `STAGING_MEDIA=y` + `VIDEO_SUNXI=y` | **Cedrus VPU 驱动在 `drivers/staging/media/sunxi/cedrus/` 下**，关 STAGING 会连它一起消失（已实测踩过） |
| `COMPAT`（32 位支持） | Orange Pi 官方 Debian/Ubuntu 镜像带 armhf 多架构，关掉直接起不来 |
| `EXT4`+`VFAT`+`NLS_utf8/936` | 挂载 SD 卡上的 CarPlay 媒体文件 |

---

## 2. 内存：怎么再往下抠 100 MB（按收益排序）

### 2.1 CMA 的三种改法（不用重编内核就能试）
```bash
# a) 内核命令行（最快，重启即生效）—— 改 U-Boot 的 bootargs
cma=16M              # 直接指定全局 CMA 大小
# b) 设备树（推荐，随板子走）
#    在板级 dts 里加一个 reusable 节点，这样这块内存"平时可被普通页使用"，
#    只在需要时才迁移走 —— 这才是真正不丢 MemTotal 的写法：
/ {
    reserved-memory {
        #address-cells = <2>; #size-cells = <2>; ranges;
        linux,cma {
            compatible = "shared-dma-pool";
            reusable;                    /* 关键：算进 MemTotal */
            size = <0 0x1000000>;        /* 16 MiB */
            linux,cma-default;
        };
    };
};
# c) Kconfig: CONFIG_CMA_SIZE_MBYTES=16   （已做进 minimal defconfig）
```
判断值不够/太多的方法：
```bash
grep -E "CmaTotal|CmaFree|MemTotal|MemAvailable" /proc/meminfo
dmesg | grep -i -E "cma|reserved"        # 看 "DMA: preallocated ... KiB pool"
```
- 只用 Wi-Fi/以太网/音频/GPIO：**`cma=0`** 都行
- 要 HDMI 输出 1080p（DRM framebuffer 约 8 MB）：**`cma=16M`**
- 要用 Cedrus 硬解 1080p/4K：**`cma=64M`~`cma=128M`**
> 注意：H616/H618 的 VE 有独立 IOMMU（`SUN50I_IOMMU`），也可以走 scatter-gather，不一定要大 CMA。

### 2.2 如果不需要 HDMI 显示和 GPU（CarPlay 主机场景大概率不需要）
在 `configs/zero2w-minimal.spec` 里追加，然后重跑 wsl-10 + wsl-11：
```
DRM=n
DRM_SUN4I=n
DRM_PANFROST=n
VIDEO_SUNXI_CEDRUS=n
VT=n
FRAMEBUFFER_CONSOLE=n
DRM_FBDEV_EMULATION=n
SOUND=n
```
收益：Image 再小 3–5 MB，运行时再省 10–25 MB（framebuffer + GPU 页表 + CMA），启动再快 0.3–1 s（HDMI 拓扑探测很慢）。

### 2.3 内存占用怎么看（别看 `free` 就完事）
```bash
cat /proc/meminfo | grep -E "MemTotal|MemFree|MemAvailable|Buffers|^Cached|Slab|SReclaimable|SUnreclaim|KernelStack|PageTables|CmaTotal|VmallocUsed"
# 内核自身的账
grep -E "^(Slab|KernelStack|PageTables|Percpu|VmallocUsed)" /proc/meminfo
# 谁在吃内存（按进程）
ps -eo rss,comm --sort=-rss | head -20
# slab 明细（SLUB_DEBUG 关掉后 /proc/slabinfo 不可读，需要临时开 CONFIG_SLUB_DEBUG）
cat /proc/slabinfo | sort -k3 -n | tail -15
```
H618 + 4 核 + minimal 内核，`MemTotal` 之外内核自身常驻应控制在 **12–18 MB**
（`Slab+SUnreclaim+KernelStack+PageTables+Percpu` 之和）。

---

## 3. 内核命令行（启动速度的第二刀）

推荐最终串（写进 U-Boot 的 `bootargs`，`zero2w-minimal_defconfig` 里内置的那条只在 bootargs 缺失时兜底）：

```
earlyprintk console=ttyS0,115200 loglevel=0 quiet
root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw
rootflags=data=writeback,noatime,nodiratime,errors=remount-ro
fw_devlink=off
cma=16M
psi=0
audit=0
printk.devkmsg=off
systemd.show_status=0 systemd.log_level=err log_buf_len=128K
initcall_debug  ← 只在测量时加
```
| 参数 | 省什么 | 备注 |
|---|---|---|
| `quiet loglevel=0` | **串口 115200 打印本身要 0.5–2 s** | 收益极大；调试时换回 `loglevel=7` |
| `rootflags=…noatime` | 每次读文件不再写 atime，SD/eMMC 上省下大量小 IO | |
| `data=writeback` | 元数据/数据顺序等待减少，挂载与首写更快 | 掉电可能丢未刷数据 |
| `rootwait` | 必需（SD 卡控制器就绪前 root 不存在），但**别写 `rootdelay=5`**，那是白等 | |
| `cma=16M` | 见 §2.1 | |
| `fw_devlink=off` | 省 driver_probe 的串行等待（几百 ms）；若某些驱动 probe 顺序变敏感导致偶发不起，去掉它即可 | |
| `printk.devkmsg=off` `audit=0` `psi=0` | 省内核日志缓冲/审计栈 | 需要 audit 的用户态再开 |
| `systemd.show_status=0 systemd.log_level=err` | 再省一段串口时间 | |
| `initcall_debug` | 不省时间，是用来**量**每个驱动 probe 花了多久（配 `systemd-analyze critical-chain`） | 量完务必去掉，它自己就拖慢 |

---

## 4. U-Boot 层：收益最大的地方

Zero2W 官方镜像的 U-Boot 是"开发版"配置，为通用性牺牲了大量启动时间。
下面是**改 `configs/orangepi_zero2w_defconfig`（或 `make menuconfig`）** 的具体项：

### 4.1 必改（收益 1–2 s）
```
CONFIG_BOOTDELAY=0                 # 原 2，直接省 2 s
# CONFIG_AUTOBOOT_KEYED is not set
CONFIG_USE_BOOTCOMMAND=y
CONFIG_BOOTCOMMAND="ext4load mmc 0:1 0x48000000 /boot/Image.gz; ext4load mmc 0:1 0x4a000000 /boot/zero2w.dtb; ext4load mmc 0:1 0x50000000 /boot/boot.scr; source 0x50000000"
                                   # 只试一次介质，别 pxelinux/usb/dhcp 全试一遍
# CONFIG_NET is not set            # 省掉 ETH 初始化 + DHCP 超时，H618 EMAC 在 u-boot 里很慢
# CONFIG_CMD_DHCP is not set
# CONFIG_CMD_PING is not set
# CONFIG_CMD_NET is not set
# CONFIG_CMD_USB is not set        # u-boot 里 usb start 扫总线要 0.5-1 s
# CONFIG_CMD_MTD is not set        # 需要再开
# CONFIG_DFU is not set
# CONFIG_CMD_DFU is not set
# CONFIG_FASTBOOT is not set
# CONFIG_EFI_LOADER is not set     # 省 ~200 KB 且避免扫描 ESP
# CONFIG_CMD_BOOTEFI is not set
# CONFIG_PYTHON is not set
# CONFIG_CMD_SETEXPR is not set    # boot.scr 若用到就保留
# CONFIG_LOG is not set
# CONFIG_TRACE? 保留，用它量时间(见 4.3)
CONFIG_HUSH_PARSER=y               # boot.scr 需要，别关
CONFIG_SYS_LONGHELP=n
CONFIG_VERSION_VARIABLE=n
CONFIG_BUILD_TARGET="..."
```

### 4.2 不要 HDMI/显示输出时（再省 0.3–0.8 s）
```
# CONFIG_VIDEO is not set
# CONFIG_VIDEO_DT_SIMPLEFB is not set
# CONFIG_SPLASH_SCREEN is not set
# CONFIG_CMD_BMP is not set
# CONFIG_BACKLIGHT is not set
# CONFIG_DM_PWM is not set          # 若 backlight/pwm 无其它用户
```
> U-Boot 里开视频驱动除了慢，还会占一大块 RAM 做 framebuffer；关掉后内核侧若也 `DRM=n`
> 就完全不显示（CarPlay 头单元的画面走 CarPlay 协议，不需要本机 HDMI）。

### 4.3 用 trace 量 U-Boot 自己（别猜）
```
=> trace on
=> run bootcmd
=> trace stats        # 每个 driver 的 probe 时间与调用次数
=> trace close
```
配合 `CONFIG_BOOTSTAGE`/`CONFIG_BOOTSTAGE_REPORT`，`=> bootstage report` 直接给分段耗时。

### 4.4 SPL/DRAM 训练
H616/H618 的 DRAM 参数（`dram_tpr_config`/`sun50i_h616_dram`）第一次上电要跑训练，几百 ms。
确定内存颗粒后把训练结果固化进 `struct dram_para`（`dram_clockt`/`dram_tpr0..10`），跳过训练。
主线 U-Boot 的 `dram_sun50i_h616.c` 支持 `dram_para` 从设备树/`auto_dram_para` 读取。

---

## 5. rootfs 层：8–30 s 的大头在这里

### 5.1 选基础镜像
| 方案 | 空载 RSS | 启动到 shell | 备注 |
|---|---|---|---|
| Orange Pi 官方 Debian 12（GNOME/Server） | 150–300 MB | 15–30 s | 太多服务，别直接用 |
| Debian 12 `debos/debootstrap minbase` + systemd-networkd | 45–60 MB | 3–6 s | **推荐，可维护** |
| Alpine / Buildroot musl + busybox init | 12–25 MB | **1.0–2.0 s** | 最省，但要自己维护交叉编译链 |
| Buildroot + systemd | 30–45 MB | 2–4 s | 平衡 |

```bash
# Debian minbase（在 WSL 里，走本地代理）
sudo debootstrap --variant=minbase --foreign --arch=arm64 bookworm root http://deb.debian.org/debian
```

### 5.2 systemd 裁剪清单（收益 3–15 s，10–40 MB）
```bash
# 一次性关掉这台板子上永远用不到的东西
systemctl mask \
  systemd-udev-settle.service \
  lvm2-monitor.service mdmonitor.service e2scrub_reap.service \
  systemd-timesyncd.service apt-daily.timer apt-daily-upgrade.timer \
  dpkg-db-backup.timer fstrim.timer man-db.timer \
  ModemManager.service sshd.service console-getty.service \
  getty@tty1.service brltty.service bluetooth.service wpa_supplicant.service
# ↑ bluetooth/wpa 按 CarPlay 需要留着就别 mask；这里是举例
systemctl disable systemd-networkd-wait-online.service
```

`/etc/systemd/system.conf`（每一项都是实打实的等待时间）：
```ini
[Manager]
DefaultTimeoutStartSec=5s
DefaultTimeoutStopSec=5s
DefaultDeviceTimeoutSec=5s
RuntimeWatchdogSec=0
WatchdogDevice=
JournalMode=auto
ShowStatus=0
LogLevel=err
ConfirmSpawnService=0
```
`/etc/systemd/journald.conf`：
```ini
Storage=volatile
RuntimeMaxUse=8M
ForwardToSyslog=no
MaxRetentionSec=1day
SyncIntervalSec=5m
```
udev 侧：
- 删掉 `/lib/udev/rules.d/` 里 `60-cdrom_id`、`60-persistent-storage-tape`、`70-*` 无关规则
- `udevadm control --log-level=err`
- 内核已关 `CONFIG_UEVENT_HELPER`，不要再设 `/proc/sys/kernel/hotplug`

fstab：**每条不确定的挂载都加 `nofail,x-systemd.device-timeout=1s`**，
`/boot` 那一条如果开机不需要写就别挂（一次 fsck 就是几秒）。

干掉官方镜像的首启动拖慢项：
```bash
rm -f /etc/systemd/system/multi-user.target.wants/orangepi-*.service   # 官方 resize2fs / 换源 / 生成 ssh key
rm -f /var/lib/cloud/seed -r
ssh-keygen -A && systemctl disable ssh keygen oneshot   # 预生成，别开机生成
```

### 5.3 网络栈换轻量实现
- `systemd-networkd` + `systemd-resolved`(可关) 替代 `NetworkManager`（后者常驻 +40 MB、启动 +3 s）
- CarPlay 需要：`dnsmasq`（DHCP+DNS，常驻 ~2 MB）、`avahi-daemon`（mDNS；启用 `enable-dbus=no` 省 6 MB）
- USB 侧走 `configfs` 手工脚本，别用 `usb-modeswitch`/`NetworkManager` 抢设备
- 别装 `firewalld`（它 + Python + dbus，常驻 30 MB+）；直接 `nft`/`iptables-nft` 下发规则

### 5.4 极省内存的取舍
- **不要 swap/zram**：1 GB 以上不需要；SD 卡上 swap 会磨损且不降低峰值 RSS
- 想要更强"内存不足不死机"能力 → 打开 `CONFIG_ZRAM=y CONFIG_ZSTD_COMPRESS=y CONFIG_ZSTD_DECOMPRESS=y`（minimal 里目前是关的）
- `vm.vfs_cache_pressure=500 vm.min_free_kbytes=1024 vm.dirty_ratio=20 vm.dirty_background_ratio=5`
  （`min_free_kbytes` 默认在 1GB 机器上算出 64 MB 左右，SD 卡设备可下调；但 CMA/网络抖动需要一定水位，别调到 <1024）
- `echo 3 > /proc/sys/vm/drop_caches` 不要常驻脚本，纯浪费 CPU 冷缓存

---

## 6. 度量：改动必须用数字说话

```bash
# 分段耗时
systemd-analyze                      # = kernel + userspace
systemd-analyze blame | head -20     # 谁最慢
systemd-analyze critical-chain ssh.service
systemd-analyze time

# 内核阶段每个驱动 probe 耗时（要 CONFIG_KALLSYMS=y，minimal 里已保留）
# cmdline 临时加 initcall_debug loglevel=7
dmesg -wH | grep -E "initcall|took"
# 或者用 python 脚本汇总 /var/log/dmesg

# 内存
grep -E "MemTotal|MemAvailable|Slab|SUnreclaim|KernelStack|PageTables|Cma" /proc/meminfo
ps -eo rss,comm --sort=-rss | head -15

# 端到端上电计时（串口抓 "Linux version" 与 "Reached target Multi-User System"）
```

---

## 7. 实测结果（2026-09-04 · WSL2 Debian 13 · gcc 14.2 · 同一棵 6.1.31 内核树）

| 指标 | full（板载全开+通用驱动） | **minimal**（推荐起点） | tiny（minimal + `MODULES=n`） |
|---|---|---|---|
| 内核版本串 | 6.1.31-zero2w | **6.1.31-opi-min** | **6.1.31-opi-tiny** |
| `Image` | 27,856,904 B = 26.6 MiB | 17,948,680 B = **17.1 MiB（-35.6%）** | 16,957,448 B = **16.2 MiB（-39.1%）** |
| `Image.gz`（SD 实际读取量） | 未压 | 7,502,511 B = 7.3 MiB | 7,194,789 B = 7.1 MiB |
| 配置项 =y / =m | 2118 / 2785 | 1660 / 897 | 1518 / **0** |
| 配置总项数 | 4903 | 2557（**-48%**） | ~1518 |
| 模块数 | 2907 个（132 MB） | 878 个（64 MB） | **0 个** |
| `CMA_SIZE_MBYTES` | 128 | **16**（省 112 MB 可用内存） | **16** |
| `BASE_SMALL` / `MEDIA_SUBDRV_AUTOSELECT` | 0 / y | **1 / n** | 1 / n |
| `LOCKDOWN`→`MODULE_SIG`→X509/PKCS7 | y | **全 n** | 全 n |
| CAN / CAN_RAW | y | **n** | **n** |
| 板载驱动自检 | 全 y/m | **全 y/m ✅** | 全 y ✅（符号级复核见下） |

`System.map` 符号级复核（tiny）：`brcmf 443 · dw_hdmi 110 · panfrost 121 · cedrus 111 ·
ac200 30 · sunxi_musb 34 · mv64xxx 23 · sun4i_spi 17 · sunxi_sid 7 · aw8738 10 ·
sun8i_ths 4 · sunxi_gmac 1` → 驱动确实链进内核，不是"只写了配置"。

产物：WSL `~/opi/out-minimal/kernel/`、`~/opi/out-tiny/kernel/`；
Windows `build-out-minimal/kernel/`、`build-out-tiny/kernel/`（`Image` + `Image.gz` + `System.map` +
`kernel.config` + `dtb/sun50i-h618-orangepi-zero2w.dtb`）。逐条裁剪说明见
**[BUILD-REPORT.md](BUILD-REPORT.md) 第 7 节**。

> `=y` 只比 full 少 400，但 `=m` 少了 1500+ —— 因为**板载驱动全部从 =m 提到 =y**（开机不依赖模块），
> 无关驱动整体删除。真正的指标是 `Image` 体积、模块数量和 `CMA`。

---

## 8. 回退清单（万一砍过头）

按现象直接定位到要恢复的选项，改 `configs/zero2w-minimal.spec` → 重跑 wsl-10 → wsl-11：

| 现象 | 恢复 |
|---|---|
| 起不来 / init 报 `No such file` 且 rootfs 需要 initrd | `BLK_DEV_INITRD=y RD_GZIP=y` |
| 32 位 armhf 二进制跑不了（官方镜像带多架构） | `COMPAT=y`（minimal 目前未关，别手动关） |
| Docker/Podman/LXC 起不来 | `MEMCG=y CPUSETS=y BLK_CGROUP=y CGROUP_BPF=y`（+`BPF_SYSCALL=y`） |
| systemd 报 `Failed to mount … bpf` 或某些 socket 激活异常 | `BPF_SYSCALL=y`（代价 ~200 KB） |
| 视频硬解/播放申请不到 DMA 内存 | `CMA_SIZE_MBYTES=64` 或 cmdline `cma=64M` |
| `/dev/ion` 打不开（老的多媒体库） | `ION=y STAGINGION=y` |
| 加 USB 网卡/U 盘之外的设备失败 | 走完整 `zero2w-full.config`（`VARIANT=full wsl-03-build-kernel.sh`） |
| Wi-Fi 需要 mesh / P2P 特殊路径 | `MAC80211_MESH=y` |
| 需要 NFS 挂载做开发 | `NFS_V3=y` |
| 想再省 ~40 KB（板上确实没有 IIO 设备） | spec 里取消 `# IIO=n` 的注释，重跑 wsl-10+11（`IIO_BUFFER` 是 default y 且被 select，光写 =n 关不掉） |
| 红外遥控不响应 | `RC_DEVICES=y RC_DECODERS=y`（`!OFF RC_` 会连菜单开关一起关掉） |
| 想 perf/ftrace 调优 | `PERF_EVENTS=y FTRACE=y FUNCTION_TRACER=y`（镜像会明显变大） |

---

*生成时间：2026-09-04 · 内核：Allwinner BSP `linux-orangepi-6.1-sun50iw9`（6.1.31）· 交叉：`aarch64-linux-gnu-gcc 14.2` · 构建环境：WSL2 Debian 13*
