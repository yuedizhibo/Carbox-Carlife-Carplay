// HFP HF（车机）侧的服务级连接（SLC）建立。
//
// 为什么必须有这一步：
//   HFP 不是「RFCOMM 连上就完事」。连上之后，**HF（车机）必须主动发 AT 指令**，
//   AG（手机）逐条应答，直到 SLC 建立完成。我们此前一个字节都不发，
//   手机发完 RFCOMM 连接后就一直在等我们的 AT+BRSF —— 表现就是
//   「蓝牙一直显示正在连接」。
//
// 依据 HFP 1.7 规范（HF 侧发起的标准序列）：
//   HF -> AG : AT+BRSF=<hf features>      AG -> HF : +BRSF: <ag features> / OK
//   HF -> AG : AT+CIND=?                  AG -> HF : +CIND: (...) / OK
//   HF -> AG : AT+CIND?                   AG -> HF : +CIND: ... / OK
//   HF -> AG : AT+CMER=3,0,0,1            AG -> HF : OK
//   HF -> AG : AT+CHLD=?                  AG -> HF : +CHLD: (...) / OK
//   （启用编解码协商时）HF -> AG : AT+BAC=1,2      AG -> HF : OK
//
// 该函数会把收发内容累积到 log（有界），便于在真机上看出手机侧随后发来的
// CarLife OOB 报文长什么样 —— 那正是下一步要实现的（参考 CarlifeBTPairInfo）。
#pragma once

#include <cstdint>
#include <string>

namespace carlife {

// 在已建立的 HFP RFCOMM 链路 fd 上以 HF 身份跑完 SLC。
//   hf_features : HF 特性位掩码（与 SDP 里发布的一致，比如 0x007f）
//   timeout_ms  : 单条 AT 应答等待上限
// 成功返回 true。失败时 *err 说明卡在哪一条指令，*log 里是实际收发内容。
bool runHfServiceLevelConnection(int fd, uint16_t hf_features, int timeout_ms, std::string* err,
                                 std::string* log);

// SLC 建立后，把链路上的一切【原样】记录下来（十六进制 + 可打印 ASCII），
// 不做任何分帧假设。
//
// 为什么需要它：实测手机连的是 HFP 而不是 SPP；SLC 建立后我方一个字节都不发、
// 也不再读，于是“手机随后到底发了什么”完全未知，而 bringup() 用自己那套
// CarLife 二进制分帧去读，不合规的数据只会超时、不会留下原始字节。
// 没有这份原始记录，实现 CarLife 蓝牙引导就只能靠猜。
//
// duration_ms 内持续收集；*log 有界（只保留尾部）。
void sniffLink(int fd, int duration_ms, std::string* log);

}  // namespace carlife
