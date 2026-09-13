# Wired CarPlay Output 接口清单

## 范围与完成度

接口层最初按“先完成 Output 功能接口，暂不对接转换层/Core”实现；现 `Core/Forward` 已作为独立调用方接入。Output 扮演 **Device（手机端）**，Accessory 是原车主机。下列“已实现”指可编译、可测试的接口层行为，不表示对应 USB/网络协议已经运行。

覆盖基准为本地 `Reference/CatPlaySource` 已给出的功能，不能从这个开源参考推导出完整的 Apple 商业协议覆盖率。Livi/CarLife 的输入转换不在本模块实现。参考代码不参与本库构建。

状态分三类：

- **已实现接口逻辑**：参数/类型/方向检查、能力门控、异步队列、状态变更、失败/超时/断开处理、数据所有权。
- **已定义数据/提供者契约**：USB、鉴权、配对、屏幕/音频配置、元数据。真实动作由后端完成。
- **扩展通道**：参考仅注释、空声明或缺少编码的功能，只能通过明确的 schema 和能力声明交给未来后端，默认不可用。

## 公共方法：完整列表

所有方法属于 `zero2w::wired::WiredCarPlayOutput`，非阻塞且线程安全。类型是 C++20 源码级 API，**不是可直接跨进程或跨编译器复制的 ABI**。

| 方法 | 作用及实际行为 |
|---|---|
| 构造 / 析构 | 校验队列上限；拥有内存；不启动线程；析构不替后端关闭硬件 |
| `start(StartSession, timeout)` | 校验显式 UDC/USB 标识，分配新 epoch，入队启动请求 |
| `shutdown(timeout)` | 入队关闭请求，取消其他请求并清理缓存；预留一个关闭槽位 |
| `submit(Operation, timeout)` | 提交下表中任一操作，返回本地受理 Ticket，不是执行成功 |
| `cancel(epoch, id)` | 取消待处理请求；已发出的状态变更取消后进入 Failed，要求后端关闭传输 |
| `pop_completion()` | 提取执行结果及原始响应；结果未消费也占容量，避免无限增长 |
| `pop_completion(epoch, id)` | 只领取指定请求的结果；Core Forward 不会抢走启动者的回执 |
| `discard_media(epoch)` | Core 切源/重配时显式清除排队播放与麦克风；旧 epoch 请求被拒绝 |
| `snapshot()` | epoch、阶段、错误、流/队列数量、字节数、拒绝/后端取包/麦克风收包计数 |
| `info_cached()` | 已成功启动的车机信息副本；断开即失效 |
| `media_clock()` | 后端提供的 NTP/样本计数/采样率/测量时间快照 |
| `push_media(packet)` | 向车机方向入队视频/音频；要求 RECORD 成功、流有效、格式对齐和容量允许 |
| `pop_event()` | 提取车机控制、CSM 或服务事件；不会自动把车机命令应答为成功 |
| `pop_microphone()` | 提取车机麦克风回传，保留 epoch、流 ID 和样本时间戳 |
| `respond(epoch, id, status, response)` | 应答车机控制；重复、迟到和旧会话应答被拒绝 |
| `take_work()` | 后端按提交顺序领取请求；同一请求只领取一次 |
| `complete(epoch, id, status, response, peer)` | 后端提交执行结果；成功启动必须带合法 PeerInfo；仅此处落实流/RECORD/modes 成功状态 |
| `report_progress(epoch, phase)` | 后端报告启动细分阶段；允许重试回退，不能借此直接伪造 Ready/Streaming |
| `disconnect(epoch, reason)` | 使当前会话失败，结束挂起请求并清除所有旧媒体、麦克风、事件和能力 |
| `update_clock(epoch, clock)` | 接受有效且测量时间不倒退的时钟快照，不自行做 NTP 同步 |
| `take_media()` | 后端领取待发媒体；计数只表示领取，不证明车机已经播放 |
| `receive_control(epoch, command, timeout)` | 后端送入车机命令，生成关联 ID、事件与待应答义务 |
| `receive_iap2(epoch, message)` | 后端送入已协商的 CSM 消息，原始参数完整保留 |
| `receive_service(epoch, message)` | 后端送入已协商服务数据，检查结构类型和范围 |
| `receive_microphone(packet)` | 后端送入车机麦克风；不能错误写到视频或未启用麦克风的音频流 |
| `take_reply(now)` | 后端领取控制应答；未处理且过期的命令生成 Timeout，绝不默认成功 |
| `expire(now)` | 定时清理过期请求；已发出状态操作的结果不确定时，使会话失败 |

另有 `describe(CommandType)`、`describe(CsmType)` 完整目录查询，以及 `wire_audio_format(AudioFormat)` / `audio_format_from_wire(bit)` 的 31 种参考音频格式双向映射。未知、多位或零格式位被拒绝。

供上层有界接入使用的 `validate_control_command` / `validate_service_message` 只进行结构和大小检查，不执行动作。MediaPacket 新增显式 `Timebase`：默认 StreamClock，CPMF 直通使用 MonotonicMicroseconds；后者必须由后端声明 `monotonic_media_timestamps` 支持，否则拒绝，不能把微秒误当 NTP 或样本计数。

## Operation：全部可提交操作

| 操作类型 | 功能接口 |
|---|---|
| `StartSession` | UDC、USB VID/PID/制造商/产品/序列号、Peripheral/Host/Negotiated 角色、配对存储标识 |
| `StopSession` | 停止会话；后端负责 RTSP/流/iAP2/USB 清理后才回执成功 |
| `ScreenConfiguration` | 流 ID、显示 UUID、主屏/副屏/仪表、H.264/HEVC、Annex-B/AVCC/HVCC、尺寸、帧率、延迟、codec 配置 |
| `AudioConfiguration` | General/Main/Alternate/MainHigh，以及显式扩展 Buffered/Auxiliary；媒体/提示/电话/语音识别/兼容用途；播放和麦克风方向独立 |
| `TeardownStream` | 按流关闭；成功后清除该流剩余媒体/麦克风，最后一条流关闭后回到 Ready |
| `Record` | 单独提交 RECORD；未建流或重复 RECORD 被拒绝；不能把 SETUP 当成播放已开始 |
| `DrainTeardown` | 请求后端完成清理队列，并提供执行回执 |
| `AssertModes` | 屏幕/主音频的当前与永久拥有者，电话/导航/语音拥有者及语音模式 |
| `SendControl` | 下列 13 个命令的类型化载荷，强制检查手机/车机方向 |
| `SendIap2` | CSM 类型、session ID、原始参数 body；TLV 未知字段不丢弃 |
| `SendService` | 下列全部 31 个功能服务；结构化载荷或显式 schema 的原始数据 |
| `PairingRequest` | PairSetup / PairVerify / AuthSetup / ForgetPeer；须由后端声明能力 |

音频格式包括：PCM 8/16/24/32/44.1/48 kHz 单/双声道（44.1/48 kHz 支持 24-bit），ALAC 44.1/48 kHz 16/24-bit 双声道，AAC-LC 44.1/48 kHz 双声道，AAC-ELD 16/24 kHz 单声道与 44.1/48 kHz 单/双声道，Opus 16/24/48 kHz 单声道。声明格式不等于已提供编码器，媒体须已符合协商格式。

`PeerInfo` 包括设备标识、名称/厂商/型号/版本、USB 角色、网络接口/地址、feature/status 位、controller/extended features、蓝牙 ID、显示/EDID/物理尺寸/输入特性、HID 描述符、夜间/限制 UI/左右舵、资源状态、完整原始 `/info` plist 和协商能力。原始 plist 保留音频延迟、OEM 图标等未展开字段。

参考的 ControllerFeature 全部可经特性列表保留：UiContext、ViewAreas、CornerMasks、FocusTransfer、H264Level51、MainBuffered、AltScreen、EnhancedSiri、Hevc、SessionManagement、LogTransfer、IApChannel、VehicleStateProtocol、VideoPlayback；不因参考存在该特性就默认启用。限制 UI 的 SoftKeyboard、SoftPhoneKeypad、NonMusicLists、MusicLists、JapanMaps，以及 EnhancedRequestCarUI、VocoderInfo、HighAccuracyTimeStamps 扩展信息也由原始 info/特性列表保留，具体解释由后端完成。

## 控制命令：13/13

| 线协议名称 | 方向 | 载荷 |
|---|---|---|
| `duckAudio` | Output → 车机 | 渐变毫秒、衰减 dB |
| `unduckAudio` | Output → 车机 | 渐变毫秒、恢复 dB |
| `disableBluetooth` | Output → 车机 | 蓝牙 MAC |
| `modesChanged` | Output → 车机 | ModeState |
| `hidSetInputMode` | Output → 车机 | HID UUID、Default/Character/Scrolling/ScrollingWithCharacters/DialPad |
| `changeModes` | 车机 → Output | 完整 BinaryPlist，保留资源优先级、take/borrow/unborrow 等字段 |
| `forceKeyFrame` | 车机 → Output | 空载荷；由未来上游决定如何生成关键帧 |
| `hidSendReport` | 车机 → Output | HID UUID、原始 report、可选 NTP 时间戳；触摸/旋钮/按键不被误当成发送到车机 |
| `requestSiri` | 车机 → Output | Prewarm / ButtonDown / ButtonUp |
| `requestUI` | 车机 → Output | 可选 URL |
| `setNightMode` | 车机 → Output | bool |
| `setLimitedUI` | 车机 → Output | bool |
| `iAPSendMessage` | 双向 | 原始 iAP 数据 |

接收控制后由调用者处理并 `respond`。拒绝入队时后端必须立即将错误回应车机；已入队但无人处理时由 `take_reply` 产生超时应答。

## iAP2 CSM：59/59

完整强类型枚举、精确数值和逐项查询见 `catalog.hpp`。这些是**消息标识与参数通路**，没有在 C++ 中复制参考的 TLV 编解码器；方向、session 路由和业务状态必须由真实 iAP2 后端验证。鉴权/识别启动消息在 Ready 之前由后端和提供者处理，不能用正常运行阶段的 `SendIap2` 绕过启动流程。

| 家族 | 数量 | 全部消息 |
|---|---:|---|
| auth | 7 | RequestAuthenticationCertificate、AuthenticationCertificate、RequestAuthenticationChallengeResponse、AuthenticationResponse、AuthenticationFailed、AuthenticationSucceeded、AccessoryAuthenticationSerialNumber |
| ident | 6 | StartIdentification、IdentificationInformation、IdentificationAccepted、IdentificationRejected、CancelIdentification、IdentificationInformationUpdate |
| carplay_modern | 4 | CarPlayAvailability、CarPlayStartSession、AvailableDigitalCarKeys、MatchedDigitalCarKeys |
| now_playing | 4 | StartNowPlayingUpdates、NowPlayingUpdate、StopNowPlayingUpdates、SetNowPlayingInformation |
| comms | 14 | StartCallStateUpdates、CallStateUpdate、StopCallStateUpdates、StartCommunicationsUpdates、CommunicationsUpdate、StopCommunicationsUpdates、InitiateCall、AcceptCall、EndCall、SwapCalls、MergeCalls、HoldStatusUpdate、MuteStatusUpdate、SendDTMF |
| comms_lists | 3 | StartListUpdates、ListUpdate、StopListUpdates |
| device_notifications | 6 | DeviceInformationUpdate、DeviceLanguageUpdate、DeviceTimeUpdate、DeviceUUIDUpdate、WirelessCarPlayUpdate、DeviceTransportIdentifierNotification |
| eap | 3 | StartExternalAccessoryProtocolSession、StopExternalAccessoryProtocolSession、StatusExternalAccessoryProtocolSession |
| gps | 4 | StartLocationInformation、GPRMCDataStatusValuesNotification、LocationInformation、StopLocationInformation |
| power | 4 | StartPowerUpdates、PowerUpdate、StopPowerUpdates、PowerSourceUpdate |
| wifi | 4 | RequestWiFiInformation、WiFiInformation、RequestAccessoryWiFiConfigurationInformation、AccessoryWiFiConfigurationInformation |

Wi-Fi/车钥匙在目录中保留只是为了完整对照，并不启动无线输出或实现数字钥匙。

## 服务数据：全部 31 个接口分类

`ServiceMessage { service, schema, payload }` 两个方向均可传递；每个功能必须出现在后端提交的 `negotiated.services` 中。后端必须按 schema 判断是否实现编码及方向，不能因为枚举存在就启用功能。

| Service | 结构化载荷 / 当前范围 |
|---|---|
| NowPlaying | NowPlaying：媒体条目、播放状态/速率、进度、队列位置、随机/循环 |
| Artwork | Artwork：ID、MIME、尺寸和图像数据 |
| Lyrics | Lyrics：曲目、语言、带起止时间的歌词行 |
| Navigation | Navigation：路线、道路、转向说明、目的地、距离与剩余时间 |
| Contacts / Favorites | ContactDirectory：版本、全量/增量、姓名、电话/邮箱、删除 ID |
| Recents | RecentCalls：联系人、号码、时间、时长、呼入/未接 |
| MediaLibrary / PlaybackQueue | MediaLibrary：版本、条目、删除 ID |
| CallState | CallState：多路呼叫、号码、姓名、状态、方向、静音 |
| Communications | Communications：运营商、服务、信号、注册、漫游 |
| VehicleInformation | VehicleInformation：厂商/车型/VIN/动力类型/年份/左右舵 |
| VehicleStatus | VehicleStatus：驻车、倒挡、可选车速/温度；不是本地 CAN 实现 |
| Location | Location：经纬度、高度、速度、方向、精度、时间、有效性 |
| Power | Power：电量、充电、外部电源、电流能力 |
| DeviceNotifications | DeviceNotification：设备、名称、语言、传输 ID、时间 |
| ExternalAccessory | ExternalAccessoryData：协议名、session ID、数据 |
| FileTransfer / Diagnostics | FileChunk：传输 ID、MIME、偏移、总大小、结束标志、分块数据；不访问文件系统 |
| DisplayPanels / ViewArea | DisplayPanels：显示 UUID 和布局矩形 |
| Vocoder | Vocoder：codec、采样率、码率 |
| UiContext / Appearance / MapAppearance / MapZoom | 显式 schema 的原始扩展载荷；暂无线协议实现 |
| Haptics / HidSetReport / FlushAudio | 显式 schema 的原始扩展载荷；暂无线协议实现 |
| DigitalCarKeys / WifiConfiguration | 显式 schema 的原始扩展载荷；保留参考扩展范围，不实现钥匙/无线配置业务 |

结构化接口不是承诺车机原生支持歌词、联系人或任意导航数据。跨协议转换与具体承载方式留待下一阶段。列表分块每批最多 256 项（通话 16 项、显示 16 项），需要上层分页；不悄悄截断。文件块检查偏移/总大小/末块一致性，但完整传输校验、重传、落盘仍归后端。

11 个参考注释扩展完整保留：`updateVehicleInformation`、`flushAudio`、`performHapticFeedback`、`hidSetReport`、`updateDisplayPanels`、`updateVocoderInfo`、`updateViewArea`、`changeUIContext`、`uiAppearanceUpdate`、`mapAppearanceUpdate`、`changeMapZoomLevel`。没有把它们冒充成已有的第 14～24 个线命令。

## USB / 鉴权 / 配对边界

启动阶段可报告：Starting、GadgetEnabled、RoleSwitching、AccessoryDetected、Iap2Negotiating、Authenticating、Identifying、NcmConfiguring、Discovering、Pairing、RtspConnecting；完成后 Ready、RECORD 后 Streaming；最后 Stopping/Closed 或 Failed。

- `AuthenticationProvider`：`certificate`、`sign_challenge`、`verify_certificate`、`verify_response`；纯虚接口，无固定成功实现。真实实现负责证书链/挑战验证及设备 I/O 超时，不能仅因函数返回就绕过验证。
- `PairingStore`：`load`、`save`、`erase`；纯虚接口，记录 opaque，不把私钥放到状态快照。加密存储和原子写由后端承担。
- USB 角色选择、gadget/configfs、热插拔、iAP2 link/识别/鉴权、NCM/IPv6、服务发现、RTSP 配对/SETUP/RECORD/TEARDOWN、RTP 包化/加密/同步/保活/重传，均由后续协议引擎实现。这里只接收明确的成功/失败回执，不触碰当前 USB SSH。

## 后端接入契约

1. 单一后端循环领取 `take_work`，执行对应动作后 `complete`。启动成功的 `PeerInfo.negotiated` 必须是“后端真实实现能力 ∩ 车机协商能力”，不能直接复制测试夹具。
2. 同时消费 `take_media` 和 `take_reply`，提交真实的时钟、事件与麦克风数据。媒体出队只交接所有权，发送失败需 `disconnect`，不能计作成功播放。
3. 定时调用 `expire`，并使用每个 WorkItem 自带的单调时钟 deadline 限制 I/O。取消/超时不能物理中止任意外部阻塞函数；后端必须实现取消和资源回收。
4. 断开后停止旧任务，废弃已领取的旧媒体和旧回执；新 epoch 不得复用旧连接。所有跨线程回调带 epoch；过期回执被拒绝。
5. 状态变更操作串行；只有 ACK 成功才更新流状态。已经发出的状态变更超时/取消后进入 Failed，后端必须关闭传输再重新启动，不能假定远端未执行。
6. 默认请求 32 个（另预留 1 个 shutdown）、反向待应答 32 个、事件 64 个；媒体 64 包/8 MiB，麦克风 64 包/256 KiB，单个原始载荷 1 MiB，流 8 条。完成结果仍占请求容量。各队列有界；结构化数据另有条目数和字符串长度限制。

## 与参考 transmitter API 的逐项对应

| 参考 API | 本模块 |
|---|---|
| bootstrap / setup gates | StartSession、启动 Phase、AuthenticationProvider、PairingStore、PairingRequest |
| closed | snapshot.phase / last_error（非阻塞快照） |
| shutdown | shutdown + Completion |
| info_cached | info_cached |
| media_clock | media_clock / update_clock |
| pop_command | pop_event + respond + take_reply |
| send_command_noresp | submit(SendControl) + Completion，保留实际应答而非假成功 |
| setup_screen | submit(ScreenConfiguration) + push_media |
| setup_audio | submit(AudioConfiguration) + push_media / receive_microphone / pop_microphone |
| record | submit(Record) |
| assert_modes | submit(AssertModes) |
| drain_teardown_queue / TeardownGuard | submit(TeardownStream / DrainTeardown)，显式异步清理而非析构时隐式操作硬件 |
