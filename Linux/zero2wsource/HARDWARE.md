# Orange Pi Zero 2W — 硬件 → 源码 / 驱动 / 固件 全映射

标注约定：
- **K61** = `01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9`（官方 next，内核 6.1.31，★主用定制基线）
- **K54** = `01-soc-h616-h618-boot-kernel/linux-orangepi-5.4-sun50iw9`（官方 current，内核 5.4.125，Allwinner 厂商驱动参考库）
- **MAIN** = `02-mainline-linux-uboot/linux-mainline-master`（主线）
- **OB** = `01-soc-h616-h618-boot-kernel/orangepi-build`（官方构建系统）

---

## 1. SoC / CPU：Allwinner H618（sun50iw9p1，H616 家族）

| 项 | 内容 |
|---|---|
| CPU | 4x Cortex-A53，最高 1.5GHz；`CPUMIN=480000`、`CPUMAX=1512000`（`OB/external/config/sources/families/sun50iw9.conf`） |
| DT | K61: `arch/arm64/boot/dts/allwinner/sun50i-h616.dtsi` + `sun50i-h616-cpu-opp.dtsi` + `sun50i-h618-orangepi-zero2w.dts`（`compatible = "xunlong,orangepi-zero2w","allwinner,sun50i-h618"`） |
| 时钟 | `drivers/clk/sunxi-ng/ccu-sun50i-h6.c`、`ccu-sun50i-h6-r.c`、`ccu-sun50i-r.c`（K54/K61/MAIN 均有） |
| 管脚 | `drivers/pinctrl/sunxi/pinctrl-sun50i-h616*.c`（`CONFIG_PINCTRL_SUN50I_H616{,_R}=y`） |
| DVFS | `drivers/cpufreq/sun50i-cpufreq-nvmem.c`（`CONFIG_ARM_ALLWINNER_SUN50I_CPUFREQ_NVMEM=y`，读 SID eFuse 定档） |
| 温度 | K61/MAIN: `drivers/thermal/sun8i_thermal.c`；K54: `drivers/thermal/sunxi_thermal.c`（厂商版） |
| MBUS/IOMMU | `drivers/memory/sunxi-mbus.c`、`drivers/iommu/sun50i-iommu.c`（K61 `CONFIG_SUN50I_IOMMU=y`） |
| 厂商系统接口 | K54/K61: `drivers/char/sunxi-sysinfo`、`drivers/misc/sunxi-addr`、`drivers/soc/sunxi/*`（smc/sysinfo/efuse） |
| RSB 总线 | `drivers/soc/sunxi/sunxi-rsb.c`（K61 `CONFIG_SUNXI_RSB=y`）；dts 里的 `&r_i2c` 就是它 |

## 2. DRAM：LPDDR4 1 / 1.5 / 2 / 4 GB

- 官方板级 DRAM 时序：**`OB/external/packages/pack-uboot/sun50iw9/bin/sys_config/sys_config_orangepizero2w.fex`** 中的 `[dram_para]` ~ `[dram_para16]`（17 组，配合 `dram_select_para` 用 GPIO/ADC 识别不同内存颗粒版本）。
- 厂商 u-boot v2018.05-h618（current 路线）：**u-boot 里没有 H616 DRAM 初始化代码**（只有 `arch/arm/mach-sunxi/board_sun50iw9.c`），DRAM 训练由全志预编译的 **boot0** 完成，参数就是上面 fex 里的 `dram_para*`。boot0 blob 在 `OB/external/packages/pack-uboot/sun50iw9/bin/boot0_sdcard.fex-linux5.4`（连同 `monitor.fex* `、`optee*.bin`、`boot_package.cfg`、打包工具 `dragonsecboot`/`script`/`update_uboot`）。
- 官方 u-boot v2024.01（next 路线）：**DRAM 参数直接写进 defconfig** → `configs/orangepi_zero2w_defconfig` 里的 `CONFIG_DRAM_SUN50I_H616_DX_ODT/DX_DRI/CA_DRI/ODT_EN/TPR6/TPR10/TPR11/TPR12` + `CONFIG_SUNXI_DRAM_H616_LPDDR4=y` + `CONFIG_DRAM_CLK=792`（实现在 `arch/arm/mach-sunxi/dram_sun50i_h616.c`）。调内存/超频改这里最方便。
- 主线 u-boot：同一份 `arch/arm/mach-sunxi/dram_sun50i_h616.c`（sunxi-workshop 逆向成果），板级 defconfig = `configs/orangepi_zero2w_defconfig`，dts 走 `dts/upstream/src/arm64/allwinner/sun50i-h618-orangepi-zero2w.dts`（与内核同步）。
- 定制点：换内存颗粒 / 提频 → 优先 next 或主线路线（改 defconfig 里 DRAM_* 与 `DRAM_CLK` 即可）；走 current 路线则改 fex 的 `dram_para*` 后重新 `dragonsecboot -pack` 打包。

## 3. PMIC：X-Powers AXP313a（供电 + 开机键 + 唤醒）

| 项 | 内容 |
|---|---|
| 接线 | RSB（`&r_i2c`）地址 0x36，IRQ = PC9（低有效），`wakeup-source` |
| 电源域 | dcdc1 = GPU/SYS(0.81–0.99V)、dcdc2 = CPU(0.81–1.1V)、dcdc3 = DRAM 1.1V、aldo1 = 1.8V、dldo1 = 3.3V |
| K61 驱动 | `drivers/mfd/axp20x.c` + `axp20x-rsb.c` + `drivers/regulator/axp20x-regulator.c`（`CONFIG_REGULATOR_AXP20X=y`，含 AXP313）、开机键 `drivers/input/misc/axp20x-pek.c` |
| K54 驱动 | 全志私有 `drivers/mfd/axp2101.c`/`axp2101-i2c.c` + `drivers/regulator/axp2101-regulator.c` + `drivers/input/misc/axp2101-pek.c`（AXP313 在全志命名里属 axp2101 系列） |
| MAIN | 同 K61（`include/linux/mfd/axp20x.h` 里有 `AXP313A_ID`） |
| 寄存器速查 | `04-pmic-axp313-ac200/XPowersLib-axp313-regs`（含 AXP313/AXP2101 全寄存器定义，调电压/电源序列时很好对照） |
| u-boot | BSP v2018.05-h618：`configs/orangepi_zero2w_defconfig` → `CONFIG_SUNXI_PMU=y`、`CONFIG_AXP1530_POWER=y`（slave 0x36）；主线 u-boot：`CONFIG_AXP313_POWER` |

## 4. 以太网 100M：H616 EMAC + X-Powers AC200 内置 PHY（RMII）

| 项 | 内容 |
|---|---|
| DT | K61 dts：`&emac1`（`phy-mode="rmii"`，`allwinner,rx-delay-ps=<3100>`、`tx-delay-ps=<700>`）+ `&mdio1 { rmii_phy: ethernet-phy@1 }`；AC200 挂在 `&i2c3`（`x-powers,ac200` @0x10，时钟由 **PWM5 输出 24MHz** 提供，见 `ac200_pwm_clk: compatible="pwm-clock"`） |
| MAC 驱动 | `drivers/net/ethernet/allwinner/sun4i-emac.c`（K54/K61/MAIN 都有；K61 `CONFIG_SUN4I_EMAC=y`） |
| PHY/MFD | K61: `drivers/mfd/sunxi-ac200.c` + `include/linux/mfd/ac200.h` + `drivers/net/phy/sunxi-ephy.c`；K54: AC200 只用作 TV-out（`drivers/video/fbdev/sunxi/disp2/tv/tv_ac200.c`）；**MAIN 目前没有任何 ac200 驱动**（走主线必须自己移植） |
| 网口灯 | `100m_link` = PC15、`100m_act` = PC16 |
| 提醒 | Zero2W 只有 **100M**（RMII + AC200），不是千兆；要千兆是 Zero3/R1-plus 那一类方案 |

## 5. Wi-Fi / 蓝牙：AP6256（Broadcom BCM43455C0 / BCM43456C5）

| 项 | 内容 |
|---|---|
| 硬件 | SDIO 4-bit 挂 `mmc1`（`mmc-ddr-1_8v`），VMMC=3.3V、VQMMC=1.8V，复位 = **PG18**（`wifi_pwrseq: mmc-pwrseq-simple`，时钟取 `&rtc 1` osc32k，post-power-on 200ms） |
| Wi-Fi 驱动 (K61/MAIN) | `drivers/net/wireless/broadcom/brcm80211/brcmfmac/`：`CONFIG_BRCMFMAC=m` + `CONFIG_BRCMFMAC_SDIO=y` |
| Wi-Fi 驱动 (K54) | 树内带完整 **`drivers/net/wireless/bcmdhd/`**（Broadcom DHD）。官方 Zero2W 把它拉黑（`MODULES_BLACKLIST_CURRENT="bcmdhd"`），但仍可自行启用 |
| 蓝牙 | `drivers/bluetooth/btbcm.c` + `hci_uart.c`（K61：`CONFIG_BT_BCM=m`、`CONFIG_BT_HCIUART_BCM=y`），UART 用 ttyS1/uart1 |
| 固件（brcmfmac 命名） | `03-wifi-bt-ap6256/orangepi-firmware/brcm/brcmfmac43455-sdio.bin` / `.clm_blob` / `.txt`，另有 `brcmfmac43456-sdio.bin/.txt`；BT：`brcm/BCM4345C5.hcd`、根目录 `BCM4345C0.hcd` |
| 固件（bcmdhd 命名） | `fw_bcm43455c0_ag{,_apsta,_p2p}.bin`、`fw_bcm43456c5_ag*.bin`、`nvram_ap6256.txt`（含 `-orangepi4a`、`-orangepirv2` 变体） |
| 上游对照 | `03-wifi-bt-ap6256/linux-firmware-sparse/broadcom`（上游 linux-firmware，sparse 只检出 broadcom/cypress） |
| 用户态参考 | `OB/external/packages/bsp/sunxi/ap6256-wifi.service`、`ap6256-bluetooth.service`（旧 bcmdhd 方案，流程参考）；`OB/external/packages/blobs/bt/{hciattach/hciattach_opi_arm64, brcm_patchram_plus_arm64}` |
| 定制点 | ① `brcmfmac43455-sdio.txt`（=nvram）里的 `pa0b0/pa0b1/pa0itims`、`txpwgtab` 调功率/天线；② `.clm_blob` 决定频域/雷达限制；③ AP+STA 并发与省电用 `brcmfmac` 模块参数 + `wpa_supplicant`/`hostapd`；④ 外置天线版本要换对应 nvram |

## 6. GPU：Mali-G31 MP2（Bifrost）

| 路线 | 位置 |
|---|---|
| 主线（先跑通） | K61 与 MAIN 的 `drivers/gpu/drm/panfrost/`（`CONFIG_DRM_PANFROST=m`）；板级 dts 里 K61 默认 `&gpu{ status="disabled" }`，用 overlay `arch/arm64/boot/dts/allwinner/overlay/sun50i-h616-gpu.dts` 打开 |
| Arm 闭源内核驱动 | `05-gpu-mali-g31-display/mali-bifrost-h616`（社区把 Arm Bifrost DRM 驱动移植到 H616/6.1） |
| 厂商 5.4 | K54 的 `CONFIG_SUNXI_GPU_TYPE="mali-g31"` 说明官方用 Arm 授权驱动，源码在全志 longan SDK 的 `bsp/drivers/gpu/`（**不在本次仓库内**，需全志账号） |
| 用户态 | 走 panfrost 就用 Mesa；闭源 `libmali` blob 由 Armbian/全志发布（参考 `06-build-system-rootfs/armbian-build` 里的 libmali 打包） |

## 7. 显示：mini-HDMI + DE2（CVBS/TV-out 走 AC200）

- K61/MAIN：`drivers/gpu/drm/sun4i/`（DE2/CRTC/encoder）+ `drivers/gpu/drm/bridge/synopsys/dw-hdmi.c` + `drivers/phy/allwinner/phy-sun8i-hdmi.c`；dts：`&de`、`&hdmi`、`connector{ compatible="hdmi-connector" }`
- K54：全志私有 fbdev 栈 `drivers/video/fbdev/sunxi/disp2/`（`dev/hdmi/`、`tv/tv_ac200.c`），`CONFIG_DISP2_SUNXI=y`、`CONFIG_HDMI2_DISP2_SUNXI=y`、`CONFIG_HDMI2_HDCP_SUNXI{,_22}=y`
- u-boot 开机 Logo：BSP u-boot `configs/orangepi_zero2w_defconfig` 里 `CONFIG_BOOT_GUI=y`、`CONFIG_DISP2_SUNXI=y`

## 8. VPU / 2D 加速

| 部件 | 源码 |
|---|---|
| Cedar VE（硬件解码） | K54: `drivers/media/cedar-ve/`；用户态：`05-gpu-mali-g31-display/libcedarc-vpu`（Allwinner libcedarc 镜像，H264/HEVC 解码） |
| G2D（2D 加速） | K54: `drivers/char/sunxi_g2d/`（含 `g2d_rcq`）；接口参考：`05-gpu-mali-g31-display/sunxi-g2d` |
| 注意 | K61/MAIN 不含厂商 cedar/g2d → 需要硬解时留在 5.4 厂商栈，或改用 GPU/软件方案 |

## 9. 音频

- 内置 codec：`sound/soc/sunxi/sun50iw9-codec.c`（K61 `CONFIG_SND_SUN50IW9_CODEC=y`）、`sun50i-codec-analog.c`、`sun50i-dmic.c`；机器侧 dts 用 `&ahub1_plat`/`&ahub1_mach`/`&ahub_dam_*`
- HDMI 音频：dw-hdmi 的 i2s 子设备（K54 侧另有 `hdmi-audio.service` 方案）
- 外部功放：AWINIC AW87xxx，固件 `03-wifi-bt-ap6256/orangepi-firmware/aw87xxx_acf.bin`；binding 见 K61/MAIN `Documentation/devicetree/bindings/sound/awinic,aw8738.yaml`
- 状态文件：`OB/external/packages/blobs/asound.state/asound.state.sun50iw9-{legacy,current,next}`；桌面 pulse 路由由 `family_tweaks_s()` 写（`hw:0,0`=Audio Codec，`hw:2,0`=HDMI）
- I2S 外挂 DAC/功放：参考 `10-community-h616-h618/opi-zero3-i2s-{5.4,6.1}`（同 H618 家族的 I2S3 补丁，思路可直接搬）

## 10. 存储 / USB / 低速外设

| 外设 | DT | 驱动 |
|---|---|---|
| TF 卡 | `&mmc0`（4bit，cd=PF6，max 50MHz） | `drivers/mmc/host/sunxi-mmc.c`（`CONFIG_MMC_SUNXI=y`） |
| 板载 16MB SPI NOR | `&spi0` + `flash@0{ compatible="jedec,spi-nor" }` | `drivers/spi/spi-sun4i.c` + `drivers/mtd/devices/m25p80.c`；厂商 u-boot 另有 `sun50iw9p1_nor_defconfig`（支持 SPI NOR 直接引导） |
| USB-C | `&usbotg{ dr_mode="peripheral" }` | `drivers/usb/musb-new/sunxi.c`（gadget：ADB / NCM / mass_storage；K54 `CONFIG_USB_SUNXI_USB_ADB=y`） |
| USB2 Host | `&ehci1/&ohci1`（USB2&3 从 24Pin/FPC 引出） | `ehci-sunxi.c`/`ohci-sunxi.c` + `drivers/phy/allwinner/phy-sun4i-usb.c` |
| IR 接收 | `&ir{ pinctrl-0=<&ir_rx_pin> }` | `drivers/media/rc/ir-sunxi.c`（K61 `CONFIG_IR_SUNXI=m`）+ `ir-keytable`/`lirc` |
| LRADC 按键 | `&r_lradc`（vref=aldo1；例：0.5V=KEY_1、0.8V=KEY_ENTER） | `drivers/input/keyboard/sun4i-lradc-keys.c`；overlay `OB/external/packages/bsp/h618/sun50i-h618-lradc-keys-{current,next}.dts` |
| 40Pin GPIO/UART/I2C/SPI/PWM | `&pio` / `&r_pio` | pinctrl + 内核子系统；用户态 `07-userspace-hal/wiringOP{,-Python}` |
| 状态灯 | PC13 green_led（heartbeat） | `gpio-leds` |

## 11. 主线（MAIN）对 Zero2W 的实际覆盖度（本地树实测）

`MAIN/arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dts` 只描述：UART0、TF(mmc0)、USB-C(peripheral)、SPI NOR、LED(PC13)、AXP313 电源域、内置 codec、GPU 节点。

**缺失（需自己补）**：`mmc1`/SDIO AP6256 Wi-Fi、`emac1`+AC200 100M 网口、IR、LRADC、HDMI connector 打开、蓝牙 UART。
**并且主线没有** AC200 的 MFD/PHY 驱动 → 需把 K61 的 `sunxi-ac200.c`/`sunxi-ephy.c` 移植过去（或等上游合入）。

## 12. 本次没拿到 / 需要外部账号的项

| 项 | 说明 |
|---|---|
| 全志 H616/H618 Datasheet & User Manual | 官方 `aw-ol.com` 文档区需登录（GitHub 无官方镜像）；日常开发靠原理图 + BSP/主线源码即可覆盖 |
| Allwinner longan SDK（`bsp/drivers/gpu` Mali 授权驱动、官方最新 `libcedarc`、Tina Linux） | 全志官网/FTP 发布；社区镜像可搜 `Mini-LinuxPC-Pro`、`TinaSDK_*` 等仓 |
| Arm Mali-Bifrost 官方 r38/r48 tarball | developer.arm.com（本机直连返回 403，需浏览器手动下载）；已用社区移植版 `mali-bifrost-h616` 覆盖 |
| OrangePi Drive 部分 PDF（用户手册 v1.3、24Pin 扩展板原理图、机械图） | Google Drive 限流；重跑 `_logs/fetch-docs-retry.py` 补齐。**整板原理图 `OPi_ZERO 2W_SCH.pdf` 已到手** |
---

## 13. 原理图实测确认（`08-docs-datasheets/orangepi-official-docs/schematic/OPi_ZERO 2W_SCH.pdf`，Xunlong “ORANGEPI Zero 2W” Rev 1.0，2023-09-26，共 7 页）

已从第 1 页（SOC1）读到的关键事实，可直接对照 dts/驱动：

| 网络名 | 含义 / 对应软件 |
|---|---|
| SoC 丝印 **H616-BGA-284** | 封装确认是 H616 系列（dts 里用 `allwinner,sun50i-h618`，同一 die） |
| `WL-SDIO-CLK/CMD/D0..D3`（PG0–PG5，11V 域） | AP6256 的 SDIO 总线 = 内核 `mmc1`；对应 dts `&mmc1{ mmc-pwrseq=<&wifi_pwrseq> }` |
| `WL-REG-ON` = **PG18**、`WL-WAKE-AP` / `AP-WAKE-BT` / `BT-WAKE-AP` | Wi-Fi 电源/唤醒：`wifi_pwrseq reset-gpios = <&pio 6 18>`；蓝牙带内唤醒（BT 驱动里 `brcm,bt-power/…` 或用户态 hciattach） |
| `BT-UART-TXD/RXD/RTS/CTS/SYNC/DOUT/DIN/PCM-*`（PB/PC 域，11V） | 蓝牙 HCI UART（内核 `ttyS1` + `btbcm`），PCM 是 BT 音频（SCO）通路 |
| `P1/RGMII-*`、`EPHY_25M`（PI 域） | AC200 内置 100M PHY 走 **RMII**（注意 dts 里 `phy-mode="rmii"`，不是 RGMII）+ 25MHz 参考时钟 |
| `PMU-SCK/PMU-SDA`（C18/B18 = PL0/PL1 RSB）、`PMU_IRQ` → **PC9** | AXP313a 挂在 **RSB**（dts `&r_i2c`，中断 PC9），不是普通 I2C |
| `SPI BOOT`：SPI0 + `SPIC_CS0` 10K 下拉、`SPI0_MOSI/MISO/CLK/CS0` | 板载 16MB SPI NOR；BOOT_SEL 引脚决定 SD/NOR 启动（厂商 u-boot 有 `sun50iw9p1_nor_defconfig`） |
| `STATUS-LED` → PC13、`PC14/PC15/PC16` | 状态灯 + 网口 link/act 灯（dts `leds` 节点） |
| `VCC-PC/PG/PI/IO` 1.8/3.3V、`DCDCE` 充电检测 | 电压域必须与 dts 里 `&pio{ vcc-pX-supply }` 一致，否则 I/O 判电平错（SDIO 1.8V 就靠 `vqmmc-supply`） |

> 预览图已渲染：`08-docs-datasheets/preview/schematic-p-1.png`、`schematic-p-2.png`
---
