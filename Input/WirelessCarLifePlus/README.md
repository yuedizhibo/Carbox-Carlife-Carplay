# CarLife+ 车机端 · Linux 版（zero2w）

无线 CarLife+ 的车机端（HU）实现：手机通过**蓝牙**认下车机 → 拿到手机在 WiFi 上的地址 →
建立 7 条 TCP 通道 → 车机与手机完成协议/鉴权/能力协商 → 手机推 H.264 + PCM →
车机硬/软解码并**显示画面**（默认协商分辨率 **1920x1080@30**），触控与硬键回传手机。

协议事实与源码出处全部写在 [SPEC.md](SPEC.md)；**蓝牙接入（CarLife 靠 HFP 认车机、UUID 选择、
`bdcf` 车机身份）见 [BLUETOOTH.md](BLUETOOTH.md)**；参考代码在 `../../Reference/`
（`apollo-DuerOS` 的 `CarLife-Android-Vehicle-V2.0/carlife-sdk` 是权威依据，
`carlife-vehicle-lib` 提供 V0.15 C++ 车机库与端口表，`FastCarPlay` / `LIVI` /
`carlinkit-cxx` 提供 Linux 侧解码与显示的工程参考）。

## 目录结构

```
Input/WirelessCarLifePlus/
├── SPEC.md                  协议事实（逐条带源码位置）
├── BLUETOOTH.md             CarLife 蓝牙接入：HFP 发现机制、UUID、BlueZ 注册、bdcf、里程碑
├── CMakeLists.txt           C++17；依赖 SDL2 + libavcodec/libavutil/libswscale（不需要 libprotobuf）
├── include/carlife/
│   ├── service_types.h      通道/端口/全部消息 ID/能力键
│   ├── wire.h               大端帧头 + 手写 protobuf 编解码
│   ├── transport.h          7 通道 TCP + UDP 7999 发现
│   ├── bt_link.h            蓝牙引导（SPP/RFCOMM，跑 CMD 短头帧）
│   ├── spp_profile.h        BlueZ ProfileManager1 注册 SPP(1101)
│   ├── hfp_profile.h        BlueZ ProfileManager1 注册 HFP HS(111E) —— CarLife 认车机的入口
│   ├── wifi_ap.h            车机侧热点计划（hostapd + dnsmasq）
│   ├── session.h            会话状态机
│   └── render.h             H.264 解码 + SDL 显示 + 快照 + PCM 播放
├── src/ * 同名实现
├── tools/mdsim/mdsim.py     手机端（MD）模拟器：完整扮演手机，喂真 H.264 流
└── tests/
    ├── unit_wire.cpp        42 项协议层断言（帧头/protobuf/未知字段跳过/round-trip）
    ├── unit_ap.cpp          8 项热点配置断言
    └── run_wsl_tests.sh     WSL 一键端到端测试（下面「测试」一节）
```

## 构建

```bash
sudo apt install -y build-essential cmake pkg-config libsdl2-dev \
     libavcodec-dev libavutil-dev libswscale-dev ffmpeg
cmake -S . -B build && cmake --build build -j$(nproc)
```
产物：`build/carlife-hu`（车机端）、`build/unit-wire`、`build/unit_ap`→`unit-ap`。

## 运行

```bash
# 1) 完整无线流程：车机开热点 + 蓝牙(SPP)引导 + 拿手机 IP + 显示画面
./build/carlife-hu --wifi-ap wlan0 --ssid zero2w-CarLife --psk carlife12345 \
                   --bt-rfcomm 1 --bt-advertise --width 1920 --height 1080 --fps 30

# 2) 已知手机地址时直接建链（跳过蓝牙）
./build/carlife-hu --phone-ip 192.168.43.x

# 3) 用 UDP 7999 发现报文的源地址当手机地址（与本项目 Android 侧 R100 的做法一致）
./build/carlife-hu --discover

# 4) 有线 / adb 转发拓扑：车机在 7200/8200/9200/9201/9202/9300 监听
./build/carlife-hu --listen

# 5) 无蓝牙硬件时的对拍/CI（AF_UNIX 承载同一套蓝牙帧；路径必须放 /tmp，不能放 /mnt/*）
./build/carlife-hu --bt-local /tmp/carlife-bt.sock --retry-seconds 12

# 只看热点配置方案（dry-run，不碰网卡）
./build/carlife-hu --print-ap-plan --wifi-ap wlan0 --wifi-band 5 --ssid AP --psk 12345678
```

常用开关：`--auth trust|dev|deny`（鉴权策略）、`--min-frames N`、`--seconds N`、
`--snapshot-dir DIR`（出图快照）、`--headless`、`--inject-touch X,Y`（千分比触控自测）、`--verbose`。

## 测试（WSL Debian 上实测通过）

```bash
./tests/run_wsl_tests.sh
```

6 个阶段全部 PASS，要点：

| 阶段 | 内容 | 结果 |
|---|---|---|
| 1 | 构建（carlife-hu / unit-wire / unit-ap） | PASS |
| 2 | 协议层单测：帧头 8/12 字节布局、大端、protobuf 与 golden 字节、未知字段跳过、各消息 round-trip | **42 passed, 0 failed** |
| 2b | 车机侧 WiFi 热点：`hw_mode=a/g`、SSID、WPA2 口令、dnsmasq 地址段 | PASS（`unit-ap: 8 passed`） |
| 3 | `ffmpeg` 生成真实 1920x1080@30 H.264 码流（5,192,841 B / 180 AU） | PASS |
| 4 | **无线端到端**：手机模拟器 + 车机 + 鉴权往返 + 协商 1920x1080@30 + 解码显示 + 心跳 + 能力协商 + 触控回传 | **framesDecoded=40，videoBytes=1,251,875，audioBytes=42,336，RESULT: OK** |
| 4b | 对车机实际渲染的帧做 BMP 快照并按字节校验 | **1920x1080，6,220,854 字节，采样 31 种颜色（真实画面，非纯色）** |
| 5 | 有线/adb 转发拓扑（车机侧监听 7200/8200/…）+ 鉴权失败负向路径 | PASS（framesDecoded=30；负向：判失败→按协议 5 秒主动断开→无画面） |
| 6 | **蓝牙引导 → 完整无线链路**：SPP 链路上 4 步 `WIRELESS_INFO / TARGET_INFO / REQUEST_IP / RESPONSE_IP / MD_STATUS`，拿到手机 IP 后建 7 通道出画面 | PASS（framesDecoded=30，displayResolution=1920x1080） |

日志与快照在 `out/`：`hu-wireless.log`、`hu-bt.log`、`sim-*.log`、`snaps-*/snapshot-*.bmp`、
`displayed-1920x1080.png`（车机显示内容的 PNG）。

## 已实现

- 7 通道 TCP（无线：主动连手机 7240/8240/9240/9241/9242/9340/9440；有线：本端监听 7200…）；
- 蓝牙引导：SPP/RFCOMM 上的 4 步无线握手与 IP 交换（`--bt-rfcomm`），以及 AF_UNIX 桥接模式（`--bt-local`）；
- BlueZ 侧尽力而为的广播（`hciconfig`/`btmgmt`/`sdptool`）；
- 车机侧 WiFi 热点：hostapd + dnsmasq 配置生成与拉起/降级（`--wifi-ap`、`--print-ap-plan`）；
- 会话状态机：协议版本协商 → 统计信息 → MD/HU 设备信息 → 鉴权四步（含 30s/5s 超时规则）→
  能力协商（19 个 `FEATURE_CONFIG_*`）→ `VIDEO_ENCODER_INIT(1920x1080@30)` → INIT_DONE → START →
  收流解码显示；VIDEO 通道 1s 心跳 + 10s 超时；
- 视频：H.264 Annex-B → libavcodec → RGBA → SDL 窗口（WSLg 真窗口，支持等比居中/缩放）；
- 音频：Media 通道 `MEDIA_INIT` + PCM16 播放（TTS/VR 通道消息已识别并记录）；
- 触控与硬键：鼠标点击 → `TOUCH_ACTION_DOWN/UP`（`CarlifeTouchSinglePoint`，按协商分辨率换算）；
  键盘方向键/确认/返回 → `TOUCH_CAR_HARD_KEY_CODE`；
- 帧快照（无显示环境也能验证"真的出图了"）。

## 待办 / 已知边界

1. **真机验证**：这里没有蓝牙适配器和真实手机，蓝牙引导与热点是用"同一套帧 + 桥接链路"验证的；
   上板（Pi Zero 2 W + BT/WiFi，或 x86 笔记本）后需要跑一次真手机连接，确认
   `authenResult` 自判在手机侧被接受（协议里车机就是裁判，但手机侧第 8 步是否还有独立校验只能实测）。
2. **激活**：`MSG_CMD_BOX_ACTIVE/HU_ACTIVE` + `CarlifeActiveRequest{mac,isActive,randomValue}` →
   `CarlifeActiveResponse{statue,token}` 的判定在闭源 `libencryption.so` 里，公开代码没有 handler；
   需要时再逆那颗 .so（有 x86_64 预编译版，可在 Linux 上黑盒采样）。
3. **内容加密**：目前按 `FEATURE_CONFIG_CONTENT_ENCRYPTION=0` 走明文（协议自带分支）。
   若要开：车机自生成 RSA-2048 + AES，参考 `CarLifeReceiverImpl.kt:245-259`。
4. **AAC / 编码音频**：`sampleFormat != 2` 时只计数并告警，尚未接解码器。
5. **VR 上行麦克风**：消息已识别，未采集麦克风。
6. **iOS/USB 承载**：`NCMProtocolTransport` / `AOAProtocolTransport`（AOAP 8 字节分通道头）未实现，
   当前覆盖 WiFi 与"adb 转发端口"两种拓扑。
7. **iOS 触控板/多点触控**：单点已通，多点与 `MSG_TOUCH_PAD_*`（CMD 通道 0x0001005A..5D）待补。