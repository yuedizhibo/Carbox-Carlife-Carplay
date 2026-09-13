# CarLife 蓝牙接入（协议取证与实现）

本文记录**无线 CarLife 的蓝牙发现与配对机制**，以及我们的实现方式。
所有结论都来自仓库内已有的现成实现（`Reference/`），**没有自行发明**。

## 1. 结论先行：CarLife 靠 **HFP** 识别车机，不是 SPP，也不是 BLE 广播

手机端 CarLife 在「无线连接」里列出的车机，是**支持 HFP（Hands-Free Profile）的设备**。
车机在协议里扮演 **HFP HS（Hands-Free unit，免提设备）**，手机扮演 **AG（Audio Gateway）**。

### 证据（百度官方车机端源码）

`Reference/carlife-vehicle-lib/CarLife-Android-Vehicle/src/com/baidu/carlifevehicle/bluetooth/`

```java
// BtPairStateMachine.java —— 配对状态机（PairIdle/PairEnable/PairDisconnected/PairConnected/PairError）
private class PairEnableState extends State {
  public void enter() {
    BtHfpProtocolHelper.btOOBInfo(STATE_IDLE);      // 通过 HFP 下发 OOB 信息
  }
  public boolean processMessage(Message msg) {
    case EVENT_MD_READY:
      int connState = getHfpConnectionState();      // 判连接用的是 HFP 状态
      if (connState == BluetoothProfile.STATE_DISCONNECTED) transitionTo(mPairDisconnectedState);
      else if (connState == BluetoothProfile.STATE_CONNECTED) transitionTo(mPairConnectedState);
```

同目录配套文件全部围绕 HFP：

| 文件 | 作用 |
|---|---|
| `BtHfpManager.java` | HFP 连接管理 |
| `BtHfpProtocolHelper.java` | 下发 OOB 信息（`btOOBInfo`） |
| `CarlifeBTHfpConnectionProto.java` | HFP 连接 protobuf |
| `CarlifeBTHfpIndicationProto.java` | HFP 指示 protobuf |
| `CarlifeBTIdentifyResultIndProto.java` | 身份识别结果（手机据此确认「这是车机」） |

**反向验证**：在官方 HU 全部 Java 源码里搜 `BluetoothLeAdvertiser` / `AdvertiseData` /
`addServiceUuid` / `BluetoothLeScanner` / `startDiscovery` / `GattServer` —— **命中 0 条**。
所以 CarLife 的发现**不走 BLE 广播，也不做经典蓝牙扫描**。

### 为什么车机要发 OOB 信息

`CarlifeBTPairInfo` 的字段（官方 protobuf，字段号即定义顺序）：

| # | 字段 | 含义 |
|---|---|---|
| 1 | `ADDRESS` | 车机蓝牙地址 |
| 2 | `PASSKEY` | 配对密钥 |
| 3 | `HASH` | LE Secure Connections OOB Hash |
| 4 | `RANDOMIZER` | LE SC OOB Randomizer |
| 5 | `UUID` | 服务 UUID（下节说明） |
| 6 | `NAME` | 车机蓝牙名 |
| 7 | `STATUS` | 状态 |

即：**车机主动告知「我是谁 + 用哪个服务 + 配对凭据」**，手机端据此直接建立链路，
不需要用户去系统设置里点「连接」。

## 1.5 【重要更正】HFP 可能不是发现机制

初次阅读官方代码时得出的「CarLife 靠 HFP 认车机」结论，已被后续证据削弱，
在此保留两个方向的证据，避免又以片面结论推进。

**支持 HFP 相关的证据**（仍在）：

- 蓝牙模块整体围绕 HFP：`BtHfpManager` / `BtHfpProtocolHelper` /
  `CarlifeBTHfpConnectionProto` / `CarlifeBTHfpIndicationProto`
- `BtPairStateMachine.PairEnableState.enter()` → `BtHfpProtocolHelper.btOOBInfo()`，
  并用 `getHfpConnectionState()` 判连接

**反面证据（后来发现，很重要）**：

```java
// BtHfpProtocolHelper.java 全文
public static void btOOBInfo(int status) {
    CarlifeBTPairInfo btPairInfo = buildBluetoothInfo(status);
    sendBluetoothInfoToMd(btPairInfo);        // "MD<---HU"：HU 发给手机
}
public static void sendBluetoothInfoToMd(CarlifeBTPairInfo info) {
    CarlifeCmdMessage btCommand = new CarlifeCmdMessage(true);
    btCommand.setServiceType(CommonParams.MSG_CMD_HU_BT_OOB_INFO);  // CarLife 命令通道
    btCommand.setData(info.toByteArray());
    ConnectClient.getInstance().sendMsgToService(msgBt);            // 走 CarLife 自己的连接
}
public static CarlifeBTPairInfo buildBluetoothInfo(int status) {
    builder.setAddress(BtUtils.getBtAddress());                     // 车机蓝牙地址
    builder.setName("");                                            // 名字为空
    builder.setStatus(status);
    builder.setUuid("00001101-0000-1000-8000-00805F9B34FB");        // ← SPP，不是 HFP
    builder.setPassKey("1234");                                     // ← 固定 1234
    builder.setRandomizer("1234");
}
```

因此：

1. **OOB 信息走 CarLife 命令通道（HU→手机），不是蓝牙承载**；
2. **OOB 里告知手机使用的 UUID 是 SPP `00001101`**，不是 HFP `111E`；
3. `btStartIdentify(address)` / `CarlifeBTIdentifyResultIndProto` 表明是**手机去识别车机**；
4. `bdcf` 里的 `BLUETOOTH_INTERNAL_UI` 与 `BtHfpManager` 暗示 **HFP 主要服务于蓝牙电话**。

**保留 HFP HS 注册的理由**：车机身份的必要组成，且手机端可能据此过滤；
**但不能假定有了它手机就一定会列出车机**。

### 确定的取值（可直接采用，来自上面代码）

| 字段 | 值 |
|---|---|
| SPP UUID | `00001101-0000-1000-8000-00805F9B34FB` |
| PASSKEY | `1234` |
| RANDOMIZER | `1234` |
| NAME | 空字符串（手机用 ADDRESS 定位） |
| ADDRESS | 车机自己的蓝牙地址 |

### 下一步靠实测日志，不靠推演

`runHfServiceLevelConnection()` 会把**实际收发的每个字节**写进 `mvp.log`：

- 手机连的是 HFP 还是 SPP（会写进 `carlife.bt.note`）
- HFP 时 SLC 走到哪一步（失败会写明卡在哪条 AT）
- 手机随后发来的报文字节（即 CarLife OOB 的真实格式）

这比继续读 Java 推断可靠。

## 2. UUID 的选择

```java
// BtUtils.java
/** UUID for serial service */
static final String SPP_UUID = "00001101-0000-1000-8000-00805F9B34FB";

// BtManager.java
builder.setUuid("00001101-0000-1000-8000-00805F9B34FB");
```

- **SPP `00001101`**：CarLife 也用到（我们已注册）——用于串口通道。
- **HFP HS `0000111e`**：**车机角色（Hands-Free）** ← 识别车机的关键，此前我们缺失。
- HFP AG `0000111f`：手机角色，**不要用**。
- HSP AG `00001112`：旧版耳机规范，本场景不涉及。

> 踩过的坑：曾把 PNG 图片里的 Photoshop XMP 元数据（`…7812-117a-ad88-b5e1db18d4d8`）
> 误认为 CarLife 专用 UUID。**教训：拿到看似 UUID 的字符串必须先看上下文。**

## 3. BlueZ 侧如何注册（不装 PulseAudio / oFono）

板上实测：`ofono`、`pulseaudio`、`pipewire` **都不存在**。
BlueZ 的 `audio` 插件只在存在免提后端时才注册 HFP，因此板上**只有 AVRCP，没有 A2DP/HFP**：

```
UUID: A/V Remote Control Target (0000110c)   ← 有
UUID: A/V Remote Control        (0000110e)   ← 有
UUID: Serial Port               (00001101)   ← 我们注册的
（缺 A2DP 0000110A/110B，缺 HFP 0000111E/111F）
```

因此**我们自己用 `org.bluez.ProfileManager1` 注册**，与注册 SPP 同一套机制
（BlueZ 5 里 SDP server 归 bluetoothd 所有，旧的 `sdptool add SP` 已失效）。

**选项名与类型依据 BlueZ 官方 `doc/org.bluez.ProfileManager.rst`**：

```
void RegisterProfile(object profile, string uuid, dict options)
预定义服务：
  HFP AG UUID: 0000111f-…  Default Version 1.7, Features 0b001001, RFCOMM channel 13
  HFP HS UUID: 0000111e-…  Default Version 1.7, Features 0b000000, RFCOMM channel 7
选项：string Name / string Role / uint16 Channel / uint16 Version / uint16 Features
```

实现见：

| 文件 | 内容 |
|---|---|
| `include/carlife/spp_profile.h` + `src/spp_profile.cpp` | SPP（`00001101`，通道 1） |
| `include/carlife/hfp_profile.h` + `src/hfp_profile.cpp` | **HFP HS（`0000111e`，通道 7，Version 0x0107，Features 0x007F）** |
| `src/carlife_input.cpp` | 蓝牙引导里两条 profile **并列注册、并列轮询** |

并列轮询会把结果写进 `carlife.bt.note`（`手机已连入 HFP/SPP，开始会话握手`），
**真机测试时可直接看出 CarLife 实际使用哪条通道**。

## 4. 车机身份配置：`bdcf`

官方 README 说明车机启动时先读 `bdcf` 拿 **Channel ID**（量产需向 CarLife 官网申请）。
参考树内该文件位于 `CarLife-Android-Vehicle/assets/bdcf`，关键内容：

```
#Channel Id: Baidu will provide the channel id to all OEM
20022100                        ← 车机通道号

AUDIO_TRACK_TYPE = 0            # 0:普通双音轨
AUDIO_TRACK_NUM = 2
AUDIO_TRACK_STREAM_TYPE = 3
AUDIO_TTS_REQUEST_FOCUS = true
VOICE_MIC = 0                   # 0:使用车机端 mic
VOICE_WAKEUP = true
NEED_MORE_DECODE_TIME = false
BLUETOOTH_INTERNAL_UI = false
TRANSPARENT_SEND_TOUCH_EVENT = true
VEHICLE_GPS = false
FOCUS_UI = false
```

## 5. 正确的连接流程（多来源一致）

1. 手机与车机**蓝牙配对**（配对后手机设置里显示「已保存」是正常的 —— 见下）
2. **手机开个人热点**（或车机开热点，两者皆有实现；官方文档以手机热点为主）
3. 车机连上该热点
4. 打开**手机端 CarLife App**，它通过蓝牙链路完成 OOB/身份识别并建立会话
5. 数据通路走 **Wi-Fi TCP**（蓝牙不承载数据：「蓝牙带宽太小，仅传输声音就把带宽占满」）

> **「已保存」点不成「已连接」是 Android 的正常行为**：Android 系统蓝牙界面只对
> *可自动连接*的规范（A2DP/HFP 等）发起连接，**SPP 不在其中**，必须由 App
> 调 `createRfcommSocketToServiceRecord()` 自行建立 RFCOMM 链路。
> 所以配对成功即为正常，**不要在系统设置里期待「已连接」**。

## 6. 里程碑

| 里程碑 | 判据 | 状态 |
|---|---|---|
| SPP 注册 | 适配器 UUID 出现 `00001101` | ✅ |
| **HFP HS 注册** | 适配器 UUID 出现 `0000111e` | 待验收 |
| **CarLife 看到车机** | 手机端 CarLife 无线列表出现车机名 | 待真机 |
| CarLife 连上车机 | `carlife.bt.note` 显示已连入 HFP/SPP | 待真机 |
| 会话建立 | `bt.phase` 走到 `bringup` → `ip-ready`；`carlife.state` → `VideoStarted` | 待实现 |

## 7. 待实现

1. **HFP 通道上的 AT 指令与 SLC 建立**（BRSF/CIND/CMER 等），使手机认为 HFP 链路可用
2. **OOB 信息交换**：按 `CarlifeBTPairInfo`（address/passkey/hash/randomizer/uuid/name）
3. **身份识别应答**：`CarlifeBTIdentifyResultIndProto`
4. 采用 `bdcf` 的 Channel ID `20022100` 与音频/GPS/MIC 能力声明

## 8. 相关环境变量

板上 `$D/mvp.env`：

```
CARLIFE_ENABLE=1        # 启用 CarLife 输入
CARLIFE_BT=1            # 启用蓝牙引导（缺它则只监听 :7200，bt.phase 为空）
CARLIFE_BT_CHANNEL=1    # SPP RFCOMM 通道（HFP 固定用 7）
CARLIFE_WIFI_NAME=…     # 引导时告知手机的热点名
```

配对侧（`/etc/systemd/system/zero2w-bt-pairing.service`）：`bt-agent -c NoInputNoOutput`。
`/etc/bluetooth/main.conf` 的 `[General]`：`JustWorksRepairing = always`（Android
「取消配对后再配对」失败的官方开关）、`AlwaysPairable = true`、`PairableTimeout = 0`。
