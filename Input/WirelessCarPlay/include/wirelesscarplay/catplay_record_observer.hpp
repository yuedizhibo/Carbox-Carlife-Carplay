#pragma once
// 原始 CPMF 记录观察者（可选）：让 Core/Forward 能在 web 预览用存储做裁决之前，
// 直接看到解析器已校验过的原始记录 —— 包括被 store 丢弃的那些（例如音乐播放时的
// 编码音频）。观察者只负责“看见字节”，会话/流过滤等语义由它自己决定。
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "wirelesscarplay/catplay_media_protocol.hpp"

namespace mvp {

// 实现方必须遵守的契约（客户端只保证下面的调用条件，不保证别的）：
//  * 两个回调都在媒体消费线程（CatPlayMediaClient::run 所在线程）上【同步】调用，
//    调用时客户端不持任何内部锁；回调返回前客户端不会再处理下一条记录。
//  * on_catplay_record 的 payload 是由解析器缓冲区借出的视图，【仅在本次回调期间有效】。
//    需要留存（拷贝进队列/缓冲）必须在回调内完成，绝不许保存 span 或其中的指针。
//  * 回调必须 noexcept 且【不得阻塞】：它是消费线程上的同步调用，阻塞会拖住整条
//    记录消费链路（含心跳/活性判断）。也不得回调回客户端对象（重入会破坏记录顺序）。
//  * 每条通过协议校验的记录恰好回调一次（hello / 会话起止 / 媒体与扩展记录都包含，
//    被 store 拒收的记录同样会到达）；握手之前或校验失败的记录不会到达。
//  * 链路失败时 on_catplay_disconnect 每次失败调用恰好一次；观察者必须把重置做成
//    幂等的（可能重复收到，例如已经断开后再次 fail）。原始记录可能先于 Core 的
//    状态落地到达（Core 的 pump 稍后才跑），实现方不能假设 Core 已经收到同一记录。
class CatPlayRecordObserver {
 public:
  virtual ~CatPlayRecordObserver() = default;
  virtual void on_catplay_record(const catplay_media::Header&, std::span<const uint8_t>) noexcept = 0;
  virtual void on_catplay_disconnect() noexcept = 0;
};

}  // namespace mvp
