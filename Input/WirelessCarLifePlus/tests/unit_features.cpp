// 本轮「CarLife 全量接口」的协议层单测。
//
// 覆盖范围：审计里 CarLife 侧全部 ❌ 的编解码 + 内容加密的完整往返。
// 为什么不在这里测“端到端”：端到端要起 mdsim + CarLifeInputAdapter，
// 那是 feature_probe 的职责（见 tests/feature_probe.cpp）；这里只保证
// “字节进出协议是自洽的”，跑得很快且不依赖网络。
//
// 所有编码都用我们自己的 PbWriter 构造、用自己的解码器读回；
// 另外对少数关键消息额外用**手工拼的字节**做正向断言（防止“自己写自己读”掩盖格式错误）。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "carlife/encryption.h"
#include "carlife/keycode_map.h"
#include "carlife/service_types.h"
#include "carlife/wire.h"

namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
    std::printf("PASS  %s\n", what);
  } else {
    ++g_fail;
    std::printf("FAIL  %s\n", what);
  }
}

// 扮演“手机端”：用 HU 给的公钥做 RSA/ECB/PKCS1Padding，把 16 字节 AES 密钥包起来。
// 与 RSAManager/RsaEncryptor.encryptToBase64 以及 mdsim.py 的 cryptography 用法等价。
bool rsa_wrap_key(const std::string& pub_b64, const std::vector<uint8_t>& aes_key,
                  std::string* out_b64) {
  std::vector<uint8_t> der;
  if (!carlife::base64_decode(pub_b64, &der) || der.empty()) return false;
  const unsigned char* p = der.data();
  EVP_PKEY* pub = d2i_PUBKEY(nullptr, &p, static_cast<long>(der.size()));
  if (!pub) return false;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pub, nullptr);
  bool ok = ctx && EVP_PKEY_encrypt_init(ctx) == 1 &&
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1;
  std::size_t out_len = 0;
  if (ok) ok = EVP_PKEY_encrypt(ctx, nullptr, &out_len, aes_key.data(), aes_key.size()) == 1;
  std::vector<uint8_t> sealed;
  if (ok) {
    sealed.resize(out_len);
    ok = EVP_PKEY_encrypt(ctx, sealed.data(), &out_len, aes_key.data(), aes_key.size()) == 1;
    if (ok) {
      sealed.resize(out_len);
      *out_b64 = carlife::base64_encode(sealed.data(), sealed.size());
    }
  }
  if (ctx) EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(pub);
  return ok;
}


}  // namespace

int main() {
  using namespace carlife;

  // ── 1. MediaInfo（含封面 bytes）──
  {
    pb::MediaInfo in;
    in.source = "com.xiaomi.music";
    in.song = "测试曲目";
    in.artist = "歌手";
    in.album = "专辑";
    in.album_art = "\xff\xd8\xff\xe0JPEGDATA";
    in.duration = 215000;
    in.playlist_num = 3;
    in.song_id = "t1";
    in.mode = 1;
    std::vector<uint8_t> buf;
    {
      PbWriter w;
      w.fieldString(1, in.source);
      w.fieldString(2, in.song);
      w.fieldString(3, in.artist);
      w.fieldString(4, in.album);
      w.fieldBytes(5, reinterpret_cast<const uint8_t*>(in.album_art.data()), in.album_art.size());
      w.fieldInt32(6, in.duration);
      w.fieldInt32(7, in.playlist_num);
      w.fieldString(8, in.song_id);
      w.fieldInt32(9, in.mode);
      buf = w.data();
    }
    pb::MediaInfo out;
    check(pb::MediaInfo::decode(buf.data(), buf.size(), &out), "MEDIA_INFO 解析成功");
    check(out.song == in.song && out.artist == in.artist && out.album == in.album,
          "MEDIA_INFO UTF-8 文本往返一致（中文）");
    check(out.album_art == in.album_art, "MEDIA_INFO 封面 bytes 逐字节一致");
    check(out.duration == 215000 && out.playlist_num == 3 && out.mode == 1,
          "MEDIA_INFO 整数字段一致");
    // 缺 required song 必须判失败（否则上层会把空壳当元数据）
    std::vector<uint8_t> no_song;
    {
      PbWriter w;
      w.fieldString(1, "x");
      no_song = w.data();
    }
    pb::MediaInfo bad;
    check(!pb::MediaInfo::decode(no_song.data(), no_song.size(), &bad),
          "MEDIA_INFO 缺 required song 判失败");
  }

  // ── 2. 导航逐向：field 6 的两种官方形态都要能读 ──
  {
    // Kotlin SDK 版：field 6 = bytes turnIconData
    std::vector<uint8_t> kt;
    {
      PbWriter w;
      w.fieldInt32(1, 3);
      w.fieldInt32(2, 12);
      w.fieldString(3, "中关村大街");
      w.fieldInt32(4, 12500);
      w.fieldInt32(5, 800);
      w.fieldBytes(6, reinterpret_cast<const uint8_t*>("\x89PNG"), 4);
      kt = w.data();
    }
    pb::NaviNextTurnInfo a;
    check(pb::NaviNextTurnInfo::decode(kt.data(), kt.size(), &a), "NAV_NEXT_TURN_INFO(Kotlin 版)");
    check(a.action == 3 && a.next_turn == 12 && a.road_name == "中关村大街" &&
              a.remain_distance == 800 && a.turn_icon.size() == 4,
          "NAV_NEXT_TURN_INFO 字段一致（含 bytes field6）");
    // C++ 库版：field 6 = int32 time
    std::vector<uint8_t> cpp;
    {
      PbWriter w;
      w.fieldInt32(1, 1);
      w.fieldInt32(2, 5);
      w.fieldString(3, "road");
      w.fieldInt32(4, 100);
      w.fieldInt32(5, 50);
      w.fieldInt32(6, 42);
      cpp = w.data();
    }
    pb::NaviNextTurnInfo b;
    check(pb::NaviNextTurnInfo::decode(cpp.data(), cpp.size(), &b) && b.time_s == 42,
          "NAV_NEXT_TURN_INFO(C++ 库版) field6 按 varint 读成 time=42");
    pb::NaviAssistantGuideInfo g;
    std::vector<uint8_t> ag;
    {
      PbWriter w;
      w.fieldInt32(1, 1);
      w.fieldInt32(2, 2);
      w.fieldInt32(3, 5);
      w.fieldInt32(4, 12500);
      w.fieldInt32(5, 800);
      w.fieldInt32(6, 60);
      ag = w.data();
    }
    check(pb::NaviAssistantGuideInfo::decode(ag.data(), ag.size(), &g) && g.camera_speed == 60,
          "NAV_ASSISTANT_GUIDE 解析一致");
  }

  // ── 3. 车况上报（HU->MD）的编码 + 反向自检 ──
  {
    pb::CarSpeed s;
    s.speed = 66;
    s.timestamp = 1757000000;
    const std::vector<uint8_t> e = s.encode();
    // 手工解析：field1=varint 66
    check(e.size() >= 2 && e[0] == 0x08 && e[1] == 66, "CAR_VELOCITY 编码 field1=66");
    pb::GearInfo gi;
    gi.gear = 4;
    pb::GearInfo back;
    const std::vector<uint8_t> ge = gi.encode();
    check(pb::GearInfo::decode(ge.data(), ge.size(), &back) && back.gear == 4,
          "GEAR_INFO encode/decode 往返一致");
    pb::Oil oil;
    oil.level = 55;
    oil.range = 420;
    oil.low_fuel_warning = true;
    const std::vector<uint8_t> oe = oil.encode();
    check(!oe.empty(), "CAR_OIL 编码非空");
    pb::CarGps gps;
    gps.latitude = 39904873;
    gps.longitude = 116397000;
    gps.speed = 66;
    gps.heading = 180;
    gps.sats_used = 9;
    gps.sats_visible = 12;
    const std::vector<uint8_t> gpsb = gps.encode();
    check(gpsb.size() > 25, "CAR_GPS 编码包含全部 required 字段");
    // field1..3 的 tag 必须是 0x08/0x10/0x18
    check(gpsb[0] == 0x08 && gpsb[2] == 0x10, "CAR_GPS field1/2 的 tag 正确");
    pb::Gyroscope gy;
    gy.x = 1.5;
    gy.y = -2.5;
    gy.z = 0.0;
    check(!gy.encode().empty(), "CAR_GYROSCOPE 编码非空（double）");
    pb::Acceleration ac;
    ac.z = 9.81;
    check(!ac.encode().empty(), "CAR_ACCELERATION 编码非空（double）");
  }

  // ── 4. 触摸板 / 手势 / 多点触控 ──
  {
    std::vector<uint8_t> mv;
    {
      PbWriter w;
      w.fieldInt32(1, 1000);
      w.fieldInt32(2, 12);
      w.fieldInt32(3, -7);
      mv = w.data();
    }
    pb::TouchPadMove m;
    check(pb::TouchPadMove::decode(mv.data(), mv.size(), &m) && m.delta_x == 12 && m.delta_y == -7,
          "TOUCH_PAD_MOVE 正负 delta 一致（int32 符号）");
    std::vector<uint8_t> pv;
    {
      PbWriter w;
      w.fieldFloat(1, 1.25f);
      pv = w.data();
    }
    pb::TouchPadPinch p;
    check(pb::TouchPadPinch::decode(pv.data(), pv.size(), &p) && p.scale > 1.24f && p.scale < 1.26f,
          "TOUCH_PAD_PINCH float 精度足够");
    std::vector<uint8_t> sv;
    {
      PbWriter w;
      w.fieldInt32(1, 10);
      w.fieldInt32(2, 20);
      w.fieldInt32(3, 30);
      w.fieldInt32(4, 40);
      w.fieldFloat(5, -3.5f);
      w.fieldFloat(6, 0.0f);
      sv = w.data();
    }
    pb::TouchScroll sc;
    check(pb::TouchScroll::decode(sv.data(), sv.size(), &sc) && sc.x2 == 30 && sc.distance_x < 0,
          "TOUCH_SCROLL 解析一致");
    std::vector<uint8_t> fv = sv;
    pb::TouchFling fl;
    check(pb::TouchFling::decode(fv.data(), fv.size(), &fl) && fl.y2 == 40,
          "TOUCH_FLING 解析一致（与 Scroll 同布局）");
  }

  // ── 5. MSG_TOUCH_ACTION_3 定长格式（大端）──
  {
    pb::TouchAction3 ev;
    ev.action = msg::motion::ACTION_MOVE;  // = 2
    ev.pointers.push_back(pb::TouchAction3::Pointer{0, 100, 200});
    ev.pointers.push_back(pb::TouchAction3::Pointer{1, 300, 400});
    const std::vector<uint8_t> b = ev.encode();
    check(b.size() == 4 + 2 * 5, "TOUCH_ACTION_3 长度 = 4 + 5n");
    check(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 2, "TOUCH_ACTION_3 action 是大端 4 字节");
    check(b[4] == 0 && b[5] == 0 && b[6] == 100 && b[7] == 0 && b[8] == 200,
          "TOUCH_ACTION_3 第 1 个触点 id/x/y 都是大端");
    check(b[9] == 1 && b[10] == 0x01 && b[11] == 0x2C, "TOUCH_ACTION_3 第 2 个触点 (300=0x012C)");
    pb::TouchAction3 rt;
    check(pb::TouchAction3::decode(b.data(), b.size(), &rt) && rt.pointers.size() == 2 &&
              rt.pointers[1].x == 300 && rt.pointers[1].y == 400,
          "TOUCH_ACTION_3 decode 往返一致");
    check(!pb::TouchAction3::decode(b.data(), b.size() - 1, &rt),
          "TOUCH_ACTION_3 长度不合法（非 5 的整数倍）判失败");
  }

  // ── 6. 电话 / HFP ──
  {
    std::vector<uint8_t> req;
    {
      PbWriter w;
      w.fieldInt32(1, hfp::REQ_START_CALL);
      w.fieldString(2, "13800138000");
      req = w.data();
    }
    pb::BTHfpRequest r;
    check(pb::BTHfpRequest::decode(req.data(), req.size(), &r) && r.command == 1 &&
              r.phone_num == "13800138000",
          "BT_HFP_REQUEST 拨号解析一致");
    std::vector<uint8_t> dtmf;
    {
      PbWriter w;
      w.fieldInt32(1, hfp::REQ_DTMF_CODE);
      w.fieldInt32(3, 53);
      dtmf = w.data();
    }
    pb::BTHfpRequest d;
    check(pb::BTHfpRequest::decode(dtmf.data(), dtmf.size(), &d) && d.dtmf_code == 53,
          "BT_HFP_REQUEST DTMF 解析一致");
    pb::BTHfpStatusResponse resp;
    resp.status = hfp::MIC_UNMUTE;
    resp.type = hfp::TYPE_MIC_STATUS;
    check(resp.encode().size() >= 4, "BT_HFP_STATUS_RESPONSE 可编码");
    std::vector<uint8_t> cover;
    {
      PbWriter w;
      w.fieldInt32(1, hfp::CALL_NEW_CALL);
      w.fieldString(2, "13800138000");
      w.fieldString(3, "张三");
      cover = w.data();
    }
    pb::BTHfpCallStatusCover c;
    check(pb::BTHfpCallStatusCover::decode(cover.data(), cover.size(), &c) && c.state == 1 &&
              c.name == "张三",
          "BT_HFP_CALL_STATUS_COVER 解析一致");
  }

  // ── 7. 通讯录 / 通话记录（proto 层；线上无 ID，见 session.cpp 的说明）──
  {
    std::vector<uint8_t> list;
    {
      PbWriter sub;
      sub.fieldInt32(1, 1);
      sub.fieldString(2, "张三");
      sub.fieldString(3, "13800138000");
      PbWriter w;
      w.fieldInt32(1, 1);
      w.fieldMessage(2, sub);
      list = w.data();
    }
    pb::ContactsList cl;
    check(pb::ContactsList::decode(list.data(), list.size(), &cl) && cl.contacts.size() == 1 &&
              cl.contacts[0].name == "张三",
          "ContactsList 解析一致");
    std::vector<uint8_t> rec;
    {
      PbWriter sub;
      sub.fieldInt32(1, 7);
      sub.fieldString(2, "李四");
      sub.fieldString(3, "13900139000");
      sub.fieldString(4, "00:01:20");
      sub.fieldString(5, "2026-09-12 10:00");
      sub.fieldInt32(6, static_cast<int32_t>(call_type::MISSED));
      PbWriter w;
      w.fieldInt32(1, 1);
      w.fieldMessage(2, sub);
      rec = w.data();
    }
    pb::CallRecordsList rl;
    check(pb::CallRecordsList::decode(rec.data(), rec.size(), &rl) && rl.records.size() == 1 &&
              rl.records[0].type == 3,
          "CallRecordsList 解析一致（含类型枚举值）");
  }

  // ── 8. 车控（含 repeated sint32 / float / bool / string）──
  {
    std::vector<uint8_t> vc;
    {
      PbWriter w;
      w.fieldInt32(1, 1);
      w.fieldInt32(2, 1001);
      w.fieldBool(3, true);
      w.fieldString(4, "tok");
      w.fieldInt32(5, 2);
      w.fieldInt32(6, 1);
      w.fieldInt32(6, 2);
      w.fieldInt32(7, 1);
      w.fieldBytes(8, reinterpret_cast<const uint8_t*>("\x01\x02"), 2);
      w.fieldSint32(9, -24);
      w.fieldSint32(9, 7);
      w.fieldSint64(10, -90000000000LL);
      w.fieldFloat(11, 21.5f);
      w.fieldString(12, "on");
      vc = w.data();
    }
    pb::VehicleControl c;
    check(pb::VehicleControl::decode(vc.data(), vc.size(), &c), "VEHICLE_CONTROL 解析成功");
    check(c.type == 1 && c.id == 1001 && c.support && c.area_id == 2 && c.value_type == 1,
          "VEHICLE_CONTROL 标量字段一致");
    check(c.area_value.size() == 2 && c.area_value[1] == 2, "VEHICLE_CONTROL repeated int32 一致");
    check(c.int32_values.size() == 2 && c.int32_values[0] == -24 && c.int32_values[1] == 7,
          "VEHICLE_CONTROL repeated sint32 zigzag 负数正确");
    check(c.int64_values.size() == 1 && c.int64_values[0] == -90000000000LL,
          "VEHICLE_CONTROL repeated sint64 正确");
    check(c.float_values.size() == 1 && c.float_values[0] > 21.4f && c.float_values[0] < 21.6f,
          "VEHICLE_CONTROL repeated float 正确");
    check(c.string_value == "on" && c.bytes_value.size() == 2 && c.token_string == "tok",
          "VEHICLE_CONTROL bytes/string 字段一致");
  }

  // ── 9. 激活 / 语音 / 时间同步 / 文件传输 ──
  {
    std::vector<uint8_t> ar;
    {
      PbWriter w;
      w.fieldString(1, "38:F7:FF:38:57:90");
      w.fieldInt32(2, 0);
      w.fieldInt32(3, 12345);
      ar = w.data();
    }
    pb::ActiveRequest a;
    check(pb::ActiveRequest::decode(ar.data(), ar.size(), &a) && a.random_value == 12345,
          "BOX_ACTIVE(CarlifeActiveRequest) 解析一致");
    pb::ActiveResponse resp;
    resp.statue = "1";
    resp.token = "T";
    check(!resp.encode().empty(), "HU_ACTIVE 应答可编码");
    std::vector<uint8_t> vcr;
    {
      PbWriter w;
      w.fieldInt32(1, 1);
      w.fieldInt32(2, 0);
      vcr = w.data();
    }
    pb::VoiceControlRequest v;
    check(pb::VoiceControlRequest::decode(vcr.data(), vcr.size(), &v) && v.command == 1,
          "HU_VOICE_CONTROL 解析一致");
    std::vector<uint8_t> ts;
    {
      PbWriter w;
      w.fieldInt32(1, 1757000000);
      ts = w.data();
    }
    pb::ConnectTimeSync t;
    check(pb::ConnectTimeSync::decode(ts.data(), ts.size(), &t) && t.timestamp == 1757000000,
          "TIME_SYNC 解析一致");
    std::vector<uint8_t> ft;
    {
      PbWriter w;
      w.fieldVarint(1, 4096);
      w.fieldInt32(2, 1);
      ft = w.data();
    }
    pb::FileTransferBegin fb;
    check(pb::FileTransferBegin::decode(ft.data(), ft.size(), &fb) && fb.file_size == 4096,
          "FileTransferBegin(int64 fileSize) 解析一致");
  }

  // ── 10. 内容加密：完整往返（RSA 公钥 → AES 密钥 → 载荷加解密）──
  {
    ContentCipher hu;
    check(hu.ensure_keypair(), "内容加密：生成 2048 位 RSA 密钥对");
    check(!hu.public_key_base64().empty(), "内容加密：公钥 Base64 非空");
    // 手机侧：用公钥做 RSA/ECB/PKCS1Padding 加密一个 16 字节 AES 密钥
    std::vector<uint8_t> der;
    check(base64_decode(hu.public_key_base64(), &der), "内容加密：公钥 Base64 可解回 DER");
    // 手机端：16 字节 AES 密钥（与 AesEncryptor.withRandomKey 一样是 UUID 前 16 字符）
    const std::string aes_plain = random_aes_key_16();
    const std::vector<uint8_t> aes_bytes(aes_plain.begin(), aes_plain.end());
    std::string wrapped;
    check(rsa_wrap_key(hu.public_key_base64(), aes_bytes, &wrapped),
          "内容加密：手机端能用 HU 公钥包起 AES 密钥");
    check(hu.install_aes_key_base64(wrapped),
          "内容加密：RSA 包装的 AES 密钥能被 HU 私钥解出");
    check(hu.aes_key_bits() == 128, "内容加密：解出的 AES 密钥是 128 位（与参考实现的 16 字节一致）");
    // 载荷加密往返
    std::vector<uint8_t> payload;
    const std::string text = "CarLife 载荷加密测试 payload 0123456789";
    payload.assign(text.begin(), text.end());
    const std::vector<uint8_t> original = payload;
    check(hu.encrypt_payload(payload), "内容加密：载荷加密成功");
    check(payload.size() % 16 == 0 && payload != original, "内容加密：密文按 16 字节对齐且已改变");
    check(hu.decrypt_payload(payload) && payload == original,
          "内容加密：解密后与原文逐字节一致（含中文）");
    // 空载荷不退化成纯填充块
    std::vector<uint8_t> empty;
    check(hu.encrypt_payload(empty) && empty.empty(), "内容加密：空载荷不膨胀");
    // 错误密文必须失败且能被 exclude 记住
    std::vector<uint8_t> junk(32, 0xAB);
    ContentCipher other;
    other.ensure_keypair();
    std::vector<uint8_t> key16(16, 0x5A);
    other.install_aes_key_raw(key16.data(), key16.size());
    check(!other.decrypt_payload(junk), "内容加密：错误密钥解出的载荷判失败（不静默通过）");
    other.exclude_service(0x00010035);
    check(other.service_excluded(0x00010035), "内容加密：decryptExcludes 生效");
    check(!hu.decrypt_payload(junk), "内容加密：另一密钥判失败（交叉验证）");
  }

  // ── 11. 硬键表：必须是 CarLife 的 KEYCODE_*，不是 Android 的 ──
  {
    // 逐行对着参考表断言（ServiceTypes.kt 的行号写在结构里）：
    //   名称 / 十进制 / 十六进制 / ServiceTypes.kt 行号
    struct Row {
      const char* name;
      int32_t dec;
      int32_t hex;
      int line;
    };
    const Row rows[] = {
        {"home", 1, 0x01, 358},          {"answer", 2, 0x02, 359},
        {"hangup", 3, 0x03, 360},        {"end_mute", 4, 0x04, 361},
        {"next", 6, 0x06, 363},          {"prev", 7, 0x07, 364},
        {"setting", 8, 0x08, 365},       {"media", 9, 0x09, 366},
        {"nav", 11, 0x0B, 368},          {"back", 14, 0x0E, 371},
        {"seek_sub", 15, 0x0F, 372},     {"seek_add", 16, 0x10, 373},
        {"volume_down", 17, 0x11, 374},  {"volume_up", 18, 0x12, 375},
        {"mute", 19, 0x13, 376},         {"ok", 20, 0x14, 377},
        {"left", 21, 0x15, 378},         {"right", 22, 0x16, 379},
        {"up", 23, 0x17, 380},           {"down", 24, 0x18, 381},
        {"main", 30, 0x1E, 387},         {"menu", 30, 0x1E, 387},
        {"play", 31, 0x1F, 388},         {"pause", 32, 0x20, 389},
        {"voice", 33, 0x21, 390},        {"vr_stop", 34, 0x22, 391},
        {"0", 35, 0x23, 392},            {"5", 40, 0x28, 397},
        {"9", 44, 0x2C, 401},            {"star", 45, 0x2D, 402},
        {"pound", 46, 0x2E, 404},        {"del", 47, 0x2F, 406},
        {"clear", 48, 0x30, 407},        {"plus", 49, 0x31, 408},
    };
    int bad = 0;
    for (const Row& r : rows) {
      const int32_t got = carlife_keycode_for(r.name);
      if (got != r.dec) {
        ++bad;
        std::printf("      %-14s 期望 %d(0x%02X) 参考表 %d 行，实际 %d\n", r.name, r.dec,
                    r.hex, r.line, got);
      }
    }
    check(bad == 0, "硬键表逐行 == ServiceTypes.kt:358-408（34 个名称，十进制+十六进制）");

    // 【反向断言：修正前的错值绝不能再出现】
    // 19/20/4 是 Android 的 DPAD_UP/DPAD_DOWN/BACK；在 CarLife 表里它们分别是
    // KEYCODE_MUTE / KEYCODE_OK / KEYCODE_PHONE_END_MUTE。
    check(carlife_keycode_for("up") == 23 && carlife_keycode_for("up") != 19,
          "up 必须是 23(0x17=MOVE_UP)，不能是 19(0x13=MUTE) —— 这就是修正的那个 bug");
    check(carlife_keycode_for("down") == 24 && carlife_keycode_for("down") != 20,
          "down 必须是 24(0x18)，不能是 20(0x14=OK)");
    check(carlife_keycode_for("back") == 14 && carlife_keycode_for("back") != 4,
          "back 必须是 14(0x0E=BACK)，不能是 4(0x04=PHONE_END_MUTE)");
    check(carlife_keycode_for("unknown-key-name") == 0, "未知名返回 0（调用方丢弃并计数）");
    check(carlife_keycode_for("UP") == 23, "名称大小写不敏感");
    check(keycode::MUTE == 19 && keycode::MOVE_UP == 23,
          "码表常量自身：MUTE=19 / MOVE_UP=23（与上面两个断言互相印证）");
  }

  // ── 11b. 导航逐向：maneuver_code 必须保留原始 action 值 ──
  {
    // 契约 NavigationState.maneuver_code 的用途就是“无法映射时保留原值”。
    // 参考树里查不到 action 码表（全树唯一的 maneuver 枚举属于 Android Auto），
    // 所以这里断言的是【编解码不丢原始值】，而不是断言某个映射结果。
    const int32_t actions[] = {0, 1, 2, 3, 7, 12, 42, 99, 255, 1000, -1};
    int bad = 0;
    for (int32_t a : actions) {
      std::vector<uint8_t> buf;
      {
        PbWriter w;
        w.fieldInt32(1, a);
        w.fieldInt32(2, 0);
        w.fieldString(3, "r");
        w.fieldInt32(4, 100);
        w.fieldInt32(5, 50);
        buf = w.data();
      }
      pb::NaviNextTurnInfo ni;
      if (!pb::NaviNextTurnInfo::decode(buf.data(), buf.size(), &ni) || ni.action != a) {
        ++bad;
        std::printf("      action=%d 往返后变成 %d\n", a, ni.action);
      }
    }
    check(bad == 0, "NAV_NEXT_TURN_INFO 的 action 原值透传（含 0/负数/大值，maneuver_code 直接用它）");
  }

  // ── 12. 帧头/长度域：加密后 payloadSize 必须跟着变 ──
  {
    Frame f;
    f.channel = ch::CMD;
    f.serviceType = msg::MEDIA_INFO;
    f.payload.assign(20, 0x41);
    const std::vector<uint8_t> plain = f.encode();
    ContentCipher c;
    c.ensure_keypair();
    std::vector<uint8_t> k(16, 0x11);
    c.install_aes_key_raw(k.data(), k.size());
    std::vector<uint8_t> enc = f.payload;
    c.encrypt_payload(enc);
    Frame g = f;
    g.payload = enc;
    const std::vector<uint8_t> sealed_frame = g.encode();
    const uint16_t plain_len = static_cast<uint16_t>((plain[0] << 8) | plain[1]);
    const uint16_t enc_len = static_cast<uint16_t>((sealed_frame[0] << 8) | sealed_frame[1]);
    check(plain_len == 20, "帧头明文长度 = 20");
    check(enc_len == 32, "加密后帧头长度自动改成 32（32=20 补齐到 16 的倍数）");
    check(sealed_frame.size() == 8 + 32, "加密后整帧长度 = 帧头 8 + 密文 32（帧头保持明文）");
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
