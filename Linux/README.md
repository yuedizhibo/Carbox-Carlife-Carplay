# Zero2W Linux 固件

实际固件工程位于 [`zero2wsource/`](zero2wsource/)。

- 首先阅读：[BUILDING.md](BUILDING.md)
- 当前启动链修复与实板证据：[zero2wsource/BOOT-CHAIN-REPAIR.md](zero2wsource/BOOT-CHAIN-REPAIR.md)
- 源与构建入口：[zero2wsource/README.md](zero2wsource/README.md)
- 配置/符号验证报告：[zero2wsource/BUILD-REPORT.md](zero2wsource/BUILD-REPORT.md)
- 优化依据：[zero2wsource/TUNING.md](zero2wsource/TUNING.md)

构建必须在 WSL 的 Linux 文件系统中进行；Windows/DrvFS 树只作为源码与结果存放位置。所有中间产物统一输出到仓库根目录 `Temp/`，发布镜像及 SHA256 应保存到项目外的发布存储。
