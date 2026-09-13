# Zero2W 车载互联桥接固件

面向香橙派 Zero2W 的轻量化 Linux 固件与车载互联桥接软件。目标是在上电后 **10 秒内完成系统启动**，接收无线 CarPlay、无线 CarLife+ 等输入，经 Core 统一调度后转换或直通为原车可识别的有线 CarPlay 输出。

> 当前 Linux 生产镜像在实板测得：`multi-user.target` 7.230 s、SSH 8.075 s、systemd 完成 8.943 s；Wi-Fi 就绪 12.445 s。10 秒目标目前指系统/核心服务启动，不等于无线链路已经连接。详见 [Linux/BUILDING.md](Linux/BUILDING.md)。

## 目录

```text
Temp/                         临时脚本和临时产物；用完删除
Input/
  WirelessCarPlay/            无线 CarPlay 输入、MFi 与独立 Engine
  WirelessCarLifePlus/        无线 CarLife+ 车机端输入
Core/
  MainMenu/                   输入注册、自动/手动选择与桌面调度
  Convert/                    有界媒体记录与协议转换数据面
  Forward/                    CarPlay 输入到输出的低损耗直通（待实现）
  Web/                        网页管理、桌面、测试与媒体接口
Output/
  WiredCarPlay/               有线 CarPlay 输出接口（当前仅骨架）
Linux/                        Zero2W 轻量化 Linux 固件
Reference/                    只读参考仓库和上游 CatPlay 基座（仅本地保留，不随仓库发布）
```

完整数据流、调度规则和完成度见 [PROJECT_ARCHITECTURE.md](PROJECT_ARCHITECTURE.md)。

## 软件构建

```bash
cmake -S . -B Temp/build -DCMAKE_BUILD_TYPE=Release
cmake --build Temp/build -j$(nproc)
ctest --test-dir Temp/build --output-on-failure
```

CarLife+ 依赖 SDL2/FFmpeg，默认不随主工程构建：

```bash
cmake -S . -B Temp/build -DBUILD_WIRELESS_CARLIFE_PLUS=ON
```

上游 CatPlay Rust 工作区已完整移到 `Reference/CatPlaySource/`，不属于产品默认构建。

## 临时文件规则

- 临时脚本、构建目录、日志、抓包、镜像中间件一律放进 `Temp/`。
- 可复现配置、正式脚本、源代码和发布校验值不得放进 `Temp/`。
- 任务完成后删除对应临时目录；不要把 `Temp/` 作为归档目录。
