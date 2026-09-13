# 抓取状态 / STATUS

生成：2026-09-04 08:45 · 目录：`D:\littlethings\CarPlay\zero2w\linux\zero2wsource`

## 总览

- 仓库数：**29**（全部 `--depth 1 --single-branch` 浅克隆）
- 文件数：**390,911**
- 体积：**8.48 GB**
- 明细（URL / commit / 用途）：`manifest.json` · 硬件→源码映射：`HARDWARE.md` · 构建路线：`README.md` + `scripts/`

## 已下载清单

| 路径 | 分支 | HEAD | MB | 文件数 | 用途 |
|---|---|---|---|---|---|
| `01-soc-h616-h618-boot-kernel/arm-trusted-firmware-h616-bl31` | `HEAD` | `4b9be5abf` | 47.8 | 4607 | ATF，已 checkout 官方锁定 commit 4b9be5a…（PLAT=sun50i_h616 -> bl31.bin） |
| `01-soc-h616-h618-boot-kernel/linux-orangepi-5.4-sun50iw9` | `orange-pi-5.4-sun50iw9` | `3a4a29414` | 1196.8 | 70186 | 官方 current 内核(5.4.125)：全志厂商驱动库 bcmdhd / disp2 fbdev / sunxi_g2d / cedar-ve / axp2101 PMIC / TV-AC200 |
| `01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9` | `orange-pi-6.1-sun50iw9` | `71144529b` | 1693.8 | 83875 | ★官方 next 内核(Allwinner/OrangePi BSP 6.1.31)：含 sun50i-h618-orangepi-zero2w.dts、sunxi-ac200、sunxi-ephy、brcmfmac —— 内核定制主基线 |
| `01-soc-h616-h618-boot-kernel/orangepi-build` | `next` | `bdba42198` | 691.4 | 2368 | ★官方构建系统(branch next)：内置 orangepizero2w；内核/uboot/ATF 分支映射、boot0/monitor blob、dragonsecboot 打包工具 |
| `01-soc-h616-h618-boot-kernel/orangepi-zero2w-bootloader-community` | `main` | `a0e28c2e3` | 24.2 | 32 | 社区 Zero2W 引导/打包仓库（boot0+u-boot 组合与烧写参考） |
| `01-soc-h616-h618-boot-kernel/sunxi-tools` | `master` | `d7bbd172a` | 0.6 | 114 | 主机端工具：sunxi-fel(救砖)、boot.scr/sunxi 相关工具 |
| `01-soc-h616-h618-boot-kernel/u-boot-orangepi-v2018.05-h618` | `v2018.05-h618` | `66e2615a1` | 148.6 | 13842 | 官方 current 引导(BSP 2018.05)：orangepi_zero2w_defconfig（DRAM 由预编译 boot0 + fex dram_para 完成） |
| `01-soc-h616-h618-boot-kernel/u-boot-orangepi-v2018.05-sun50iw9` | `v2018.05-sun50iw9` | `273096252` | 127.9 | 13204 | 同族更早的 sun50iw9 BSP u-boot（参考） |
| `01-soc-h616-h618-boot-kernel/u-boot-orangepi-v2024.01` | `v2024.01` | `a4d4b0e24` | 157.9 | 20394 | 官方 next 引导(全志 fork, 主线风格)：configs/orangepi_zero2w_defconfig + dram_sun50i_h616 + AXP313 |
| `02-mainline-linux-uboot/linux-mainline-master` | `master` | `bc35965f6` | 1901.1 | 96042 | 主线内核：已有 zero2w dts，但缺 AP6256 SDIO 与 AC200 网口（见 HARDWARE.md 第11节） |
| `02-mainline-linux-uboot/u-boot-mainline-master` | `main` | `cc557af45` | 311.3 | 38601 | 主线 u-boot：orangepi_zero2w_defconfig（无需 ATF） |
| `03-wifi-bt-ap6256/linux-firmware-sparse` | `main` | `e981caea6` | 795.2 | 174 | 上游 linux-firmware，sparse 只检出 broadcom/cypress（对照/升级固件） |
| `03-wifi-bt-ap6256/orangepi-firmware` | `master` | `db5e86200` | 83.5 | 468 | ★AP6256(BCM43455/43456) Wi-Fi 固件 + nvram_ap6256 + BT .hcd + aw87xxx 功放固件 |
| `04-pmic-axp313-ac200/XPowersLib-axp313-regs` | `master` | `d6997586e` | 11.1 | 113 | X-Powers AXP313/AXP2101 寄存器级参考（调电源时序时查表） |
| `05-gpu-mali-g31-display/libcedarc-vpu` | `master` | `e4246be52` | 78.6 | 489 | Allwinner Cedar 硬件视频解码库（用户态） |
| `05-gpu-mali-g31-display/mali-bifrost-h616` | `main` | `0f8ec33d9` | 6.3 | 450 | Arm Mali-G31(Bifrost) 闭源 DRM 内核驱动的 H616 移植 |
| `05-gpu-mali-g31-display/sunxi-g2d` | `main` | `77a50a4ce` | 0.1 | 40 | 全志 G2D 2D 加速接口参考 |
| `06-build-system-rootfs/armbian-build` | `main` | `96d652a9e` | 270.5 | 9219 | Armbian：主线内核/u-boot 板级配置与 patch 组织方式参考 |
| `06-build-system-rootfs/buildroot` | `master` | `c05de97a2` | 32.4 | 15219 | Buildroot：做极简可复现 rootfs |
| `06-build-system-rootfs/Buildroot-YuzukiSBC` | `master` | `377bdb4de` | 115.9 | 13082 | 社区 buildroot 配置集（含 H616/H618 类板子） |
| `06-build-system-rootfs/OrangePi_Build-legacy` | `master` | `9cc4d8d18` | 0.1 | 41 | 老一代官方构建系统（参考） |
| `06-build-system-rootfs/orangepi-external` | `master` | `e462137f8` | 109.9 | 638 | OrangePi Linux SDK external（旧，含部分驱动/工具） |
| `06-build-system-rootfs/orangepi-scripts` | `master` | `aa95623d3` | 0.1 | 34 | OrangePi Linux SDK 构建脚本（旧） |
| `07-userspace-hal/wiringOP` | `next` | `a7921ca2f` | 1.5 | 240 | OrangePi 用户态 GPIO/UART/I2C/SPI/PWM 库（wiringPi 分支） |
| `07-userspace-hal/wiringOP-Python` | `master` | `98a3e6622` | 0.1 | 69 | wiringOP 的 Python 绑定 |
| `08-docs-datasheets/opi-documentation` | `master` | `6f5192f7c` | 22.6 | 115 | OrangePi 文档仓库（旧板子为主） |
| `09-toolchain/gcc-aarch64-linux-gnu-6.3` | `aarch64-linux-gnu-6.3` | `1d74de505` | 855.7 | 7185 | OrangePi 官方工具链仓分支（linaro aarch64 6.3.1 tarball） |
| `10-community-h616-h618/opi-zero3-i2s-5.4` | `main` | `8bc84b62f` | 0.2 | 35 | 社区补丁：H618 I2S3 音频（5.4/current） |
| `10-community-h616-h618/opi-zero3-i2s-6.1` | `main` | `1d52026a2` | 0.1 | 35 | 社区补丁：H618 I2S3 音频（6.1/next） |

## 硬件资料（官方 Google Drive）

- `08-docs-datasheets/orangepi-official-docs/`
  - schematic/Opi ZERO 2W DXF.rar  (177 KB)
  - schematic/OPi_ZERO 2W_SCH.pdf  (566 KB)
  - user-manual/How to Deploy OpenClaw on Orange Pi 4Pro, Zero3 and Zero2W.pdf  (667 KB)
- 原理图预览：`08-docs-datasheets/preview/schematic-p-1.png` / `-p-2.png`（由 `OPi_ZERO 2W_SCH.pdf` 渲染，Xunlong "ORANGEPI Zero 2W" Rev1.0，7 页）
- `opi-documentation/`：OrangePi 文档仓库（旧板子为主）

## 未完成 / 需要你自己补的

| 项 | 状态 | 怎么办 |
|---|---|---|
| `OrangePi_Zero2w_H618_User Manual_v1.3.pdf` | ⚠ Drive 限流 | 1–2 小时后 `python _logs\fetch-docs-retry.py`；或浏览器开 [用户手册目录](https://drive.google.com/drive/folders/1KIZMMDBlqf1rKmOEhGH7_7A-COAgYoGZ) |
| `ZERO_2W_24PIN_EXTEND_SCH.pdf` | ⚠ Drive 限流 | [Schematic 目录](https://drive.google.com/drive/folders/1tyaEemq5COwmbvGT7v_cfKhg2_t4u_yg) |
| 机械尺寸图 | ⚠ Drive 限流 | [Mechanical 文件](https://drive.google.com/file/d/1eKUft1eGb4a7q1CJBPQM7S6nnvq_YW-h/view) |
| 全志 H616/H618 Datasheet / User Manual | ❌ 官方需登录 | 注册 www.aw-ol.com；日常开发靠原理图 + 源码已够 |
| Allwinner longan SDK（Mali 授权驱动、官方 libcedarc、Tina） | ❌ 无官方 GitHub | 全志官网/FTP；社区镜像搜 `Mini-LinuxPC-Pro`、`TinaSDK_*` |
| Arm Mali-Bifrost r38/r48 tarball | ❌ developer.arm.com 403 | 浏览器手动下载；已备 `mali-bifrost-h616` |
| OrangePi 官方镜像（base rootfs） | ➖ 未下载（体积大） | [Zero2W 支持页](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/service-and-support/Orange-Pi-Zero-2W.html)（本机 HTTPS 不通，用 HTTP/浏览器） |

## Windows 检出已知问题（不影响 Linux 编译）

1. `drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c` 是 Windows 保留名，已用 `core.protectNTFS=false` 强制检出；若仍缺，Linux 侧 `git checkout -f`。
2. 大小写重名（`xt_MARK.h`/`xt_mark.h`、`ipt_TTL.h`/`ipt_ttl.h` 等）→ `git status` 永久 13 条 `M`，正常现象。
3. 建议 `rsync` 到 WSL2 本地 fs 再编译（`scripts/prepare-wsl-ubuntu.sh` 已含该步骤）。

## 重跑抓取

```powershell
cd D:\littlethings\CarPlay\zero2w\linux\zero2wsource
powershell -File _logs\clone-remaining.ps1     # 5 个大仓库（已存在会 SKIP）
powershell -File _logs\fetch-extras.ps1        # 社区 / GPU / VPU 仓库
python _logs\fetch-docs-retry.py               # Drive 限流的 PDF
```

> 本次 GitHub 直连多次被重置，最终经本机代理 `127.0.0.1:7890` 完成。
> `git config --global http.proxy`/`https.proxy` 已设置；不想保留：
> `git config --global --unset http.proxy; git config --global --unset https.proxy`

---

## 第三阶段快照（2026-09-04 晚）：极简内核（只要板载设备 + 省内存 + 快启动）

需求变更：**不要 CAN；只留开发板上有的设备；尽量压低运行内存与启动时间。**

| 变体 | 版本串 | Image | Image.gz | =y / =m | 模块 | 说明 |
|---|---|---|---|---|---|---|
| full（第一阶段） | `6.1.31-zero2w` | 26.6 MiB | — | 2118 / 2785 | 2907（132 MB） | 板载全开 + BSP 默认一堆无关驱动 |
| **minimal（推荐）** | `6.1.31-opi-min` | **17.1 MiB（-35.6%）** | 7.2 MiB | 1660 / 897 | 878（64 MB） | 只留板载设备，驱动全部 built-in |
| tiny | `6.1.31-opi-tiny` | **16.2 MiB（-39.1%）** | 6.9 MiB | 1518 / **0** | **0** | minimal 再 `MODULES=n`，一个模块都不装 |

配置项总量 **4877 → 2557（-48%）**；`wsl-13-verify-minimal.sh` 输出 `VERIFY_MINIMAL_OK`
（101 项板载驱动字面 `=y`、62 项无关/调试项为 `n`、`System.map` 抽查 13 组驱动符号确认内建）。

**运行内存最大的一刀**：`CONFIG_CMA_SIZE_MBYTES` 128 → 16（1 GB 板上直接多回 112 MB 可用），
另加 `BASE_SMALL=1`、`MEMCG/CPUSETS/BLK_CGROUP=n`、`SLUB_DEBUG=n`、`BLK_DEV_INITRD=n`、
`KSM/ZRAM/HUGETLB/THP=n`、`LOCKDOWN→MODULE_SIG→X509/PKCS7` 整链关闭、`HZ=100`、`-Os`。
**启动时间**：`STAGING/无关驱动`删掉后 probe 项减少；其余大头在 u-boot 与 rootfs（见 TUNING.md §4/§5）。

新增文件：
- `configs/zero2w-minimal.spec`（裁剪清单，1000+ 行，可复审可编辑）
- `configs/zero2w-minimal_defconfig` / `zero2w-minimal.config` / `zero2w-minimal.stats.txt`
- `configs/zero2w-tiny_defconfig` / `zero2w-tiny.config`
- `scripts/minimize_config.py`、`wsl-10-minimize-config.sh`、`wsl-11-build-minimal.sh`、
  `wsl-12-finalize-minimal.sh`、`wsl-13-verify-minimal.sh`
- `build-out-minimal/kernel/`、`build-out-tiny/kernel/`（Image / Image.gz / System.map / kernel.config / dtb）
- `TUNING.md`（内核之外那 80% 的收益：CMA、cmdline、u-boot 裁剪、systemd 裁剪、度量方法、回退清单）

**仍未做的**：板子实测（`MemTotal`、上电→登录耗时）；u-boot 侧那套 `CONFIG_BOOTDELAY=0 / 砍 NET /
砍 VIDEO` 的重编；rootfs 裁剪脚本。原因：需要实物板子与烧卡权限。
