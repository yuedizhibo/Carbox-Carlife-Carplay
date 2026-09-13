# 两个 Input：功能与 API 对照

更新日期：2026-09-13。基准是本工作区 Reference 的本地快照，不代表上游最新版。

## 本轮范围与结论

用户已明确：**现在只对接两个 Input 的接口，核对消息、方向和载荷，不做有线输出或真车验收。** 麦克风与车辆数据在产品架构中来自原车 CarPlay 回传，不要求另配本地麦克风/CAN。

已补接下述接口并做代码层回归。**仍不能宣称参考仓库的所有功能/API 全部接入**：本地预留扩展、宿主应用 API、真实生产者和完整业务事务必须区分，不把“有结构体/能解析测试包”当成实现完成。

- [两侧接口、全部本地扩展及限制](INTERFACE_CONTRACT.md)：公共 ABI、9 类控制、25 类下行/11 类上行逐项列出。
- [完整来源索引](inventory.md) / [机器可读索引](inventory.json)：2687 条不同类别记录，不是2687个已完成功能。
- [LIVI 全部70个 preload 成员](LIVI_API_STATUS.md)：保留 Electron、dongle、AA 和应用更新等非无线 Input 接口，不静默遗漏。
- [手机校验实现与依据](AUTH.md)。
- [测试记录](TEST_RESULTS.md) / [提取器遗漏项](gaps.md)。后者是解析器缺口，不是产品缺口。

## 目录

| 目录 | 当前职责 |
| --- | --- |
| Input/WirelessCarLifePlus | BlueZ 无线引导、7通道TCP、会话/protobuf、HostSink → SessionCore/RealMediaStore |
| Input/WirelessCarPlay | C++ CPMF 媒体/控制客户端 |
| Input/WirelessCarPlay/Engine | 独立 Rust 侧车进程；外部 CatPlay 本地评估补丁，不等于移植 LIVI 全栈 |
| Core/MainMenu、Core/Convert | 选择/反向控制、统一状态、有界媒体存储 |
| Core/Web | 状态、配置、媒体与9类控制解析 |
| Core/Forward、Output/WiredCarPlay | 仍为后续输出范围；本轮没有实施或验收 |
| Reference | 只读对照源；本轮没有往参考库写修复 |

## 本轮已补接/修复

| 部分 | 可追踪的实现 |
| --- | --- |
| CarLife 手机校验 | SdkPhoneVerifier：按参考库接收端摘要路径校验，默认sdk，保留注入seam；错误/异常/未知模式拒绝 |
| CarLife 加密 | 已有RSA/AES协商；修正Off/协商中/Ready状态，能力与配置/密钥匹配 |
| CarLife 麦克风 | 可注入上行PCM源，校验长度/异常；MIC_RECORD_* 与VR下行方向分离；PREPARE提示音完成/失败均结束准备阶段 |
| CarLife 控制 | 旋钮多步整批排队、VR松开、完整DTMF数字/*/#及拨号末尾PHONE_CALL；多点触控按能力选线格式 |
| CarLife 专用命令 | 新增 requestForeground / requestModuleControl → 空CMD / singular protobuf，经会话线程与加密出口发送 |
| CarLife 应用事件 | 8类前后台/屏幕/桌面/返回前台通知经 onAppEvent → 可注入宿主处理方，不再仅日志 |
| CarLife 状态 | MODULE_STATUS 回调不再仅记录；激活默认未知，不用固定成功冒充token发放 |
| CarPlay 生命周期 | 扩展绑定当前会话；断线/换会话清缓存与半片；拒绝旧会话，音频描述符映射和重复流计数修正 |
| CarPlay 下行 | 真实iAP2 NowPlaying/封面/CallState → CPMF；主屏offer和HID能力生产者 |
| CarPlay 上行 | 媒体/电话/旋钮HID、双点触控、Home/Back/方向/滚轮/选择、接近、Siri、电话控制、night_mode/limited_ui |
| CarPlay 传输 | 扩展控制带非零session和sequence；Rust按声明长度处理分包/粘包；过期/重放扩展不执行 |
| Core/Web | 9类控制严格表单解析；重复、未知、溢出、NUL及非法编码拒绝；新增HTTP路由回归 |
| Core 状态比较 | 成员相等比较替换结构体memcmp，消除padding误判 |

封面/歌词组包还覆盖缺片、版本混合、总长度及256分片边界。代码路径通过测试不等于真实手机支持所有操作。

## 尚未对齐的部分

详细到每个类型见 INTERFACE_CONTRACT.md。主要是 CarPlay 歌词、导航、媒体库、联系人/通话记录的真实生产者与完整数据接口；部分无障碍/HID模式预留；多设备轮转；完整多源状态隔离/重放；逐控制执行ACK。CarLife 部分文件/OTA/激活/HFP/车控接口有解析或宿主回调，但不是完整执行事务。

CarLife Android ServiceTypes.kt 的309条常量全部提取；筛选出的152个消息名中150个数值在本地存在，剩余 CHANNEL_SPECIAL_SETTINGS 0x01018003 与 HU_LOG 0x7FFF0001 未接。这两条在参考扫描中只有专用渠道设置/日志声明，未据此猜测业务载荷。数值匹配不是功能覆盖率。

C++参考：414条API声明、56个回调字段；Android SDK：406条方法、319条属性、70条类型（含非公开成员）；LIVI原生CP：144个方法候选（正则不是完整AST）；preload：70个成员。原始名称/行号都在索引中，重复声明/包装层不合并冒充功能数。

## 可复跑

```sh
python Input/API_AUDIT/generate_inventory.py
cmake --build Temp/input-audit-build -j4
ctest --test-dir Temp/input-audit-build --output-on-failure --timeout 20
```

外部 CatPlay 只对独立副本依次应用 cp-native、input-only、input-api 三层补丁。当前三层已验证无fuzz应用并与评审源文件逐字节一致；许可证边界仍按 Engine/catplay-patch/README.md，不部署/发布评估产物。

PiAgent承担了部分有界实现；超时任务已终止，遗留修改由Codex复查并修正。测试结论来自独立命令，不采信工作器未验证的完成声明。
