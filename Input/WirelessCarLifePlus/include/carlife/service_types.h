// CarLife+ 车机端 (head unit) 协议常量
//
// 全部取值逐一取自参考实现，来源见 SPEC.md：
//   通道枚举/SDK 版本 : apollo-DuerOS/CarLife-Android-Vehicle-V2.0/carlife-sdk/.../sdk/Constants.kt
//   消息 ID          : .../sdk/internal/protocol/ServiceTypes.kt
//   无线端口         : .../sdk/receiver/transport/wirless/WirlessConnector.kt:20-26
//   车机侧端口       : carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/core/CConnectManager.h:32-44
#pragma once
#include <cstdint>

namespace carlife {

// SDK_VERSION_CODE = "2.0" (Constants.kt)。鉴权 randomValue = "<该值>;<HH:mm:ss>"
constexpr const char* kSdkVersionCode = "2.0";
constexpr int32_t kProtocolMinorVersion = 1;      // PROTOCOL_VERSION_MINOR_VERSION

// MSG_CHANNEL_* —— SocketCommunicator 每条通道一个 TCP 连接
namespace ch {
constexpr int32_t CMD = 1;
constexpr int32_t VIDEO = 2;
constexpr int32_t AUDIO = 3;   // Media 通道
constexpr int32_t TTS = 4;
constexpr int32_t VR = 5;
constexpr int32_t TOUCH = 6;
constexpr int32_t UPDATE = 7;
constexpr int32_t COUNT = 8;   // 索引 1..7
}  // namespace ch

namespace port {
// 无线：手机侧(MD)监听，车机主动 connect —— WirlessConnector.kt
constexpr int MD_CMD = 7240;
constexpr int MD_VIDEO = 8240;
constexpr int MD_AUDIO = 9240;
constexpr int MD_TTS = 9241;
constexpr int MD_VR = 9242;
constexpr int MD_TOUCH = 9340;
constexpr int MD_UPDATE = 9440;
// 有线/adb 转发：车机侧监听 —— CConnectManager.h / CommonParams.java:487-494
constexpr int HU_CMD = 7200;
constexpr int HU_VIDEO = 8200;
constexpr int HU_AUDIO = 9200;
constexpr int HU_TTS = 9201;
constexpr int HU_VR = 9202;
constexpr int HU_TOUCH = 9300;
constexpr int HU_UPDATE = 9400;
}  // namespace port

namespace msg {
// 约定：ID 的 0x8000 位置位 = 车机(HU)发出；未置位 = 手机(MD)发出
constexpr uint32_t PROTOCOL_VERSION_MATCH_STATUS = 0x00010002;  // MD->HU
constexpr uint32_t HU_INFO = 0x00018003;                        // HU->MD (CarlifeDeviceInfo)
constexpr uint32_t MD_INFO = 0x00010004;                        // MD->HU (CarlifeDeviceInfo)
constexpr uint32_t HU_PROTOCOL_VERSION = 0x00018001;            // HU->MD (首条消息)
constexpr uint32_t VIDEO_ENCODER_INIT = 0x00018007;             // HU->MD
constexpr uint32_t VIDEO_ENCODER_INIT_DONE = 0x00010008;        // MD->HU
constexpr uint32_t VIDEO_ENCODER_START = 0x00018009;            // HU->MD
constexpr uint32_t VIDEO_ENCODER_PAUSE = 0x0001800A;
constexpr uint32_t VIDEO_ENCODER_RESET = 0x0001800B;
constexpr uint32_t VIDEO_ENCODER_FRAME_RATE_CHANGE = 0x0001800C;
constexpr uint32_t VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE = 0x0001000D;
constexpr uint32_t MD_VIDEO_ENCODER_REQ = 0x0001000F;           // MD->HU 要求关键帧
constexpr uint32_t NAV_NEXT_TURN_INFO = 0x00010030;
constexpr uint32_t CAR_DATA_SUBSCRIBE_RSP = 0x00018032;
constexpr uint32_t MEDIA_INFO = 0x00010035;
constexpr uint32_t MEDIA_PROGRESS_BAR = 0x00010036;
constexpr uint32_t CONNECT_EXCEPTION = 0x00010037;
constexpr uint32_t REQUEST_GO_TO_FOREGROUND = 0x00010038;
constexpr uint32_t UI_ACTION_SOUND = 0x00010039;
constexpr uint32_t BT_HFP_INDICATION = 0x00018041;
constexpr uint32_t HU_AUTH_REQUEST = 0x00018048;
constexpr uint32_t MD_AUTH_RESPONSE = 0x00010049;
constexpr uint32_t HU_AUTH_RESULT = 0x0001804A;
constexpr uint32_t MD_AUTH_RESULT = 0x0001004B;
constexpr uint32_t MD_AUTH_RESULT_RESPONSE = 0x0001804C;
constexpr uint32_t START_BT_AUTO_PAIR_REQUEST = 0x0001004D;
constexpr uint32_t BT_HFP_STATUS_REQUEST = 0x0001004F;
constexpr uint32_t BT_HFP_STATUS_RESPONSE = 0x00018050;
constexpr uint32_t MD_FEATURE_CONFIG_REQUEST = 0x00010051;
constexpr uint32_t HU_FEATURE_CONFIG_RESPONSE = 0x00018052;
constexpr uint32_t ERROR_CODE = 0x00018055;
constexpr uint32_t MD_EXIT = 0x00010059;
constexpr uint32_t HU_CONNECT_STATISTIC = 0x00018060;
constexpr uint32_t TIME_SYNC = 0x00010060;

// ── 以下是本轮「全量接口」补齐的消息 ID ──
// 全部逐条取自 ServiceTypes.kt（与 CommonParams.java 交叉核对一致）。
// 约定不变：0x8000 位置位 = 车机(HU)发出。
constexpr uint32_t HU_BT_OOB_INFO = 0x00018005;              // HU->MD (CarlifeBTPairInfo)
constexpr uint32_t MD_BT_OOB_INFO = 0x00010006;              // MD->HU (CarlifeBTPairInfo)
constexpr uint32_t DEVICE_VERSION_INFO = 0x00010068;         // MD->HU
constexpr uint32_t ASR_VERSION_MATCH = 0x00010062;           // MD->HU

// 激活（AUDIT 3.1 / 5.1 #10）：厂家商务授权。request 由手机侧发起。
constexpr uint32_t BOX_ACTIVE = 0x00016001;                  // MD->HU (CarlifeActiveRequest)
constexpr uint32_t HU_ACTIVE = 0x00016002;                   // HU->MD (CarlifeActiveResponse)

// 车辆数据订阅与上报（AUDIT 3.6 / 5.1 #8）：两条独立路径，都要实现。
// 路径一 CAR_DATA_*：手机订阅具体的车况模块（速度/GPS/…）；
// 路径二 CARLIFE_DATA_*：官方 SDK 的 FeaturesHandler 走的那条（收发订阅表）。
//
// 【ID 的权威来源】以百度官方 C++ 车机库的枚举为准（和 Kotlin 的 ServiceTypes.kt 交叉核对）：
//   Reference/.../CarLife-Vehicle-Lib/LibSource/include/CTranRecvPackageProcess.h:119-146
// 两者不一致时以 C++ 库为准，并且【两个 ID 都接】——见 NAVI_ASSISTANT_GUIDE 的注释。
constexpr uint32_t CAR_DATA_SUBSCRIBE = 0x00010031;          // MD->HU (C++ 库叫 CAR_DATA_SUBSCRIBE)
constexpr uint32_t CAR_DATA_SUBSCRIBE_REQ = CAR_DATA_SUBSCRIBE;  // 别名（ServiceTypes.kt 叫 ..._REQ）
constexpr uint32_t CAR_DATA_SUBSCRIBE_DONE_ID = 0x00010032;  // MD->HU
constexpr uint32_t CAR_DATA_SUBSCRIBE_START = 0x00010033;    // MD->HU (CarlifeVehicleInfo)
constexpr uint32_t CAR_DATA_START_REQ = CAR_DATA_SUBSCRIBE_START;
constexpr uint32_t CAR_DATA_SUBSCRIBE_STOP = 0x00010034;     // MD->HU
constexpr uint32_t CAR_DATA_STOP_REQ = CAR_DATA_SUBSCRIBE_STOP;
constexpr uint32_t CARLIFE_DATA_REQ = 0x00010043;            // MD->HU (FeaturesHandler.kt:47)
constexpr uint32_t CARLIFE_DATA_SUBSCRIBE = 0x00018043;      // HU->MD (订阅表)
constexpr uint32_t CARLIFE_DATA_SUBSCRIBE_DONE = 0x00010044; // MD->HU
constexpr uint32_t CARLIFE_DATA_SUBSCRIBE_DONE_RSP = 0x00018044;  // HU->MD
constexpr uint32_t CARLIFE_DATA_SUBSCRIBE_START = 0x00018045;      // HU->MD
constexpr uint32_t CARLIFE_DATA_SUBSCRIBE_STOP = 0x00018046;       // HU->MD
// 【同一能力的两个 ID，都要接】官方两套代码自己就不一致：
//   ServiceTypes.kt      : MSG_CMD_NAV_ASSISTANT_GUIDE_INFO = 0x00018047（HU->MD）
//   CTranRecvPackageProcess.h : MSG_CMD_NAVI_ASSITANTGUIDE_INFO = 0x00010047（MD->HU）
// 我们作为接收方把两个都当合法输入，不动其中一个为“错”（真实实现只能这样兼容）。
constexpr uint32_t NAV_ASSISTANT_GUIDE = 0x00010047;
// 档位还有一条：C++ 库的 GEAR_INFO(0x00010029, MD->HU) 与 ServiceTypes.kt 的
// CAR_GEAR(0x00018029, HU->MD) 是【不同方向的两个消息】。我们把两者都实现。
constexpr uint32_t GEAR_INFO = 0x00010029;                   // MD->HU
constexpr uint32_t GO_TO_FOREGROUND_RESPONSE = 0x0001004C;   // MD->HU (HU 请求转前台的应答)
constexpr uint32_t CAR_VELOCITY = 0x0001800F;                // HU->MD (CarlifeCarSpeed)
constexpr uint32_t CAR_GPS = 0x00018010;                     // HU->MD (CarlifeCarGps)
constexpr uint32_t CAR_GYROSCOPE = 0x00018011;               // HU->MD (CarlifeGyroscope)
constexpr uint32_t CAR_ACCELERATION = 0x00018012;            // HU->MD (CarlifeAcceleration)
constexpr uint32_t CAR_OIL = 0x00018013;                     // HU->MD (CarlifeOil)
constexpr uint32_t CAR_GEAR = 0x00018029;                    // HU->MD (CarlifeGearInfo)

// 导航逐向 / 辅助引导（AUDIT 3.6 / 5.1 #7）。
constexpr uint32_t NAV_ASSISTANT_GUIDE_INFO = 0x00018047;    // HU->MD

// 媒体元数据列表（AUDIT 3.7）：MEDIA_INFO 这里就是 CarlifeMediaInfo。
constexpr uint32_t MEDIA_INFO_LIST = 0x00010035;             // 与 MEDIA_INFO 同 ID（proto 决定是单条还是列表）

// 电话 / HFP 全套（AUDIT 3.7 / 5.1 #13 #14）。
constexpr uint32_t BT_HFP_REQUEST = 0x00010040;              // MD->HU (CarlifeBTHfpRequest)
constexpr uint32_t BT_HFP_CONNECTION = 0x00018042;           // HU->MD (CarlifeBTHfpConnection)
constexpr uint32_t BT_HFP_RESPONSE = 0x0001804E;             // HU->MD (CarlifeBTHfpResponse)
constexpr uint32_t BT_HFP_CALL_STATUS_COVER = 0x00010058;    // MD->HU (CarlifeBTHfpCallStatusCover)
constexpr uint32_t BT_HFP_CALL_STATUS_COVER_ACK = 0x00018059;  // HU->MD（应答，保持 0x58 对称）
constexpr uint32_t START_BT_IDENTIFY_REQ = 0x00018053;       // HU->MD
constexpr uint32_t BT_IDENTIFY_RESULT_IND = 0x00010054;      // MD->HU (CarlifeBTIdentifyResultInd)

// 音频焦点（AUDIT 3.2 / 8.5）：这两条是【本机内部】消息（CommonParams 里取值 3001..3005），
// 不上线；线上仲裁靠 TTS 通道的 INIT/END + AUDIO_TTS_REQUEST_FOCUS 配置。见 session.cpp。

// 车控（AUDIT 3.6 / 5.1 #18）。
constexpr uint32_t VEHICLE_CONTROL_INFO = 0x00018061;        // HU->MD (CarlifeVehicleInfoList)
constexpr uint32_t VEHICLE_CONTROL = 0x0001006F;             // MD->HU (CarlifeVehicleControl)
constexpr uint32_t HU_VOICE_CONTROL = 0x00010066;            // MD->HU (CarlifeVoiceControlRequest)

// 文件传输（AUDIT 3.8 / 5.1 #17）：G 类，走 UPDATE 通道。
constexpr uint32_t SEND_START = 0x00070001;
constexpr uint32_t SENDING_DATA = 0x00070002;
constexpr uint32_t SEND_FINISH = 0x00070003;
constexpr uint32_t SEND_STOP = 0x00070004;
constexpr uint32_t STOP_RECEIVE = 0x00070005;

// 触摸板（AUDIT 3.5 / 5.1 #4 #5）：MD->HU，手机侧的触摸板事件。
constexpr uint32_t TOUCH_PAD_DOWN = 0x0001005A;
constexpr uint32_t TOUCH_PAD_MOVE = 0x0001005B;
constexpr uint32_t TOUCH_PAD_UP = 0x0001005C;
constexpr uint32_t TOUCH_PAD_PINCH = 0x0001005D;
constexpr uint32_t FOCUS_CHANGE = 0x0001005E;

// 视频 JPEG 截帧（AUDIT 3.4）。
constexpr uint32_t VIDEO_ENCODER_JPEG = 0x00018056;
constexpr uint32_t VIDEO_ENCODER_JPEG_ACK = 0x00010057;
constexpr uint32_t PAUSE_MEDIA = 0x0001800E;

// 蓝牙引导（SPP RFCOMM 上跑 CMD 短头）—— InstantConnectionSetup.kt
constexpr uint32_t WIRELESS_INFO_REQUEST = 0x00100001;
constexpr uint32_t WIRELESS_INFO_RESPONSE = 0x00108002;
constexpr uint32_t WIRELESS_TARGET_INFO_REQUEST = 0x00100004;
constexpr uint32_t WIRELESS_TARGET_INFO_RESPONSE = 0x00108005;
constexpr uint32_t WIRELESS_REQUEST_IP = 0x00108006;
constexpr uint32_t WIRELESS_RESPONSE_IP = 0x00100007;
constexpr uint32_t WIRELESS_MD_STATUS = 0x00100008;
constexpr uint32_t WIRELESS_HU_STATUS = 0x00108009;
constexpr uint32_t MD_RSA_PUBLIC_KEY_REQUEST = 0x0001006A;      // 内容加密：手机来要公钥
constexpr uint32_t HU_RSA_PUBLIC_KEY_RESPONSE = 0x0001806B;
constexpr uint32_t MD_AES_KEY_SEND_REQUEST = 0x0001006C;        // 手机回传 RSA 加密的 AES key
constexpr uint32_t HU_AES_REC_RESPONSE = 0x0001806D;
constexpr uint32_t MD_ENCRYPT_READY = 0x0001006E;
constexpr uint32_t MD_ENCRYPT_READY_DONE = 0x0001806F;
constexpr uint32_t STATISTIC_INFO = 0x00018027;                 // HU->MD
constexpr uint32_t MODULE_STATUS = 0x00010026;                  // MD->HU (CarlifeModuleStatusList)
// 命令型消息（HU->MD）：转前台与模块控制。载荷格式不在这里，见 host_sink.h 的
// encodeControlCommand（一条空载荷、一条 singular CarlifeModuleStatus）。
// 【取值来源，两套参考实现一致】
//   Reference/carlife-vehicle-lib/.../LibSource/include/CTranRecvPackageProcess.h:116,119
//     MSG_CMD_GO_TO_FOREGROUND = 0x00018025（cmdGoToForeground 只写包头、dataSize=0）
//     MSG_CMD_MODULE_CONTROL   = 0x00018028（cmdModuleControl 发 singular CarlifeModuleStatus）
//   Reference/apollo-DuerOS/.../carlife-sdk/.../protocol/ServiceTypes.kt:128,131（同值）
constexpr uint32_t GO_TO_FOREGROUND = 0x00018025;               // HU->MD（空载荷）
constexpr uint32_t MODULE_CONTROL = 0x00018028;                 // HU->MD (CarlifeModuleStatus)
constexpr uint32_t GO_TO_DESKTOP = 0x00010021;
constexpr uint32_t SCREEN_ON = 0x00010018;
constexpr uint32_t SCREEN_OFF = 0x00010019;
constexpr uint32_t USER_PRESENT = 0x0001001A;
constexpr uint32_t FOREGROUND = 0x0001001B;
constexpr uint32_t BACKGROUND = 0x0001001C;
constexpr uint32_t MIC_RECORD_WAKEUP_START = 0x00010022;
constexpr uint32_t MIC_RECORD_END = 0x00010023;
constexpr uint32_t MIC_RECORD_RECOG_START = 0x00010024;
// 麦克风录制【准备】握手（AUDIT 3.3）。取值逐字对照参考树：
//   carlife-sdk/src/main/java/com/baidu/carlife/sdk/internal/protocol/ServiceTypes.kt:125-126
//     MSG_CMD_MIC_RECORD_PREPARE_START = 0x00010071   // MD -> HU
//     MSG_CMD_MIC_RECORD_PREPARE_DONE  = 0x00018072   // HU -> MD
// 参考实现的用法（vehicle-app/.../module/VRModule.kt:82,147,157）：收到 START 就播一声
// 提示音（bdspeech_recognition_start.pcm），提示音【播完(onFinish)或出错(onError)】才回 DONE。
// 注意它【不校验硬件麦克风】—— 这是一次“准备动作做完了”的握手，不是硬件能力声明。
constexpr uint32_t MIC_RECORD_PREPARE_START = 0x00010071;
constexpr uint32_t MIC_RECORD_PREPARE_DONE = 0x00018072;
constexpr uint32_t TEL_STATE_INCOMING = 0x00010014;
constexpr uint32_t TEL_STATE_OUTGOING = 0x00010015;
constexpr uint32_t TEL_STATE_IDLE = 0x00010016;
constexpr uint32_t TEL_STATE_INCALLING = 0x00010017;

// VIDEO 通道 (ch::VIDEO)
constexpr uint32_t VIDEO_DATA = 0x00020001;
constexpr uint32_t VIDEO_HEARTBEAT = 0x00020002;

// AUDIO / Media 通道 (ch::AUDIO)
constexpr uint32_t MEDIA_INIT = 0x00030001;
constexpr uint32_t MEDIA_STOP = 0x00030002;
constexpr uint32_t MEDIA_PAUSE = 0x00030003;
constexpr uint32_t MEDIA_RESUME_PLAY = 0x00030004;
constexpr uint32_t MEDIA_SEEK_TO = 0x00030005;
constexpr uint32_t MEDIA_DATA = 0x00030006;
constexpr uint32_t MEDIA_DATA_ENCODER = 0x00030007;

// TTS 通道 (ch::TTS)
constexpr uint32_t NAV_TTS_INIT = 0x00040001;
constexpr uint32_t NAV_TTS_END = 0x00040002;
constexpr uint32_t NAV_TTS_DATA = 0x00040003;
constexpr uint32_t NAV_TTS_DATA_ENCODE = 0x00040004;

// VR 通道 (ch::VR)
constexpr uint32_t VR_DATA = 0x00058001;
constexpr uint32_t VR_AUDIO_INIT = 0x00050002;
constexpr uint32_t VR_AUDIO_DATA = 0x00050003;
constexpr uint32_t VR_AUDIO_STOP = 0x00050004;
constexpr uint32_t VR_MODULE_STATUS = 0x00050005;
constexpr uint32_t VR_AUDIO_INTERRUPT = 0x00050006;
constexpr uint32_t VR_AUDIO_DATA_ENCODE = 0x00050007;

// TOUCH 通道 (ch::TOUCH)，车机 -> 手机
constexpr uint32_t TOUCH_ACTION = 0x00068001;
constexpr uint32_t TOUCH_ACTION_DOWN = 0x00068002;
constexpr uint32_t TOUCH_ACTION_UP = 0x00068003;
constexpr uint32_t TOUCH_ACTION_MOVE = 0x00068004;
constexpr uint32_t TOUCH_SINGLE_CLICK = 0x00068005;
constexpr uint32_t TOUCH_DOUBLE_CLICK = 0x00068006;
constexpr uint32_t TOUCH_LONG_PRESS = 0x00068007;
constexpr uint32_t TOUCH_CAR_HARD_KEY_CODE = 0x00068008;
constexpr uint32_t TOUCH_UI_ACTION_SOUND = 0x00060009;
constexpr uint32_t TOUCH_UI_ACTION_BEGIN = 0x0006800A;
constexpr uint32_t TOUCH_ACTION_POINTER_DOWN = 0x0006800B;
constexpr uint32_t TOUCH_ACTION_POINTER_UP = 0x0006800C;
constexpr uint32_t TOUCH_ACTION_OTHER_POINTER_UP = 0x0006800D;
// 【多点触控的真实线格式】
// 依据 ServiceTypes.kt:350-354 + receiver/touch/RemoteControlManager.kt:76-104：
//   MSG_TOUCH_ACTION_3 = 0x0006800E，不使用 protobuf，固定布局：
//     action(4B BE)  [pointerId(1B) x(2B BE) y(2B BE)] × pointerCount
// 且“用哪个格式”由能力位 MULTI_TOUCH 决定（RemoteControlManager:61-69）：
//   enableMultiTouch == 1 → ACTION_3；否则 → 旧的 MSG_TOUCH_ACTION + CarlifeTouchAction。
// 下面的 action 取值就是 Android MotionEvent 的 action（fillByteArray 写的是 event.action）。
constexpr uint32_t TOUCH_ACTION_3 = 0x0006800E;
namespace motion {
constexpr int32_t ACTION_DOWN = 0;
constexpr int32_t ACTION_UP = 1;
constexpr int32_t ACTION_MOVE = 2;
constexpr int32_t ACTION_CANCEL = 3;
// POINTER_DOWN/UP 的高字节是手指【索引】（Android 的约定：action | (index << 8)）。
constexpr int32_t ACTION_POINTER_DOWN = 5;
constexpr int32_t ACTION_POINTER_UP = 6;
constexpr int32_t kPointerIndexShift = 8;
}  // namespace motion

// UPDATE 通道 (ch::UPDATE)
constexpr uint32_t DATA_MD_TRANSFER_START = 0x00070007;
constexpr uint32_t DATA_MD_TRANSFER_SEND = 0x00070008;
constexpr uint32_t DATA_MD_TRANSFER_END = 0x00070009;
constexpr uint32_t DATA_HU_UPDATE_START = 0x0007800A;
constexpr uint32_t DATA_HU_UPDATE_END = 0x0007800B;

// ── 硬键 KEYCODE_*（ServiceTypes.kt:358-408，共 30+ 项）──
// 【重要】这不是 Android 的 KEYCODE_*，而是 CarLife 自己的小车机键码表。
// 我们的 TOUCH_CAR_HARD_KEY_CODE 下发必须用它，不能混用 ADB keycode。
namespace keycode {
constexpr int32_t HOME = 0x01;
constexpr int32_t PHONE_CALL = 0x02;
constexpr int32_t PHONE_END = 0x03;
constexpr int32_t PHONE_END_MUTE = 0x04;
constexpr int32_t HFP = 0x05;
constexpr int32_t SELECTOR_NEXT = 0x06;
constexpr int32_t SELECTOR_PREVIOUS = 0x07;
constexpr int32_t SETTING = 0x08;
constexpr int32_t MEDIA = 0x09;
constexpr int32_t RADIO = 0x0A;
constexpr int32_t NAV = 0x0B;
constexpr int32_t SRC = 0x0C;
constexpr int32_t MODE = 0x0D;
constexpr int32_t BACK = 0x0E;
constexpr int32_t SEEK_SUB = 0x0F;
constexpr int32_t SEEK_ADD = 0x10;
constexpr int32_t VOLUME_SUB = 0x11;
constexpr int32_t VOLUME_ADD = 0x12;
constexpr int32_t MUTE = 0x13;
constexpr int32_t OK = 0x14;
constexpr int32_t MOVE_LEFT = 0x15;
constexpr int32_t MOVE_RIGHT = 0x16;
constexpr int32_t MOVE_UP = 0x17;
constexpr int32_t MOVE_DOWN = 0x18;
constexpr int32_t MOVE_UP_LEFT = 0x19;
constexpr int32_t MOVE_UP_RIGHT = 0x1A;
constexpr int32_t MOVE_DOWN_LEFT = 0x1B;
constexpr int32_t MOVE_DOWN_RIGHT = 0x1C;
constexpr int32_t TEL = 0x1D;
constexpr int32_t MAIN = 0x1E;
constexpr int32_t MEDIA_START = 0x1F;
constexpr int32_t MEDIA_STOP = 0x20;
constexpr int32_t VR_START = 0x21;
constexpr int32_t VR_STOP = 0x22;
constexpr int32_t NUMBER_0 = 0x23;
// 数字 1..9 连续递增（0x24..0x2C）
constexpr int32_t NUMBER_STAR = 0x2D;
constexpr int32_t NUMBER_POUND = 0x2E;
constexpr int32_t NUMBER_DEL = 0x2F;
constexpr int32_t NUMBER_CLEAR = 0x30;
constexpr int32_t NUMBER_ADD = 0x31;
}  // namespace keycode

// ── HFP（BtHfpManager.java:42-69，逐条照抄）──
namespace hfp {
// CarlifeBTHfpRequest.command
constexpr int32_t REQ_START_CALL = 1;      // 拨号（带 phoneNum）
constexpr int32_t REQ_TERMINATE_CALL = 2;  // 挂断
constexpr int32_t REQ_ANSWER_CALL = 3;     // 接听
constexpr int32_t REQ_REJECT_CALL = 4;     // 拒接
constexpr int32_t REQ_DTMF_CODE = 5;       // 发送 DTMF（带 dtmfCode）
constexpr int32_t REQ_MUTE_MIC = 6;
constexpr int32_t REQ_UNMUTE_MIC = 7;
// CarlifeBTHfpResponse.status
constexpr int32_t STATUS_INVALID_PARAM = -1;
constexpr int32_t STATUS_FAILURE = 0;
constexpr int32_t STATUS_SUCCESS = 1;
// CarlifeBTHfpStatusRequest.type
constexpr int32_t TYPE_MIC_STATUS = 1;
constexpr int32_t MIC_MUTE = 1;
constexpr int32_t MIC_UNMUTE = 0;
// CarlifeBTHfpConnection.state（与 Android BluetoothProfile 一致）
constexpr int32_t CONN_DISCONNECTED = 0;
constexpr int32_t CONN_CONNECTING = 1;
constexpr int32_t CONN_CONNECTED = 2;
constexpr int32_t CONN_DISCONNECTING = 3;
// CarlifeBTHfpIndication.state（通话状态）
constexpr int32_t CALL_NEW_CALL = 1;       // 来电响铃
constexpr int32_t CALL_OUT_CALL = 2;       // 去电
constexpr int32_t CALL_ACTIVE = 3;         // 通话中
constexpr int32_t CALL_NO_CALL_ACTIVE = 4; // 空闲
}  // namespace hfp

// ── 档位（CarlifeGearInfo.gear；参考实现按 Android 档位语义使用）──
namespace gear {
constexpr int32_t UNKNOWN = 0;
constexpr int32_t PARK = 1;
constexpr int32_t REVERSE = 2;
constexpr int32_t NEUTRAL = 3;
constexpr int32_t DRIVE = 4;
}  // namespace gear

// ── 麦克风来源（Constants.kt:95-107 / CarlifeConfUtil VOICE_MIC）──
namespace mic {
constexpr int32_t STATUS_USE_VEHICLE_MIC = 0;
constexpr int32_t STATUS_USE_MOBILE_MIC = 1;
constexpr int32_t STATUS_NOT_SUPPORTED = 2;
constexpr int32_t STATUS_VEHICLE_AES_MIC_LEFT = 3;
constexpr int32_t STATUS_VEHICLE_AES_MIC_RIGHT = 4;
}  // namespace mic

// ── 通话记录类型（CarlifeCallRecordsProto.CallRecordsType）──
namespace call_type {
constexpr int32_t DEFAULT = 0;
constexpr int32_t INCOMING = 1;
constexpr int32_t OUTGOING = 2;
constexpr int32_t MISSED = 3;
}  // namespace call_type

// ── 数据通道升级/文件传输状态（用于 LinkState.ota_state）──
namespace transfer {
constexpr uint8_t OTA_NONE = 0;
constexpr uint8_t OTA_DOWNLOADING = 1;
constexpr uint8_t OTA_VERIFYING = 2;
constexpr uint8_t OTA_READY = 3;
constexpr uint8_t OTA_FAILED = 4;
}  // namespace transfer

// 模块 ID (Constants.kt)
constexpr int32_t MODULE_PHONE = 1;
constexpr int32_t MODULE_NAVI = 2;
constexpr int32_t MODULE_MUSIC = 3;
constexpr int32_t MODULE_VR = 4;
constexpr int32_t MODULE_CONNECT = 5;
constexpr int32_t MODULE_MIC = 6;
constexpr int32_t MODULE_CRUISE = 8;
constexpr int32_t MODULE_CRUISE_FOLLOW = 9;

// 版本匹配
constexpr int32_t PROTOCOL_VERSION_MATCH = 1;
constexpr int32_t PROTOCOL_VERSION_NOT_MATCH = 2;
}  // namespace msg

// keycode / hfp / gear / mic / call_type / transfer 这几个常量组被写在了 msg 命名空间
// 的文本范围里（紧跟在消息 ID 之后），但它们描述的是【取值域】而不是消息 ID。
// 为了让调用方写 `hfp::REQ_START_CALL` 而不是 `msg::hfp::...`，这里给简短的顶层别名。
namespace keycode = msg::keycode;
namespace hfp = msg::hfp;
namespace gear = msg::gear;
namespace mic = msg::mic;
namespace call_type = msg::call_type;
namespace transfer = msg::transfer;

// FEATURE_CONFIG_* 能力协商键（ServiceTypes.kt / FeaturesHandler.kt）
namespace feature {
constexpr const char* kContentEncryption = "CONTENT_ENCRYPTION";
constexpr const char* kConnectType = "CONNECT_TYPE";
constexpr const char* kUsbMtu = "USB_MTU";
constexpr const char* kMultiTouch = "MULTI_TOUCH";
constexpr const char* kIFrameInterval = "I_FRAME_INTERVAL";
constexpr const char* kAacSupport = "AAC_SUPPORT";
constexpr const char* kAudioTransmissionMode = "AUDIO_TRANSMISSION_MODE";
constexpr const char* kMediaSampleRate = "MEDIA_SAMPLE_RATE";
constexpr const char* kVoiceMic = "VOICE_MIC";
constexpr const char* kVoiceWakeup = "VOICE_WAKEUP";
constexpr const char* kBluetoothAutoPair = "BLUETOOTH_AUTO_PAIR";
constexpr const char* kBluetoothInternalUi = "BLUETOOTH_INTERNAL_UI";
constexpr const char* kFocusAreaAutoSet = "FOCUS_AREA_AUTO_SET";
constexpr const char* kFocusUi = "FOCUS_UI";
constexpr const char* kInputDisable = "INPUT_DISABLE";
constexpr const char* kMusicHud = "MUSIC_HUD";
constexpr const char* kEngineType = "ENGINE_TYPE";
// 本轮新增：bdcf 里存在、但原实现没有上报的键（AUDIT 3.9 全量清单）。
// 键名逐条取自 CarlifeConfUtil.java:166-188。
constexpr const char* kAudioTrackNum = "AUDIO_TRACK_NUM";
constexpr const char* kAudioTrackStreamType = "AUDIO_TRACK_STREAM_TYPE";
constexpr const char* kAudioTrackType = "AUDIO_TRACK_TYPE";
constexpr const char* kTtsRequestAudioFocus = "AUDIO_TTS_REQUEST_FOCUS";
constexpr const char* kNeedMoreDecodeTime = "NEED_MORE_DECODE_TIME";
constexpr const char* kTransparentSendTouchEvent = "TRANSPARENT_SEND_TOUCH_EVENT";
constexpr const char* kSendActionDown = "SEND_ACTION_DOWN";
constexpr const char* kVehicleGps = "VEHICLE_GPS";
constexpr const char* kGpsFormat = "GPS_FORMAT";
constexpr const char* kConnectTypeAndroid = "CONNECT_TYPE_ANDROID";
constexpr const char* kConnectTypeIphone = "CONNECT_TYPE_IPHONE";
constexpr const char* kIphoneUsbConnectType = "IPHONE_USB_CONNECT_TYPE";
constexpr const char* kIphoneNcmEthernetName = "IPHONE_NCM_ETHERNET_NAME";
}  // namespace feature

}  // namespace carlife