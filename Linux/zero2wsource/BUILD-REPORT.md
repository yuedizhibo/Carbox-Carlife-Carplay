# Zero2W 定制内核 —— 首次构建报告

时间：2026-09-04 · 构建机：WSL2 Debian 13 (trixie)，内核 6.6.114.1-microsoft-standard-WSL2，20 核 / 23GB
结果：**内核 + dtb + 模块 + ATF + 主线 u-boot 全部编译通过**

## 1. 产物

| 文件 | 大小 | 说明 |
|---|---|---|
| `~/opi/out/kernel/Image` | 27.9 MB | `6.1.31-zero2w`，ARM64 boot executable，4K pages |
| `~/opi/out/kernel/dtb/sun50i-h618-orangepi-zero2w.dtb` | 40.6 KB | 板级设备树 |
| `~/opi/out/zero2w-modules.tar.gz` | — | 2907 个模块（BSP defconfig 带了很多无关驱动，后续可裁剪） |
| `~/opi/out/kernel/kernel.config` | 203 KB | 最终生效配置 |
| `~/opi/out/u-boot/bl31.bin` | 49 KB | ATF `PLAT=sun50i_h616` DEBUG=1 |
| `~/opi/out/u-boot-mainline/u-boot-sunxi-with-spl.bin` | 879 KB | 主线 u-boot（binman 已把 SPL+ATF+u-boot 打包好） |

> WSL 路径在 Windows 侧：`\\wsl$\Debian\home\dfkvm\opi\out\`

## 2. 板载硬件驱动落地情况（全部 built-in，不依赖 initrd）

| 硬件 | CONFIG | 状态 |
|---|---|---|
| Wi-Fi AP6256 (BCM43455) | `BRCMFMAC=y` `BRCMFMAC_SDIO=y` `CFG80211=y` `MAC80211=y` | ✅ |
| 蓝牙 | `BT_BCM=y` `BT_HCIUART_BCM=y` `SERIAL_DEV_BUS=y` | ✅ |
| 100M 网口 (AC200 ePHY) | `MFD_AC200=y` `SUNXI_GMAC=y` `MDIO_SUN4I=y` `SUN4I_EMAC=y` | ✅ |
| GPIO 40Pin/24Pin | `PINCTRL_SUN50I_H616{,_R}=y` `GPIO_SYSFS=y` `GPIO_CDEV=y` `OF_GPIO=y` | ✅ |
| LED / 按键 / 红外 | `LEDS_GPIO=y` `KEYBOARD_GPIO=y` `KEYBOARD_SUN4I_LRADC=y` `IR_SUNXI=y` | ✅ |
| TF 卡 / SDIO | `MMC_SUNXI=y` | ✅ |
| 16MB SPI NOR | `MTD_SPI_NOR=y` `SPI_SUN4I=y` | ✅ |
| HDMI 显示 | `DRM_SUN4I=y` `DRM_SUN8I_DW_HDMI=y` `SUN50I_DE2_BUS=y` | ✅ |
| GPU Mali-G31 | `DRM_PANFROST=y` | ✅ |
| VPU 硬解 | `VIDEO_SUNXI_CEDRUS=y`（全志 cedar；主线 hantro 在本树不适用） | ✅ |
| 音频 codec + 功放 | `SND_SUN50IW9_CODEC=y` `SND_SUN50I_CODEC_ANALOG=y` `SND_SOC_AW8738=y` `SND_USB_AUDIO=y` | ✅ |
| PMIC AXP313 | `SUNXI_RSB=y` `MFD_AXP20X{,_RSB}=y` `REGULATOR_AXP20X=y` | ✅ |
| RTC / 温度 / 调频 / eFuse | `RTC_DRV_SUN6I=y` `SUN8I_THERMAL=y` `ARM_ALLWINNER_SUN50I_CPUFREQ_NVMEM=y` `NVMEM_SUNXI_SID=y` | ✅ |
| USB host / gadget(CarPlay) | `USB_EHCI_HCD_PLATFORM=y` `USB_MUSB_SUNXI=y` `USB_CONFIGFS_F_FS=y` | ✅ |
| 用户态总线接口 | `I2C_CHARDEV=y` `SPI_SPIDEV=y` `I2C_MV64XXX=y` | ✅ |
| DT overlay | `OF_OVERLAY=y`（改外设不用重编内核） | ✅ |
| 硬件加密 | `CRYPTO_DEV_SUN4I_SS=m` | ✅（模块） |
| PWM | `PWM_SUN4I=m` | ✅（模块） |
| CAN | ❌ 本 BSP 的 `sun4i_can` 只支持 A10/A20；H616 CAN 需全志 SDK 的 `sunxi-can` 驱动 | 待办 |

dtb 反编译核对（`dtc -I dtb -O dts`）确认存在：`xunlong,orangepi-zero2w`、`mmc@4020000`(TF)、`mmc@4021000`(SDIO Wi-Fi)、`mmc-pwrseq-simple`、`ethernet@5030000`、`x-powers,ac200`、`ac200-ephy`、`pmic@36`、`ir@`、`codec`、`gpu@1800000`、`hdmi`、`jedec,spi-nor`、`lradc`、`gpio-leds`、`sun50i-h616-rtc`。

## 3. 踩过的坑（都已解决，脚本里已固化）

| 现象 | 原因 | 解决 |
|---|---|---|
| `Kconfig: warning: ignoring unsupported character` + `missing end statement` | Windows 侧 git 检出是 **CRLF**，kconfig 解析器不吃 `\r` | 编译用源码必须在 Linux 里取（`scripts/wsl-04-prepare-lf.sh` / tarball 直取）；Windows 那份只做归档 |
| `mkimage: not found` | 缺 u-boot-tools（overlay 的 `fixup.scr` 需要） | `apt install u-boot-tools` |
| `sun50i-cpufreq-nvmem.c: error: assignment ... -Wint-conversion` | GCC 14 把 `int-conversion` 等升级为硬错误，全志老驱动中招 | `KCFLAGS="-Wno-error=int-conversion -Wno-error=implicit-function-declaration ..."` |
| Allwinner `u-boot-orangepi-v2024.01` 编译失败：`SWIG_Python_AppendOutput too few arguments` | 该 fork 开 `BINMAN`→`DTOC`→`PYLIBFDT`，与 swig 4.3 不兼容 | 改用**主线 u-boot**（已编好）；要用官方 fork 则需自行编 swig 4.1 |
| 主线 u-boot `binman: faked external blobs bl31.bin` | 需要 ATF | 把 `bl31.bin` 放到 u-boot 源码根目录（脚本已自动处理） |
| `mkeficapsule.c: gnutls.h not found` | 缺 `libgnutls28-dev` | `apt install libgnutls28-dev` |
| 板载网卡驱动找错 | Zero2W 的 `emac1` compatible 是 **`allwinner,sunxi-gmac`**（不是主线 `sun4i-emac`/`dwmac-sun8i`） | 配置里 `SUNXI_GMAC=y` |
| RTC 找不到 | H616 的 `allwinner,sun50i-h616-rtc` 由 **`RTC_DRV_SUN6I`** 匹配（不是 `RTC_DRV_SUNXI`） | 已修正 |

## 4. 复现步骤

```bash
# WSL 内，一次到位
bash /mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource/scripts/wsl-01-setup-env.sh     # root
bash .../scripts/wsl-04-prepare-lf.sh        # 取 LF 源码（内核走 tarball，抗断连）
bash .../scripts/wsl-05-get-kernel-tarball.sh
bash .../scripts/wsl-03-build-kernel.sh      # 内核 + dtb + 模块
bash .../scripts/wsl-06-build-bootloader.sh  # ATF bl31（u-boot fork 会因 swig 失败）
bash .../scripts/wsl-08-build-uboot-mainline.sh   # 主线 u-boot（推荐，已验证 OK）
bash .../scripts/wsl-07-verify.sh            # 产物与驱动核对
```

## 5. 部署到板子

```bash
# 板子先烧官方镜像（Debian12/Ubuntu22）并联网，拿到 IP
BOARD=root@<板子IP> bash .../scripts/deploy-to-board.sh
```
脚本会：装模块+AP6256 固件 → 备份并替换 `/boot/Image` 与 dtb → 重启 → 自动打印 `uname -a`、网络接口、Wi-Fi 扫描、蓝牙、`gpioinfo`、`/dev/dri`、`/dev/snd`、温度与 cpufreq、dmesg 关键行。

## 6. 下一步建议

1. ~~裁剪 defconfig~~ **已完成**，见下面第 7 节（4877 项 → 2557 项 -48%，模块 2907 → 878 → 0，Image 26.6 → 16.2 MiB）。
2. **确认上电即用的部分**：`brcmfmac` 需要 `/lib/firmware/brcm/brcmfmac43455-sdio.{bin,txt}`；若 Wi-Fi 不识别，先 `dmesg | grep brcm` 看固件路径与 nvram 是否匹配（板载天线不同 → 换对应 `nvram_ap6256.txt`）。
3. **蓝牙**：`btbcm` 走 `ttyS1`，需要 `hciattach`/`bluetoothd`（官方镜像已带 `orangepi-wifi-bt` 服务）。
4. ~~CAN~~ 你已明确不要 CAN：minimal/tiny 里 `CAN=n`，wsl-10 的自检里强制验证它为 `n`（脚本会 FAIL）。
5. **想完全自主镜像**：用 `06-build-system-rootfs/orangepi-build`（`./build.sh -b orangepizero2w -k next`）把这套内核/uboot 直接打进 img。
## 7. 第二轮：极简（board-only）配置 —— 只留开发板上的设备

需求：**去掉 CAN，只留 Zero2W 板载设备，尽量压低运行内存和启动时间**。
做法不是手点 `menuconfig`，而是把裁剪规则写成可复审的清单
`configs/zero2w-minimal.spec`（141 条 `!OFF <前缀>` 批量关闭 + 900 余条精确设置），
由 `scripts/minimize_config.py` 施加到 `linux_sunxi64_defconfig` 之上，再 `olddefconfig` 收敛依赖。

```bash
bash    .../scripts/wsl-10-minimize-config.sh    # -> configs/zero2w-minimal_defconfig (+ .config + 自检)
VARIANT=minimal bash .../scripts/wsl-11-build-minimal.sh
VARIANT=tiny    bash .../scripts/wsl-11-build-minimal.sh
bash    .../scripts/wsl-12-finalize-minimal.sh   # gzip + 符号级验证 + 同步到 Windows
```

### 7.1 三个变体实测对比（同一棵 6.1.31 内核树、同一 toolchain）

| 指标 | full（第一轮，板载全开+通用驱动） | **minimal** | tiny（minimal + `MODULES=n`） |
|---|---|---|---|
| `Image` | 27,856,904 B（26.6 MiB） | 17,948,680 B（17.1 MiB）**-35.6%** | 16,957,448 B（16.2 MiB）**-39.1%** |
| `Image.gz`（SD 上真正要读的） | 未压 | 7,502,511 B（7.3 MiB） | 7,194,789 B（7.1 MiB） |
| 内核版本串 | 6.1.31-zero2w | **6.1.31-opi-min** | **6.1.31-opi-tiny** |
| 配置项 `=y` / `=m` | 2118 / 2785 | 1660 / 897 | 1518 / **0** |
| 配置总项数 | 4903 | 2557（**-48%**） | ~1518 |
| 模块 `.ko` | 2907 个 / 132 MB | 878 个 / 64 MB | **0 个** |
| `CONFIG_CMA_SIZE_MBYTES` | 128 | **16** | **16** |
| `BASE_SMALL` / `MEDIA_SUBDRV_AUTOSELECT` / `LOCKDOWN` | 0 / y / y | **1 / n / n** | 1 / n / n |
| `CAN` / `CAN_RAW` | y | **n** ✅ | **n** ✅ |
| 产物目录 | `~/opi/out/kernel` | `~/opi/out-minimal/kernel` | `~/opi/out-tiny/kernel` |
| Windows 侧 | `build-out/kernel` | `build-out-minimal/kernel` | `build-out-tiny/kernel` |

### 7.2 自检结果（每次生成配置都自动跑，不通过就 exit 1）
板载项全部为 `y`：`ARCH_SUNXI GPIO_SYSFS GPIO_CDEV OF_GPIO OF_OVERLAY PINCTRL_SUN50I_H616[_R]
SUNXI_CCU SUN50I_H616_CCU SUNXI_RSB NVMEM_SUNXI_SID SUNXI_WATCHDOG SUN8I_THERMAL I2C_MV64XXX
SPI_SUN4I SPI_SPIDEV IR_SUNXI LEDS_GPIO KEYBOARD_[GPIO|SUN4I_LRADC] MFD_AXP20X_RSB
REGULATOR_AXP20X REGULATOR_FIXED_VOLTAGE MFD_AC200 MMC_SUNXI MMC_BLOCK MTD MTD_SPI_NOR
MTD_OF_PARTS SPI_MEM DRM_SUN4I SUN50I_DE2_BUS DRM_SUN8I_DW_HDMI DRM_DW_HDMI DRM_PANFROST
VIDEO_SUNXI_CEDRUS SND_SUN50IW9_CODEC SND_SUN50I_CODEC_ANALOG SND_SOC_AW8738 SND_SOC_SUNXI_MACH
BRCMFMAC[_SDIO] MAC80211 CFG80211_WEXT BT_BCM BT_HCIUART_[BCM|SERDEV] SUNXI_GMAC SUN4I_EMAC
MDIO_SUN4I PHYLIB USB_EHCI_HCD_PLATFORM USB_MUSB_SUNXI USB_CONFIGFS_F_FS USB_F_FS
SERIAL_8250_CONSOLE RTC_DRV_SUN6I ARM_ALLWINNER_SUN50I_CPUFREQ_NVMEM CPU_FREQ THERMAL`
（`OF_OVERLAY=y` 保留 → DTBO 裁剪 pinmux 仍可用，见 GPIO.md 第 4 节）

`System.map` 符号级复核（证明不是只"配置开了"而是真的链进内核）：
`brcmf 443 · musb 220 · stmmac 218 · dw_hdmi 135 · panfrost 121 · cedrus 111 · axp20x 65 ·
hci_uart 42 · sunxi_musb 34 · ac200 30 · mv64xxx 23 · sun4i_spi 17 · sunxi_sid 7 · aw8738 10 ·
sun8i_ths 4 · gmac 129`

### 7.3 这一轮新踩的坑（都已固化进脚本）
| 坑 | 现象 | 结论 |
|---|---|---|
| **Cedrus 在 staging 下** | `STAGING=n` 后 `VIDEO_SUNXI_CEDRUS` 直接从 `.config` 消失 | 硬解驱动位于 `drivers/staging/media/sunxi/cedrus/`，必须 `STAGING=y` **且** `STAGING_MEDIA=y` **且** `VIDEO_SUNXI=y` 三层菜单都开 |
| **`MODULE_SIG` 关不掉** | 设 `MODULE_SIG=n` 后 olddefconfig 又变回 y，连带 PKCS7/X509/ASN1 全进来 | 链条是 `LOCKDOWN` → `SECURITY_LOCKDOWN_LSM` → `LOCK_DOWN_KERNEL_EFFECTIVE select MODULE_SIG`；必须 **`LOCKDOWN=n`** 才断得掉（arm64 + 自己的 u-boot 根本用不到 lockdown） |
| **`MODULES=n` 反而更大** | tiny 第一版 `=y` 从 1718 暴涨到 2872 | kconfig 在 `!MODULES` 时把 `.config` 里所有 `=m` **升级成 `=y`**。必须先把 `=m` 全部注释掉，再关 `MODULES` |
| `make modules` 直接失败 | `The present kernel configuration has modules disabled` + Error 1 | tiny 的构建目标只能是 `Image dtbs` |
| `size arch/arm64/boot/Image` | `file format not recognized` | arm64 的 `Image` 是 raw image，不是 ELF；量大小用 `stat`/`ls` |
| `savedefconfig` 省略默认值 | `CMA_SIZE_MBYTES=16` 在生成的 defconfig 里"看不到" | 因为它等于 Kconfig 默认值，属正常。核对要以**展开后的 `.config`** 为准（脚本已打印 `CMA=CONFIG_CMA_SIZE_MBYTES=16`） |
| 批量 `!OFF` 会误伤公共符号 | `MTD MMC I2C SPI PHYLIB MDIO_DEVICE GPIOLIB THERMAL REGULATOR RTC_CLASS INPUT LEDS MEDIA_* DRM_KMS_HELPER USB_GADGET` 全被连带关掉，驱动变 `n` | 驱动前缀和"公共依赖"同名（如 `!OFF MTD_` 会关掉 `MTD`），所以 spec 里必须先批量 `!OFF`、再精确重开公共符号，最后才 `olddefconfig` |

### 7.4 `tiny` 变体部署 rootfs 的注意事项（重要）
`MODULES=n` 之后内核不再支持加载模块，官方 rootfs 里 `/lib/modules/6.1.31-zero2w/` 那批 `.ko`
（brcmfmac、panfrost、cedrus…）**永远不会被加载**，但 udev 仍会尝试 modprobe 并报 `Unknown symbol`/
`already loaded` 一类噪声。所以：

```bash
# 板上执行：删掉旧模块树，避免和 built-in 驱动打架
mv /lib/modules /lib/modules.bak
# 官方镜像里开机自动加载模块的服务也可以关掉
systemctl disable systemd-modules-load.service 2>/dev/null || true
```
另外 `BLK_DEV_INITRD=n`：u-boot 就算把 `initrd` 加载进内存，内核也会直接忽略它 ——
**必须确认 rootfs 的 `/` 是 ext4 且 ext4 已内建**（minimal/tiny 里 `EXT4_FS=y`，成立）。
若你的镜像依赖 initrd（LUKS、RAID、`/usr` 独立分区），把这三行加进 spec 再重跑：
```
BLK_DEV_INITRD=y
RD_GZIP=y
```

### 7.5 还没做的（收益/风险比不划算，留给你按需开）
- `CONFIG_MODULE_SIG=n` 已生效，但 `DEBUG_FS` 被 DRM 等 `select` 回来了（省不了，无害）。
- `CONFIG_ARM64_64K_PAGES=y`：能再省几 MB `struct page`，但 Allwinner 的 Mali/多媒体 blob 用户态
  基本都按 4K 页编译，**强烈不建议**。
- `CONFIG_PREEMPT_NONE`（默认已是）：对 CarPlay 音频延迟敏感时可换 `PREEMPT_RT`，那是反向的取舍。

### 7.4 补充坑位（第二轮验证时又踩到的）
| 坑 | 结论 |
|---|---|
| `VARIANT=tiny` 第二次构建时优化"莫名丢失"（`BASE_SMALL` 又变 0） | 脚本会优先取 `configs/zero2w-tiny_defconfig`，而它是**上一轮**`savedefconfig` 的产物。改了 spec 之后必须先 `rm configs/zero2w-<variant>_defconfig` 再重建 |
| `CONFIG_FW_DEVLINK=n` 写了没用 | 这棵 6.1 BSP 树里根本没有 `config FW_DEVLINK` 符号（6.3+ 才有）。要关 fw_devlink 只能用内核命令行 `fw_devlink=off`，见 TUNING.md §3 |
| `CONFIG_BASE_SMALL=y` 设不上（一直 0） | `BASE_SMALL` 是**只读计算符号**：`default 1 if !BASE_FULL`。要省这份开销必须 `BASE_FULL=n` |
| 三个变体版本号相同会互相覆盖 `/lib/modules/6.1.31` | 每个变体注入独立 `CONFIG_LOCALVERSION`（`-opi-min` / `-opi-tiny`），脚本已做 |

### 7.5 第三轮（审计驱动）的坑：批量 `!OFF 前缀` 会把"菜单开关"一起干掉
这一轮加了一次**全量审计**（列出所有"应关未关"和"应开不见"的符号），结果抓到 3 个真实回归：

| 被误伤的符号 | 为什么被误伤 | 后果（如果不验证就会发现） |
|---|---|---|
| `MEDIA_PLATFORM_SUPPORT` | `!OFF MEDIA_PLATFORM`（想关别的平台 media）把它一起关了 | **Cedrus VPU + 所有 platform media 驱动从 `.config` 里彻底消失** |
| `RC_DEVICES` / `RC_DECODERS` | 为了关 `RC_MAP`/`RC_LOOPBACK` 用了 `!OFF RC_` | **板载红外接收头 `IR_SUNXI` 消失**（它在 `if RC_CORE → if RC_DEVICES` 里） |
| `W1`（1-Wire） | 被 `BATTERY_DS2780/2781/2782` 这些电源驱动 `select` 拉回 | 说明"关一个符号"要连它的 selector 一起关 |

**教训（已固化成脚本）**：
1. `!OFF <前缀>` 只能用于**厂商/芯片家族**这类不会有歧义的前缀；凡是 `MEDIA_/NET_/I2C_/SPI_/RC_` 这种
   同时命名了"菜单开关"的前缀，必须在 spec 里显式把菜单开关重新 `=y`。
2. `wsl-10` 的自检有个盲区：符号**从 `.config` 消失**（依赖不满足导致不可见）会被判成"本内核树无此符号"
   而**不算失败**。所以必须有第二道严格闸门：
   ```bash
   bash scripts/wsl-13-verify-minimal.sh      # 要求 101 个板载项必须字面 =y，少一个就 exit 1
   ```
   它的 `YES` 列表用 `grep -q "^CONFIG_X=y$"` 硬匹配，"消失"和"变 n"一样判 FAIL。
   现在流程是：`wsl-10`（宽松自检）→ `wsl-11`（构建）→ `wsl-13`（严格验证）→ `wsl-12`（打包同步）。

