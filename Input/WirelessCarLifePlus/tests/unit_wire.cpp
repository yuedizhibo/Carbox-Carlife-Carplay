// 帧头 + protobuf 编解码单元测试（全部断言对齐参考实现 CarLifeMessage.kt / BitConverter.kt）
#include <cstdio>
#include <cstring>
#include <iostream>

#include "carlife/service_types.h"
#include "carlife/wire.h"

using namespace carlife;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::cout << "FAIL  " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; \
      ++g_fail;                                                                  \
    } else {                                                                     \
      ++g_pass;                                                                  \
    }                                                                            \
  } while (0)

static void testHeaderSizes() {
  CHECK(headerSize(ch::CMD) == 8);
  CHECK(headerSize(ch::TOUCH) == 8);
  CHECK(headerSize(ch::VIDEO) == 12);
  CHECK(headerSize(ch::AUDIO) == 12);
  CHECK(headerSize(ch::TTS) == 12);
  CHECK(headerSize(ch::VR) == 12);
  CHECK(headerSize(ch::UPDATE) == 12);
}

static void testCmdFrame() {
  Frame f;
  f.channel = ch::CMD;
  f.serviceType = msg::HU_PROTOCOL_VERSION;
  f.payload = {0x11, 0x22, 0x33};
  auto b = f.encode();
  CHECK(b.size() == 8 + 3);
  CHECK(be16_get(b.data()) == 3);          // 前两字节 = 消息体长度(u16)
  CHECK(b[2] == 0 && b[3] == 0);           // 保留
  CHECK(be32_get(b.data() + 4) == msg::HU_PROTOCOL_VERSION);
  CHECK(b[8] == 0x11 && b[10] == 0x33);
  HeaderInfo hi;
  parseHeader(b.data(), ch::CMD, &hi);
  CHECK(hi.payloadSize == 3);
  CHECK(hi.serviceType == msg::HU_PROTOCOL_VERSION);
  CHECK(hi.timestamp == 0);
}

static void testVideoFrame() {
  Frame f;
  f.channel = ch::VIDEO;
  f.serviceType = msg::VIDEO_DATA;
  f.timestamp = 0xDEADBEEF;
  f.payload = {0xAA};
  auto b = f.encode();
  CHECK(b.size() == 13);
  CHECK(be32_get(b.data()) == 1);
  CHECK(be32_get(b.data() + 4) == 0xDEADBEEF);
  CHECK(be32_get(b.data() + 8) == msg::VIDEO_DATA);
  HeaderInfo hi;
  parseHeader(b.data(), ch::VIDEO, &hi);
  CHECK(hi.payloadSize == 1 && hi.timestamp == 0xDEADBEEF);
  CHECK(hi.serviceType == msg::VIDEO_DATA);
}

static void testProtobufGolden() {
  pb::VideoEncoderInfo vi;
  vi.width = 1920;
  vi.height = 1080;
  vi.frameRate = 30;
  auto e = vi.encode();
  // 1920 = 15*128 + 0  -> varint 80 0F ; 1080 = 8*128 + 56 -> varint B8 08 ; 30 -> 1E
  const uint8_t expect[] = {0x08, 0x80, 0x0F, 0x10, 0xB8, 0x08, 0x18, 0x1E};
  CHECK(e.size() == sizeof(expect));
  CHECK(std::memcmp(e.data(), expect, sizeof(expect)) == 0);

  pb::AuthenResult ar;
  ar.authenResult = true;
  auto a = ar.encode();
  CHECK(a.size() == 2 && a[0] == 0x08 && a[1] == 0x01);
  pb::AuthenResult out;
  CHECK(pb::AuthenResult::decode(a.data(), a.size(), &out) && out.authenResult);

  pb::AuthenRequest rq;
  rq.randomValue = "2.0;12:34:56";
  auto r = rq.encode();
  CHECK(r.size() == 2 + 12 && r[0] == 0x0A && r[1] == 12);
}

static void testProtobufRoundTrips() {
  pb::DeviceInfo di;
  di.os = "Linux";
  di.brand = "zero2w";
  di.model = "CarLifeHU-Linux";
  di.sdk = "33";
  di.sdkInt = 33;
  di.token = "tok-123";
  di.btAddress = "00:11:22:33:44:55";
  di.carlifeVersion = "2.0";
  auto e = di.encode();
  pb::DeviceInfo back;
  CHECK(pb::DeviceInfo::decode(e.data(), e.size(), &back));
  CHECK(back.os == "Linux" && back.brand == "zero2w" && back.model == "CarLifeHU-Linux");
  CHECK(back.sdk == "33" && back.sdkInt == 33 && back.token == "tok-123");
  CHECK(back.btAddress == "00:11:22:33:44:55" && back.carlifeVersion == "2.0");

  pb::FeatureConfigList list;
  list.configs = {{feature::kContentEncryption, 0}, {feature::kConnectType, 2}};
  list.huBtAudioSupport = true;
  list.huBtName = "CarLife-HU";
  list.huBtMac = "00:11:22:33:44:55";
  auto le = list.encode();
  pb::FeatureConfigList lback;
  CHECK(pb::FeatureConfigList::decode(le.data(), le.size(), &lback));
  CHECK(lback.configs.size() == 2);
  CHECK(lback.configs[0].key == feature::kContentEncryption && lback.configs[0].value == 0);
  CHECK(lback.configs[1].key == feature::kConnectType && lback.configs[1].value == 2);
  CHECK(lback.huBtAudioSupport && lback.huBtName == "CarLife-HU");
  CHECK(lback.huBtMac == "00:11:22:33:44:55");

  // 带未知字段的消息必须被安全跳过
  PbWriter w;
  w.fieldInt32(1, 1280);
  w.fieldInt32(2, 720);
  w.fieldInt32(3, 25);
  w.fieldString(15, "future-field");
  w.fieldVarint(16, 400);
  pb::VideoEncoderInfo v2;
  CHECK(pb::VideoEncoderInfo::decode(w.data().data(), w.size(), &v2));
  CHECK(v2.width == 1280 && v2.height == 720 && v2.frameRate == 25);

  pb::ProtocolVersionMatchStatus ms;
  ms.matchStatus = 1;
  ms.carlifeProtocolVersion = 2;
  PbWriter mw;
  mw.fieldInt32(1, ms.matchStatus);
  mw.fieldInt32(2, ms.carlifeProtocolVersion);
  pb::ProtocolVersionMatchStatus mback;
  CHECK(pb::ProtocolVersionMatchStatus::decode(mw.data().data(), mw.size(), &mback));
  CHECK(mback.matchStatus == 1 && mback.carlifeProtocolVersion == 2);

  pb::ModuleStatusList mod;
  mod.items = {{2, 1}, {3, 0}};
  PbWriter bw;
  bw.fieldInt32(1, 2);
  for (auto& it : mod.items) {
    PbWriter sub;
    sub.fieldInt32(1, it.first);
    sub.fieldInt32(2, it.second);
    bw.fieldMessage(2, sub);
  }
  pb::ModuleStatusList msl;
  CHECK(pb::ModuleStatusList::decode(bw.data().data(), bw.size(), &msl));
  CHECK(msl.items.size() == 2 && msl.items[0].first == 2 && msl.items[1].second == 0);
}

int main() {
  testHeaderSizes();
  testCmdFrame();
  testVideoFrame();
  testProtobufGolden();
  testProtobufRoundTrips();
  std::cout << "unit-wire: " << g_pass << " passed, " << g_fail << " failed" << std::endl;
  return g_fail ? 1 : 0;
}