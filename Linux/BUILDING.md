# Zero2W Linux 固件可复现流程

## 1. 当前选定方案

- SoC/板卡：Orange Pi Zero2W，Allwinner H618。
- 内核：Orange Pi/Allwinner BSP 6.1，源目录 `zero2wsource/01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9`，记录提交 `71144529b0334d1488624c41d0d3ba0cb03dd4c1`。
- 版本：`6.1.31-opi-min`。
- Rootfs：已有 Debian 双分区镜像（FAT boot + ext4 root），不是当前 Buildroot 输出。
- 启动：无 initrd，`booti ${kernel_addr_r} - ${fdt_addr_r}`；根分区优先按第二分区 PARTUUID 定位。
- 选择 BSP 而非纯主线的原因：当前产品需要板载 SDIO Wi-Fi 与 AC200 以太网支持。

源仓库 URL/提交以 `zero2wsource/manifest.json` 为准。最终产品约束以 `scripts/wsl-15-verify-no-initrd-image.sh` 和选定 bundle 的 `kernel.config` 为准；旧说明与验证脚本冲突时，不得依据旧说明发布。

## 2. 环境与源码

不要直接在 Windows checkout 编译。将 LF 源准备到 WSL 的 ext4 文件系统：

```bash
cd /mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource
bash scripts/wsl-01-setup-env.sh
bash scripts/wsl-04-prepare-lf.sh
bash scripts/wsl-05-get-kernel-tarball.sh
```

`wsl-01` 安装工具，`wsl-04` 创建 LF 工作副本，`wsl-05` 准备内核源归档。每次全新复现应核对 `manifest.json` 中的提交，不要用浮动分支替代。

## 3. 最小内核

```bash
bash scripts/wsl-10-minimize-config.sh
VARIANT=minimal bash scripts/wsl-11-build-minimal.sh
bash scripts/wsl-13-verify-minimal.sh
bash scripts/wsl-12-finalize-minimal.sh
```

必须保留并评审：

- `configs/zero2w-minimal.spec`
- `configs/zero2w-minimal_defconfig`
- `configs/zero2w-minimal.config`
- `configs/zero2w-minimal.stats.txt`
- `configs/zero2w-headless-mfi.spec`

严格验证不可省略：Kconfig 依赖可能让配置项隐藏，简单文本检查会把“依赖未满足”误判为“已正确关闭”。

最终 kernel bundle 应含 `Image`、`Image.gz`、`System.map`、DTB、`kernel.config`、`modules-6.1.31-opi-min.tar.gz`、模块 SHA256、`artifact-manifest.sha256` 和 `provenance.txt`，且内核与模块 ABI 必须一致。

## 4. 组装无 initrd 镜像

```bash
mkdir -p /mnt/d/littlethings/CarPlay/zero2w/Temp/LinuxBuild
bash scripts/wsl-14-assemble-no-initrd-image.sh \
  /path/to/immutable-debian-source.img \
  /mnt/d/littlethings/CarPlay/zero2w/Temp/LinuxBuild/zero2w-no-initrd.img
IMAGE_PROFILE=production bash scripts/wsl-15-verify-no-initrd-image.sh \
  /mnt/d/littlethings/CarPlay/zero2w/Temp/LinuxBuild/zero2w-no-initrd.img
bash scripts/wsl-16-optimize-production-image.sh \
  /mnt/d/littlethings/CarPlay/zero2w/Temp/LinuxBuild/zero2w-no-initrd.img \
  /mnt/d/littlethings/CarPlay/zero2w/Temp/LinuxBuild/zero2w-production.img
```

`wsl-14` 复制源镜像后只修改副本，安装 kernel/DTB/modules 与重建的 `boot.scr`，不会替换镜像中已有 SPL/U-Boot。因此最终镜像的可复现性还依赖源镜像本身含有已验证 bootloader；发布时必须记录源镜像 SHA256 和 bootloader 来源。

开发/AP 派生镜像分别用 `wsl-17-create-sub10-development-image.sh`、`wsl-18-create-sub10-ap-development-image.sh`，不能冒充 production。

## 5. 刷写和硬件验证

刷写工具位于 `zero2wsource/scripts/flash-tf-*.ps1`，验证用 `verify-tf-card.ps1`。刷写前后均记录镜像与介质校验值。

结构校验不等于实板启动证明。每个发布镜像必须采集 UART0 115200 日志，并确认：

1. 最终 kernel command line 正确；
2. 出现 `VFS: Mounted root`，或记录第一个 MMC/ext4 错误；
3. `systemd-analyze time`、`critical-chain`；
4. Core 服务首次 ready 时间；
5. Wi-Fi、蓝牙、USB gadget、MFi I²C 的独立 ready 时间。

已有 production 实板记录：multi-user 7.230 s、SSH 8.075 s、systemd 8.943 s、Wi-Fi 12.445 s。因此“10 秒完成”目前只对系统与核心服务成立，无线连接尚未达到 10 秒。

## 6. 清理与发布

构建缓存、挂载目录、`.img` 中间件和日志全部放 `Temp/LinuxBuild/`，验证后删除。只将最终镜像、SHA256、manifest、UART 日志和测量报告复制到项目外的版本化发布存储。不要把数十 GB 的 `build-out*` 再放回源码目录。
