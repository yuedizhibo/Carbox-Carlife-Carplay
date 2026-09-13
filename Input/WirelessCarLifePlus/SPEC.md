# CarLife+ 车机端协议事实（zero2w 实现依据）

本文只记录**从参考实现里逐行核对出来的事实**，并给出可追溯的源码位置。参考代码在
`../../Reference/apollo-DuerOS/`（Apache-2.0，官方 DuerOS 车机方案快照）与
`../../Reference/carlife-vehicle-lib/`（协议 V0.15 的 C++ 车机库）。

缩写：HU = 车机（本实现），MD = 手机端。

## 1. 通道与端口

SocketCommunicator 模型：每条通道一条独立 TCP 连接，帧头内不再带通道号。

| 通道 | id | MD 侧监听（无线，HU 主动连） | HU 侧监听（有线/adb 转发） |
|---|---:|---:|---:|
| CMD | 1 | 7240 | 7200 |
| VIDEO | 2 | 8240 | 8200 |
| MEDIA（音乐） | 3 | 9240 | 9200 |
| TTS（导航播报） | 4 | 9241 | 9201 |
| VR（语音上行） | 5 | 9242 | 9202 |
| TOUCH | 6 | 9340 | 9300 |
| UPDATE | 7 | 9440 | 9400 |

来源：`carlife-sdk/.../receiver/transport/wirless/WirlessConnector.kt:20-26`、
`.../internal/protocol/Constants.kt(MSG_CHANNEL_*)`、
老库 `CarLife-Vehicle-Lib/LibSource/core/CConnectManager.h:32-44`、
`vehicle-app/.../CommonParams.java:487-494`。
`ConnectManager` 的 `IOS_WIFI_CONNECTION / ADB_CONNECTION` 开关说明官方就区分"WiFi"与"USB"两种承载。

## 2. 帧格式（全大端）

`BitConverter.kt`：`toShort/toInt/fillByteArray` 均为 big-endian。

- CMD / TOUCH 通道，头 8 字节：
  `[0..1] 载荷长度(u16)` `[2..3] 保留(0)` `[4..7] serviceType(i32)` `[8..] protobuf`
- 其余通道，头 12 字节：
  `[0..3] 载荷长度(i32)` `[4..7] 时间戳(i32)` `[8..11] serviceType(i32)` `[12..] 数据`
- AOA/USB 单通道额外前置 8 字节 `[channel i32][size i32]` 用于分通道（`CarLifeMessage.header()`）。
- CMD/TOUCH 载荷长度只有 16 位 → 单条控制消息上限 65535 字节（本实现在 `Transport::send` 里做了拒绝）。

## 3. 建会话与鉴权（`receiver/protocol/v1/ConnectionEstablishHandler.kt`）

```
HU→MD  HU_PROTOCOL_VERSION      0x00018001  {major=0, minor=1}
MD→HU  PROTOCOL_VERSION_MATCH_STATUS 0x00010002 {matchStatus=1, carlifeProtocolVersion=N}
HU→MD  STATISTIC_INFO           0x00018027  CarlifeStatisticsInfo{cuid,versionName,versionCode,channel,connectCount,connectSuccessCount,connectTime}
MD→HU  MD_INFO                  0x00010004  CarlifeDeviceInfo
HU→MD  HU_INFO                  0x00018003  CarlifeDeviceInfo（含 btaddress=23、carlifeversion=24）
HU→MD  HU_AUTH_REQUEST          0x00018048  CarlifeAuthenRequest{randomValue}
        randomValue = SDK_VERSION_CODE + ";" + connectTime   // "2.0;HH:mm:ss"，不是随机数
MD→HU  MD_AUTH_RESPONSE         0x00010049  CarlifeAuthenResponse{encryptValue}
HU 本地 seed = MD.brand + MD.model + MD.sdk + connectTime
        EncryptionUtils.getVerifyResult(seed, encryptValue)   // 闭源 libencryption.so
HU→MD  HU_AUTH_RESULT           0x0001804A  CarlifeAuthenResult{authenResult}
MD→HU  MD_AUTH_RESULT           0x0001004B  CarlifeAuthenResult{authenResult}
HU→MD  MD_AUTH_RESULT_RESPONSE  0x0001804C  （无载荷）
```
超时/失败：`HU_AUTH_REQUEST` 发出后 30s 无响应 → HU 断开；`authenResult=false` → HU 5 秒后断开（源码 line 188-196 / 224-226）。
消息 ID 的 0x8000 位约定：置位 = HU 发出，未置位 = MD 发出。

**鉴权方向**：是"车机验手机"。车机只需在 `HU_AUTH_RESULT` 自报布尔，协议里没有要求车机向手机提交任何由闭源 .so 产出的证明。

**内容加密（与鉴权无关，可关）**：`MD_RSA_PUBLIC_KEY_REQUEST 0x0001006A` → `HU_RSA_PUBLIC_KEY_RESPONSE 0x0001806B` → `MD_AES_KEY_SEND_REQUEST 0x0001006C` → `HU_AES_REC_RESPONSE 0x0001806D` → `MD_ENCRYPT_READY 0x0001006E` / `MD_ENCRYPT_READY_DONE 0x0001806F`。
车机在 `CarLifeReceiverImpl.kt:245-259` **运行时自己生成 RSA-2048**，手机回传 AES key；`MessageDispatcher.kt:57/114` 只对 `supportEncrypt && isEncryptionEnabled` 的消息加解密，`CarLifeContextImpl.kt:124` 默认 `false`；`HU_PROTOCOL_VERSION` 永不加密。能力位 `FEATURE_CONFIG_CONTENT_ENCRYPTION`。

**激活（真正的商务授权点）**：`MSG_CMD_BOX_ACTIVE 0x00016001`（转换盒发送）/ `MSG_CMD_HU_ACTIVE 0x00016002`（激活端发送），载荷 `CarlifeActiveRequest{mac,isActive,randomValue}` / `CarlifeActiveResponse{statue,token}`；`token` 之后随 `CarlifeDeviceInfo.token(22)` 上报。公开代码里只有 `PayloadDecoderFactory.kt:17-19` 注册了解码器，**没有任何 handler** → 判定在闭源 SDK 内。

## 4. 蓝牙引导（无线的"第一条腿"）

链路：经典蓝牙 SPP，`Constants.kt: BLUETOOTH_COMMUNICATE_UUID = 00001101-0000-1000-8000-00805F9B34FB`。
链路上跑 **CMD 通道短头帧**（`BluetoothCommunicator.kt` 直接用 `CarLifeMessage` 收发）。

`receiver/transport/instant/InstantConnectionSetup.kt`：

```
MD→HU  MSG_WIRELESS_INFO_REQUEST        0x00100001
HU→MD  MSG_WIRELESS_INFO_RESPONSE       0x00108002  CarlifeWirlessInfo{wirlessType=1, wifiFrequency=2}
MD→HU  MSG_WIRELESS_TARGET_INFO_REQUEST 0x00100004
HU→MD  MSG_WIRELESS_TARGET_INFO_RESPONSE 0x00108005 CarlifeWirlessTarget{wifiDeviceName=1, targetInfo=2}
HU→MD  MSG_WIRELESS_REQUEST_IP          0x00108006  （空载荷）
MD→HU  MSG_WIRELESS_RESPONSE_IP         0x00100007  CarlifeWirlessIp{wirlessip=1}
        -> onDeviceConnected(ip)  然后 HU 去连该 IP 的 7240/8240/...
MD→HU  MSG_WIRELESS_MD_STATUS           0x00100008  CarlifeWirlessStatus{status=1}
HU→MD  MSG_WIRELESS_HU_STATUS           0x00108009
```
类型常量：`TYPE_NONE=0, TYPE_WIFI=1, TYPE_WIFI_DIRECT=2, TYPE_ALL=3`；`FREQUENCY_2_4G=0, FREQUENCY_5G=1`。
即：**手机先经蓝牙把车机"认领"下来，拿到手机在 WiFi 上的地址，然后所有媒体/控制走 TCP。**

## 5. 视频 / 音频 / 触控

- HU 建会话后立即发 `VIDEO_ENCODER_INIT 0x00018007` = `CarlifeVideoEncoderInfo{width=1,height=2,frameRate=3}`；
  手机回 `VIDEO_ENCODER_INIT_DONE 0x00010008`；HU 再发 `VIDEO_ENCODER_START 0x00018009`
  （`RemoteDisplayRenderer.kt:126-139`、`ConnectionEstablishHandler.kt:64-70`）。
- 视频数据：VIDEO 通道 `MSG_VIDEO_DATA 0x00020001`，载荷 = H.264 Annex-B（一个 access unit 一条消息，
  `FrameDecoder.kt` 直接把 payload 喂 MediaCodec，并把同批数据写 `.h264` 文件）。
- 心跳：HU 在 VIDEO 通道周期发 `MSG_VIDEO_HEARTBEAT 0x00020002`；无响应 10s 判超时（`ConnectionEstablishHandler.kt:40-49,124`）。
- 手机要求重编码：`MD_VIDEO_ENCODER_REQ 0x0001000F`；帧率变更 `VIDEO_ENCODER_FRAME_RATE_CHANGE 0x0001800C` / `..._DONE 0x0001000D`。
- 音频：MEDIA 通道 `MSG_MEDIA_INIT 0x00030001` = `CarlifeMusicInit{sampleRate,channelConfig,sampleFormat}`，
  `MSG_MEDIA_DATA 0x00030006`（PCM16 时 `sampleFormat=2`）；`STOP/PAUSE/RESUME/SEEK = 0x00030002..5`。
  TTS 通道 `0x00040001/0x00040003/0x00040004`；VR 通道 `0x00050002/0x00050003/0x00058001`。
- 触控（HU→MD，TOUCH 通道，8 字节短头）：`0x00068002 DOWN` / `0x00068004 MOVE` / `0x00068003 UP`，
  载荷 `CarlifeTouchSinglePoint{x,y,pointerx,pointery}`；硬键 `0x00068008` = `CarlifeCarHardKeyCode{keycode}`。
  手机侧键（CMD 通道）`MSG_TOUCH_PAD_DOWN/MOVE/UP/PINCH = 0x0001005A..5D`。
- 模块状态（MD→HU）`MSG_CMD_MODULE_STATUS 0x00010026` = `CarlifeModuleStatusList{cnt, moduleStatus[]{moduleID,statusID}}`，
  模块 ID：PHONE=1 NAVI=2 MUSIC=3 VR=4 CONNECT=5 MIC=6 CRUISE=8 CRUISE_FOLLOW=9。
- 能力协商：`MD_FEATURE_CONFIG_REQUEST 0x00010051` ↔ `HU_FEATURE_CONFIG_RESPONSE 0x00018052`，
  载荷 `CarlifeFeatureConfigList{cnt=1, featureConfig[]{key=1,value=2}=2, huBtAudioSupport=3, huBtName=4, huBtMAC=5}`；
  能力键全集 19 个（`CONTENT_ENCRYPTION`、`CONNECT_TYPE`、`USB_MTU`、`MULTI_TOUCH`、`I_FRAME_INTERVAL`、
  `AAC_SUPPORT`、`AUDIO_TRANSMISSION_MODE`、`MEDIA_SAMPLE_RATE`、`VOICE_MIC`、`VOICE_WAKEUP`、
  `BLUETOOTH_AUTO_PAIR`、`BLUETOOTH_INTERNAL_UI`、`FOCUS_AREA_AUTO_SET`、`FOCUS_UI`、`INPUT_DISABLE`、
  `MUSIC_HUD`、`ENGINE_TYPE`、`FEATURE_CONFIG_REQUEST/RESPONSE`）。

## 6. 随附库与 Linux 适配（2026-09-13 更新）

`libs/encryption.jar`（2 个 class，纯 JNI 壳）+ `jniLibs/{arm64-v8a,armeabi-v7a,x86,x86_64}/libencryption.so`，
只导出 `getVerifyCode`、`getVerifyResult`，二进制内无 PEM 私钥。
产品现已提供接收端 `SdkPhoneVerifier`，默认 `--auth sdk`，按随附库的
`getVerifyResult` 摘要路径验证手机；来源、UTF-8/JNI边界及验证范围见
`Input/API_AUDIT/AUTH.md`。没有直接将 Android JNI 库链接到 Debian。
`--auth dev` 的 FNV-1a 摘要（`sim-<16hex>`）和 `--auth trust` 仍保留供显式模拟器测试，
不再是生产默认。此校验不包含转换盒激活 token 发放或 Android 应用签名检查。
