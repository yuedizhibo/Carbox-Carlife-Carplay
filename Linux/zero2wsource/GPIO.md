# Zero2W GPIO / 引脚使用手册（全部来自源码实测）

数据来源：`linux-orangepi-6.1-sun50iw9/arch/arm64/boot/dts/allwinner/sun50i-h616.dtsi`（pinctrl 组）、
`.../sun50i-h618-orangepi-zero2w.dts`（板级占用）、`.../overlay/*.dts`（可开启的功能）、
`OPi_ZERO 2W_SCH.pdf`（原理图网络名）。

## 1. 编号规则

Allwinner 传统编号：`gpio = 组序号*32 + 组内偏移`，A=0 B=1 C=2 D=3 E=4 F=5 G=6 H=7 I=8 L(R_PIO)=11。

| 端口 | 起始编号 | 例子 |
|---|---|---|
| PA | 0 | PA13 = 13 |
| PB | 32 | |
| PC | 64 | **PC13 = 77**（状态灯）、PC15 = 79、PC16 = 80、PC9 = 73（PMU 中断） |
| PD | 96 | |
| PE | 128 | |
| PF | 160 | **PF6 = 166**（TF 卡热插拔检测） |
| PG | 192 | **PG18 = 210**（Wi-Fi 电源/复位 WL-REG-ON） |
| PH | 224 | PH0/PH1 = 224/225（UART0 调试串口）、PH10 = 234（IR 接收） |
| PI | 256 | PI5..PI14（40Pin 上的 I2C/UART/PWM） |
| PL | 352 | PL0/PL1 = 352/353（RSB：PMU_SCK / PMU_SDA，被 AXP313 占用） |

## 2. 三种访问方式（内核已全开）

```bash
# A. 新标准 chardev（推荐，内核 CONFIG_GPIO_CDEV=y）
apt install gpiod libgpiod2 i2c-tools spi-tools   # Debian/Ubuntu
gpioinfo gpiochip0
gpioset gpiochip0 77=1            # 点亮 PC13 状态灯
gpioget gpiochip0 77
gpiomon gpiochip0 77

# B. 老 sysfs（内核 CONFIG_GPIO_SYSFS=y，wiringOP/老脚本用这个）
echo 77 > /sys/class/gpio/export && echo out > /sys/class/gpio/gpio77/direction && echo 1 > /sys/class/gpio/gpio77/value

# C. wiringOP（Arduino 风格，仓库 07-userspace-hal/wiringOP）
./wiringOP/build && sudo ./wiringOP/examples/blinkBoardZero2W   # 或 gpioRead/gpioWrite
```

> 注意：`gpiochip0` = 主 pinctrl（PA~PI），`gpiochip1` = R_PIO（PL）。用 `gpioinfo` 先确认芯片号。

## 3. 板上已被占用的引脚（不要再拿去当普通 GPIO 用）

| 引脚 | 用途 | 出处 |
|---|---|---|
| PC13 | 绿色状态 LED（默认 heartbeat） | 板级 dts `leds/led-green` |
| PC15 / PC16 | 100M 网口 link / act 灯 | 板级 dts `100m_link`、`100m_act` |
| PC9 | AXP313 PMU 中断（PMU_IRQ） | 板级 dts `&axp313a interrupts=<2 9>` |
| PF0..PF5, PF6 | TF 卡（mmc0）+ 卡检测 | `mmc0_pins`、`cd-gpios=<&pio 5 6>` |
| PG0..PG5 | AP6256 SDIO（mmc1） | `mmc1_pins` |
| PG18 | Wi-Fi/BT 电源复位（WL-REG-ON） | `wifi_pwrseq reset-gpios=<&pio 6 18>` |
| PH0 / PH1 | UART0 调试串口（115200） | `uart0_ph_pins` |
| PH10 | 红外接收（ir_rx） | `ir_rx_pin` |
| PI0 | emac0 RGMII（本板未用，走 AC200） | `ext_rgmii_pins` |
| PL0 / PL1 | RSB 总线（PMU_SCK/SDA）→ AXP313 | 原理图 + `&r_i2c` |
| SPI0 (PC0..PC3) | 板载 16MB SPI NOR | 板级 dts `&spi0 flash@0` |

## 4. 40Pin / 24Pin 可开启的功能（overlay 现成可用）

内核树 `arch/arm64/boot/dts/allwinner/overlay/` 里已带全部 dtbo，**不用重编内核**，改 `orangepiEnv.txt` 里的 `overlay_prefix`/`overlays=` 即可：

| overlay | 打开的功能 | 用到的引脚 |
|---|---|---|
| `sun50i-h616-pi-i2c0` | I2C0 | PI5(SDA)/PI6(SCL) —— 与 uart2 复用，二选一 |
| `sun50i-h616-pi-i2c1` | I2C1 | PI7/PI8 |
| `sun50i-h616-pi-i2c2` | I2C2 | PI9/PI10 —— 与 uart3 复用 |
| `sun50i-h616-pi-uart2` | UART2 | PI5/PI6 |
| `sun50i-h616-pi-uart3` | UART3 | PI9/PI10 |
| `sun50i-h616-pi-uart4` | UART4 | PI13/PI14 —— 与 pwm3/pwm4 复用 |
| `sun50i-h616-pi-pwm1` | PWM1 | PI11 |
| `sun50i-h616-pi-pwm2` | PWM2 | PI12 |
| `sun50i-h616-pi-pwm3` | PWM3 | PI13 |
| `sun50i-h616-pi-pwm4` | PWM4 | PI14 |
| `sun50i-h616-spi0-spidev` | 把 SPI0 暴露成 `/dev/spidev0.0` | 与板载 NOR 冲突，慎用 |
| `sun50i-h616-spi1-cs0-spidev` | SPI1 + CS0 | PH5(CS0)/PH6(MOSI/CLK…) |
| `sun50i-h616-spi1-cs1-spidev` | SPI1 + CS1 | PH9(CS1) |
| `sun50i-h616-usb0-host` | 把 USB-C 改成 host（OTG0） | 需外接供电 |
| `sun50i-h616-ir` | 红外接收 | PH10 |
| `sun50i-h616-gpu` | 打开 Mali-G31（板级 dts 默认 disabled） | — |
| `sun50i-h616-disable-leds` / `-zero2w-disable-led` | 释放 LED 引脚当 GPIO 用 | PC13/PC15/PC16 |
| `sun50i-h616-disable-uart0` | 释放调试串口当 GPIO 用 | PH0/PH1 |
| `sun50i-h616-ph-i2c1..4`、`-ph-uart2/5`、`-ph-pwm12/34` | 24Pin/FPC 上的 I2C/UART/PWM | PH0..PH9 |

另外 `i2s3`（PH5/PH6/PH7…）在 dtsi 里有 pin 组，做外接 DAC 时参考 `10-community-h616-h618/opi-zero3-i2s-6.1` 的补丁写法。

## 5. 想加自己的板载外设

1. 复制 `sun50i-h618-orangepi-zero2w.dts` → `myboard.dts`，改 `model`/`compatible`，加节点；
2. 小改动优先写 overlay（参考上表任一 dtbo 的 `target = <&xxx>` 语法），放到 `arch/arm64/boot/dts/allwinner/overlay/` 并在 `Makefile` 加一行；
3. `bash scripts/wsl-03-build-kernel.sh`（或只跑 `build-dtb.sh`）→ 换 `/boot` 里的 dtb 与 dtbo 即可，不必重刷镜像。

## 6. 内核里必须开的项（本次已全部打开）

```
CONFIG_PINCTRL_SUN50I_H616=y   CONFIG_PINCTRL_SUN50I_H616_R=y   CONFIG_GPIOLIB=y
CONFIG_GPIO_SYSFS=y            CONFIG_GPIO_CDEV=y               CONFIG_GPIO_CDEV_V1=y
CONFIG_OF_GPIO=y               CONFIG_LEDS_GPIO=y               CONFIG_KEYBOARD_GPIO=y
CONFIG_KEYBOARD_SUN4I_LRADC=y  CONFIG_IR_SUNXI=y                CONFIG_I2C_CHARDEV=y
CONFIG_SPI_SPIDEV=y            CONFIG_PWM_SUN4I=m               CONFIG_OF_OVERLAY=y
```