# Orange Pi Zero 2W — Linux 内核 / 全硬件 SDK 源码仓库

> 抓取时间：2026-09-04 · 内容：**29 个 git 仓库 ≈ 8.7 GB**（39 万个文件）· 全部为 **浅克隆（--depth 1）** · 明细见 `STATUS.md` / `manifest.json`
> 目标：为后续 **自己定制香橙派 Zero 2W 的 Linux 内核 / 系统** 准备全部上游源码（BSP + 主线 + 构建系统 + 固件 + 硬件资料）

---

## ✅ 状态：已经构建成功（2026-09-04）· 已有 3 个内核变体：full / minimal / tiny

在 WSL2 Debian 13 里已经把 **Zero2W 定制内核 + 设备树 + 模块 + ATF + u-boot** 全部编出来，板载硬件驱动与 GPIO 全开：

| 产物 | 位置（WSL 内；Windows 侧 `\\wsl$\Debian\...`） |
|---|---|
| `Image` 6.1.31-zero2w（27.9 MB） | `/home/dfkvm/opi/out/kernel/Image` |
| `sun50i-h618-orangepi-zero2w.dtb` | `/home/dfkvm/opi/out/kernel/dtb/` |
| 2907 个模块 | `/home/dfkvm/opi/out/zero2w-modules.tar.gz` |
| ATF `bl31.bin` | `/home/dfkvm/opi/out/u-boot/` |
| 主线 u-boot `u-boot-sunxi-with-spl.bin` | `/home/dfkvm/opi/out/u-boot-mainline/` |
| **极简内核 `Image` 17.6 MiB / 16.2 MiB** | `~/opi/out-minimal/kernel/`、`~/opi/out-tiny/kernel/`（Windows: `build-out-minimal`、`build-out-tiny`） |
| 只含板载设备的 defconfig | `configs/zero2w-minimal_defconfig`、`configs/zero2w-tiny_defconfig` |

- 驱动落地清单、踩坑记录、复现步骤：**[BUILD-REPORT.md](BUILD-REPORT.md)**
- 离线镜像的 `6.1.31-opi-min` 无 initrd 启动修复、可复现组装（`wsl-14`）、只读校验（`wsl-15`）和 UART 验证：**[BOOT-CHAIN-REPAIR.md](BOOT-CHAIN-REPAIR.md)**
- GPIO 编号 / 40Pin 功能 / overlay 速查：**[GPIO.md](GPIO.md)**
- **只要极简版（只留板载设备 / 省内存 / 快启动）**：`wsl-10-minimize-config.sh` → `VARIANT=minimal|tiny wsl-11-build-minimal.sh` → `wsl-12-finalize-minimal.sh`，取舍与实测见 **[TUNING.md](TUNING.md)**
- 一键流程：`wsl-01-setup-env.sh` → `wsl-04-prepare-lf.sh` → `wsl-05-get-kernel-tarball.sh` → `wsl-03-build-kernel.sh` → `wsl-08-build-uboot-mainline.sh` → `wsl-07-verify.sh`
- 部署到板子（选变体，会自动跳过模块安装并顺带量内存/启动耗时）：
  `BOARD=root@<IP> VARIANT=minimal bash scripts/deploy-to-board.sh`

> ⚠️ 关键前提：**Windows 那份源码是 CRLF，只能当归档，不能编译**（kconfig 解析器会报 `missing end statement`）。编译必须用 WSL 里从 GitHub 直取的 LF 源码，见 BUILD-REPORT.md 第 3 节。

---

## 0. 这块板子的硬件（先看这个）

Orange Pi Zero 2W = **Allwinner H618**（H616 家族，内部代号 **sun50iw9p1**）：

| 部件 | 具体芯片 | Linux 里的驱动 |
|---|---|---|
| SoC / CPU | Allwinner H618，4x Cortex-A53 @ 最高 1.5GHz | `ARCH_SUNXI` / `mach-sun50i` |
| GPU | Mali-G31 MP2（Bifrost） | 主线 **panfrost**；闭源 **mali-bifrost**（社区 H616 移植版已下载） |
| 内存 | 1 / 1.5 / 2 / 4 GB LPDDR4（无 eMMC） | DRAM 时序在 `sys_config_orangepizero2w.fex`（17 组 dram_para） |
| 无线 | **AP6256 = Broadcom BCM43455/BCM4345C0**，Wi-Fi5(2.4/5G) + BT5.0，SDIO 挂 `mmc1`，复位脚 PG18 | 内核 `brcmfmac`（6.1/主线）、`bcmdhd`（5.4 BSP）；蓝牙 `btbcm` + `hci_uart` |
| 以太网 100M | H616 内置 EMAC + **X-Powers AC200** 内置 RMII PHY（mdio1 @ addr 1） | `sun4i-emac.c` + `sunxi-ac200.c` / `sunxi-ephy.c`（BSP 6.1） |
| PMIC | **X-Powers AXP313a**（RSB/`r_i2c` 0x36，PC9 中断，兼作开机键 PEK） | `axp20x-rsb.c` + `axp20x-regulator.c`（6.1/主线）；`axp2101*.c`（5.4 BSP） |
| 显示 | mini-HDMI（DE2 + DW-HDMI） | `drivers/gpu/drm/sun4i`（6.1/主线）；`disp2` fbdev（5.4 BSP） |
| 音频 | H616 内置 codec（3.5mm Line-out）+ HDMI 音频 | `sun50iw9-codec.c` / `sun50i-codec-analog.c` |
| 存储 | TF 卡（mmc0, 4bit）+ 板载 **16MB SPI NOR**（spi0） | `sunxi-mmc.c` / `spi-sun4i.c` + `m25p80` |
| USB | USB-C = OTG（**peripheral**）；USB2 host（ehci1/ohci1，经 24Pin/FPC 引出） | `phy-sun4i-usb.c` + `ehci/ohci-sunxi` |
| 其它 | IR 接收、LRADC 按键、3x LED（PC13 状态 / PC15 / PC16 网口灯）、40Pin GPIO/UART/I2C/SPI/PWM、24Pin FPC | `ir-sunxi`、`sun4i-lradc-keys`、`gpio-leds`、`wiringOP` |

> 常见误解：商品页写 H616，但 **dts compatible 是 `allwinner,sun50i-h618`**，文件名也是 `sun50i-h618-orangepi-zero2w.dts`。H616/H618 同代 silicon，驱动通用。

逐器件细节（文件路径 / DT 节点 / 固件文件名 / 已知坑）见 **[HARDWARE.md](HARDWARE.md)**。

---

## 1. 目录结构与已下载内容

```
zero2wsource/
├── README.md / HARDWARE.md / manifest.json / STATUS.md
├── scripts/                       可直接跑的构建脚本（WSL2 / Linux）
├── 01-soc-h616-h618-boot-kernel/  全志 H616/H618 引导 + BSP 内核（官方 SDK 主干）
│   ├── linux-orangepi-6.1-sun50iw9/     Allwinner/OrangePi BSP 内核 6.1.31  <- 含 zero2w 板级 dts ★
│   ├── linux-orangepi-5.4-sun50iw9/     Allwinner BSP 内核 5.4.125（bcmdhd/disp2/g2d/cedar-ve/axp2101）
│   ├── u-boot-orangepi-v2024.01/        官方 "next" u-boot（configs/orangepi_zero2w_defconfig）
│   ├── u-boot-orangepi-v2018.05-h618/   官方 "current" u-boot（BSP，含 zero2w defconfig）
│   ├── u-boot-orangepi-v2018.05-sun50iw9/
│   ├── arm-trusted-firmware-h616-bl31/  ATF，已 checkout 官方锁定 commit 4b9be5a（PLAT=sun50i_h616）
│   ├── orangepi-build/                  官方构建系统（next 分支，内置 orangepizero2w）
│   ├── orangepi-zero2w-bootloader-community/
│   └── sunxi-tools/                     sunxi-fel / boot.scr 等主机端工具
├── 02-mainline-linux-uboot/       主线源码（长期可维护方案）
│   ├── linux-mainline-master/           主线内核（有 zero2w dts，但外设覆盖不全）
│   └── u-boot-mainline-master/          主线 u-boot（有 orangepi_zero2w_defconfig）
├── 03-wifi-bt-ap6256/
│   ├── orangepi-firmware/               brcmfmac43455-sdio.bin/.txt/.clm_blob, BCM4345C0.hcd, nvram_ap6256.txt, aw87xxx_acf.bin ...
│   └── linux-firmware-sparse/           上游 linux-firmware（sparse 只留 broadcom/cypress）
├── 04-pmic-axp313-ac200/XPowersLib-axp313-regs/   AXP313 寄存器级参考实现
├── 05-gpu-mali-g31-display/
│   ├── mali-bifrost-h616/  libcedarc-vpu/  sunxi-g2d/
├── 06-build-system-rootfs/        orangepi-scripts, orangepi-external, OrangePi_Build-legacy, armbian-build, buildroot, Buildroot-YuzukiSBC
├── 07-userspace-hal/              wiringOP, wiringOP-Python
├── 08-docs-datasheets/            orangepi-official-docs（原理图/DXF/手册）+ opi-documentation
├── 09-toolchain/gcc-aarch64-linux-gnu-6.3/
├── 10-community-h616-h618/        opi-zero3-i2s-5.4 / -6.1（H618 I2S 音频补丁）
└── _logs/                         抓取日志 + 可重跑的抓取脚本
```

---

## 2. 板级配置的"唯一事实来源"

| 想搞清楚的事 | 看这里 |
|---|---|
| Zero2W 用哪个内核/uboot 分支 | `01-…/orangepi-build/external/config/boards/orangepizero2w.conf` |
| 官方内核/uboot/ATF 分支映射（最重要） | `01-…/orangepi-build/external/config/sources/families/sun50iw9.conf` |
| 板级 DTS（内核侧 6.1） | `linux-orangepi-6.1-sun50iw9/arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dts` |
| 6.1 内核 config | `linux-orangepi-6.1-sun50iw9/arch/arm64/configs/linux_sunxi64_defconfig`（等价：`orangepi-build/external/config/kernel/linux-6.1-sun50iw9-next.config`） |
| 100M 网口 / DRAM 时序 / 管脚（BSP 侧） | `orangepi-build/external/packages/pack-uboot/sun50iw9/bin/sys_config/sys_config_orangepizero2w.fex` |
| u-boot 侧板级 dts | `…/pack-uboot/sun50iw9/bin/dts/orangepizero2w-u-boot-current.dts` |
| 全志引导 blob 与打包工具 | `…/pack-uboot/sun50iw9/bin/`（boot0_sdcard.fex、monitor.fex、optee、boot_package.cfg）+ `…/tools/`（dragonsecboot、script、update_uboot、dtc…） |
| 启动脚本 / env | `orangepi-build/external/config/bootscripts/boot-sun50iw9{,-next}.cmd`、`external/config/bootenv/sun50iw9-default.txt` |
| DT overlay | 内核树 `linux-orangepi-6.1-sun50iw9/arch/arm64/boot/dts/allwinner/overlay/`（`sun50i-h616-gpu.dts`、`-ir.dts`、`-ph-i2c*.dts`、`-pi-pwm*.dts`、`-spi0-spidev.dts`、`-usb0-host.dts`、`fixup.scr` 等）、`orangepi-build/external/packages/bsp/h618/sun50i-h618-lradc-keys-{current,next}.dts` |
| 音频初始状态 | `orangepi-build/external/packages/blobs/asound.state/asound.state.sun50iw9-{legacy,current,next}` |

官方分支映射（摘自 `families/sun50iw9.conf`，已逐条核对）：

- **current**：内核 `orange-pi-5.4-sun50iw9` + u-boot `v2018.05-h618`（需 dragonsecboot 打包 boot_package.fex）
- **next**：内核 `orange-pi-6.1-sun50iw9` + u-boot `v2024.01` + ATF（`PLAT=sun50i_h616`，commit `4b9be5abfa8b1a7c7e00776da01768d3358e648a`）
- WiFi 用 in-tree `brcmfmac`（`bcmdhd` 被 MODULES_BLACKLIST），蓝牙 `btbcm` + `hci_uart` + `hciattach_opi`

> 结论：**内核定制请从 6.1-sun50iw9 起步**（唯一自带 zero2w 完整板级 dts 的 BSP 内核）。
> 5.4-sun50iw9 的价值是**厂商驱动参考**（`drivers/net/wireless/bcmdhd`、`drivers/video/fbdev/sunxi/disp2`、`drivers/char/sunxi_g2d`、`drivers/media/cedar-ve`、`axp2101` PMIC、TV-AC200）；它里面 **没有** zero2w 专属 dts（只有 `overlay/sun50i-h616-zero2w-disable-led.dts` 与 zero3 板级 dts）。

---

## 3. 三条可复现路线

### 前置：请在 Linux（推荐 WSL2 Ubuntu 22.04/24.04）里编译
Windows 上这些源码树不能直接编译（`aux.c` 保留名、大小写重名文件、构建脚本依赖 bash）。

```bash
bash /mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource/scripts/prepare-wsl-ubuntu.sh
```

### 路线 A：官方 orangepi-build 一键出镜像（第一次跑通最省事）

```bash
cd 01-soc-h616-h618-boot-kernel/orangepi-build
sudo apt-get install -y git libncurses-dev bison flex libssl-dev device-tree-compiler \
     u-boot-tools qemu-user-static binfmt-support pigz whiptail
./build.sh -b orangepizero2w -k next -d jammy      # 板子 / 内核分支(current|next) / 发行版
# 产物：output/images/Orangepizero2w_..._jammy_(desktop|cli).img
# 只改内核反复出镜像：SKIP_UBOOT_COMPILE=yes ./build.sh ...
```
本项目推荐直接用 headless 封装脚本：`bash scripts/build-orangepi-build.sh`。它使用
`userpatches/config-carplay.conf` 和 `userpatches/customize-image.sh`，内置并启用 OpenSSH，
允许密码方式登录 root；默认账号/密码为 `root` / `root`：

```bash
ssh root@<板子IP>
# password: root
```

> 该密码仅用于可信局域网内调试，接入不可信网络前必须修改。

### 路线 B：手动编译 BSP 内核（迭代最快，做驱动定制推荐）

```bash
cd 01-soc-h616-h618-boot-kernel
# 1) ATF bl31（next 需要；仓库已 checkout 到官方锁定 commit）
cd arm-trusted-firmware-h616-bl31
make CROSS_COMPILE=aarch64-linux-gnu- PLAT=sun50i_h616 DEBUG=1 bl31 -j$(nproc)
cp build/sun50i_h616/debug/bl31.bin ../u-boot-orangepi-v2024.01/    # Allwinner u-boot 在源码根目录找 bl31.bin

# 2) u-boot（官方 next = Allwinner fork v2024.01）
cd ../u-boot-orangepi-v2024.01
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- orangepi_zero2w_defconfig
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)           # -> u-boot-sunxi-with-spl.bin

# 3) 内核 6.1 BSP
cd ../linux-orangepi-6.1-sun50iw9
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- linux_sunxi64_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs modules -j$(nproc)
# -> arch/arm64/boot/Image
# -> arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dtb
```
上面 1)-3) 已封装为 `scripts/build-kernel-bsp.sh`。

### 路线 C：全主线（mainline u-boot + 主线内核）

```bash
cd 02-mainline-linux-uboot/u-boot-mainline-master
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- orangepi_zero2w_defconfig
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)     # 主线 H616 不需要 ATF
cd ../linux-mainline-master
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- sunxi_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs modules -j$(nproc)
```

**主线现状（已在本地树逐条核对，别踩坑）：**
- ✅ `sun50i-h618-orangepi-zero2w.dts` 存在，但只描述 UART0 / TF(mmc0) / USB-C(peripheral) / SPI NOR / LED / AXP313 电源域 / 内置 codec / GPU(panfrost)
- ❌ **没有** `mmc1` SDIO Wi-Fi 节点 -> 板载 AP6256 用不了
- ❌ **没有** EMAC/AC200 网口节点，且主线内核里 **没有** `drivers/mfd/*ac200*`、`sunxi-ephy` 驱动 -> 100M 以太网不可用
- ➡️ 走主线必须自己补 dts + 从 6.1 BSP 移植 AC200 PHY 驱动（Wi-Fi 只需补 `mmc1` + `brcmfmac` + 固件即可）

---

## 4. 烧写与调试

| 事项 | 值 |
|---|---|
| 串口 | UART0 @115200，PH0/PH1（板边 3Pin 焊盘 / 40Pin） |
| SD 卡 SPL 位置 | 主线与 v2024.01：`u-boot-sunxi-with-spl.bin` -> `dd bs=1024 seek=8`；BSP-2018(current)：`boot0_sdcard.fex` -> `dd bs=8k seek=1`，`boot_package.fex` -> `dd bs=8k seek=2050`（依据 `orangepi-build/…/include/sunxi64_common.inc: write_uboot_platform()`） |
| FEL / 救砖 | Maskrom 模式 + `sunxi-fel`（`01-…/sunxi-tools`） |
| boot.scr | `mkimage -C none -A arm -T script -d boot-sun50iw9.cmd boot.scr` |
| 快速验证只换内核 | 挂载镜像 FAT 的 /boot，替换 `Image` + `dtb/…dts/sun50i-h618-orangepi-zero2w.dtb` + `orangepiEnv.txt`，不必重刷整卡 |

---

## 5. Windows 侧注意事项（本仓库怎么落盘的）

1. `drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c` 在 Windows 是非法文件名 -> 已用 `git -c core.protectNTFS=false` 检出；若报缺文件，Linux 侧 `git checkout -f` 即可复原。
2. 大小写重名（`xt_MARK.h`/`xt_mark.h`、`ipt_TTL.h`/`ipt_ttl.h` 等）会让 `git status` 永久显示 13 条 "M" —— **Windows 文件系统限制，不是源码损坏**，内核编译不引用它们。
3. 所有仓库浅克隆。要历史：`git fetch --unshallow --filter=blob:none`。
4. GitHub 直连不稳定，最终走本机代理 `127.0.0.1:7890`（`git config --global http.proxy` 已设置；不想保留就 `--unset`）。

## 6. 重新抓取 / 补齐

```powershell
powershell -File _logs\clone-remaining.ps1    # 补 5 个大仓库（已完成会 SKIP）
powershell -File _logs\fetch-extras.ps1       # 补社区 / GPU / VPU 仓库
python _logs\fetch-docs-retry.py              # 补 Drive 限流的 PDF（用户手册 / 机械图 / 24Pin 扩展板原理图）
```

## 7. 建议的下一步

1. 按 **路线 B** 在 WSL2 里编出 `Image + dtb`，替换官方镜像内核跑通，建立"能编译、能启动"的基线。
2. 建自己的 defconfig（`linux_sunxi64_defconfig` -> menuconfig -> `savedefconfig`），裁掉不需要的厂商驱动。
3. 复制 `sun50i-h618-orangepi-zero2w.dts` 做你的板级 dts；小改动优先用 overlay（内核树 `arch/arm64/boot/dts/allwinner/overlay/` 里已有 gpu/ir/i2c/pwm/spi/usb-host 等现成写法，另有 `bsp/h618/sun50i-h618-lradc-keys-*.dts`）。
4. Wi-Fi/BT 定制：注意 `external/packages/bsp/sunxi/ap6256-{wifi,bluetooth}.service` 是**旧的 bcmdhd 方案**（Zero2W 不用，`MODULES_BLACKLIST_CURRENT=bcmdhd`）。Zero2W 走 in-tree `brcmfmac` + `/lib/firmware/brcm/brcmfmac43455-sdio.{bin,txt,clm_blob}`；蓝牙走 `btbcm` + `hci_uart`（`hciattach_opi`、`brcm_patchram_plus` 预编译二进制在 `external/packages/blobs/bt/`）。要调发射功率/天线/频域就改 nvram（`brcmfmac43455-sdio.txt`，源文件在 `03-wifi-bt-ap6256/orangepi-firmware`）。
5. 想长期跟主线：按路线 C 补 AC200 + mmc1，把改动整理成 patch series（`06-…/armbian-build` 的组织方式可以照抄）。
