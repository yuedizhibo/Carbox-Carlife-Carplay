// CarLife 内容加密（CONTENT_ENCRYPTION）—— 照抄参考实现，不自行发明。
//
// 依据（全部是现成代码）：
//   Reference/CarLife-Android-Vehicle/src/com/baidu/carlifevehicle/encryption/
//     RSAManager.java      :2048 位 RSA 密钥对；getEncoded() 得到 X.509(SPKI) 公钥 DER → Base64；
//                         解密用 RSA/ECB/PKCS1Padding（EncryptConfig.TRANSFORMATION_SETTING）
//     AESManager.java     :Cipher.getInstance("AES")（= AES/ECB/PKCS5Padding），密钥取 aesKey 的
//                         UTF-8 字节（16/24/32 皆可，参考实现是 16 字节）
//     EncryptSetupManager:① 收到 MD_RSA_PUBLIC_KEY_REQUEST → 回 HU_RSA_PUBLIC_KEY_RESPONSE{公钥}
//                         ② 收到 MD_AES_KEY_SEND_REQUEST{aesKey} → 用【私钥】RSA 解出 AES key
//                         ③ MD_ENCRYPT_READY → 启用（AES 加密开始）
//   .../carlife-sdk/.../internal/protocol/encrypt/EncryptionTool.kt
//                         :encrypt() **只加密 commandSize 之后的 payload**，并同步修正 payloadSize；
//                          decrypt() 失败把该 serviceType 加入 decryptExcludes 并不再重试
//
// 【为什么放在协议层而不是 Session 里】加密是“载荷怎么上/下线”的横切关注点，
// 放在这里可以让 session.cpp 只关心业务；同时也便于单测（不依赖网络）。
//
// 手机校验另由 SdkPhoneVerifier 实现；它不代替转换盒激活，见 API_AUDIT/AUTH.md。
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace carlife {

// AES 密钥允许的长度（字节）。Java 的 SecretKeySpec(bytes,"AES") 接受 16/24/32。
constexpr std::size_t kAesKeyMinBytes = 16;
constexpr std::size_t kAesKeyMaxBytes = 32;
// 参考实现里 AES key 是 UUID 前 16 字符（AesEncryptor.withRandomKey）。
constexpr std::size_t kAesKeyPreferredBytes = 16;

// 内容加密的协商状态（映射到 Core 的 LinkState::content_encryption）。
enum class EncryptionState : uint8_t {
  Off = 0,        // 未启用（能力位为 0 或对端未发起）
  Advertised = 1, // 已把公钥发给手机，等 AES 密钥
  KeyReceived = 2,
  Ready = 3,      // 手机已发 MD_ENCRYPT_READY，之后所有载荷都走 AES
};

const char* toString(EncryptionState s);

// AES-ECB/PKCS5 + RSA-2048，全部走 OpenSSL（板上已验证有 libcrypto + 头文件）。
// 线程安全：Session 的收包在主循环线程，加密开关也只在它上面改，故不加锁；
// 但用过的调用方若要从别处触发，需自行串行化。这一点在注释里写死，避免以后误用。
class ContentCipher {
 public:
  ContentCipher();
  ~ContentCipher();
  ContentCipher(const ContentCipher&) = delete;
  ContentCipher& operator=(const ContentCipher&) = delete;

  // 生成 2048 位 RSA 密钥对（等价于 RSAManager.keyPairGenerate）。
  // 幂等：已生成过就返回 true。
  bool ensure_keypair();
  bool has_keypair() const { return have_keypair_; }

  // X.509(SPKI) DER 的 Base64，NO_WRAP —— 与 Java 的
  //   Base64.encodeToString(mPublicKey.getEncoded(), Base64.NO_WRAP)
  // 逐字节等价（getEncoded() 对 RSA 公钥就是 X.509 SubjectPublicKeyInfo）。
  std::string public_key_base64() const { return public_key_b64_; }

  // 收到 MD_AES_KEY_SEND_REQUEST.aesKey（Base64）→ 用私钥 RSA 解出明文字节。
  // 返回 false 表示 Base64 或 RSA 解密失败（此时保持未启用，绝不用半截密钥）。
  bool install_aes_key_base64(const std::string& key_b64);
  bool has_aes_key() const { return aes_key_size_ > 0; }
  std::size_t aes_key_bits() const { return aes_key_size_ * 8; }

  // 直接安装明文密钥（供本地测试；参考实现里 AES_ENCRYPT_AS_BEGINE=true 就是这条路）。
  bool install_aes_key_raw(const uint8_t* key, std::size_t size);

  // AES-ECB/PKCS5 就地加解密。成功返回 true 并改写 payload。
  // 空载荷直接返回 true（没有可加密的内容，也不该产生一个 16 字节的纯填充块）。
  bool encrypt_payload(std::vector<uint8_t>& payload) const;
  bool decrypt_payload(std::vector<uint8_t>& payload) const;

  // decryptExcludes：解密失败过的 serviceType 不再尝试（照抄 EncryptionTool.kt）。
  void exclude_service(uint32_t service_type) { excludes_.insert(service_type); }
  bool service_excluded(uint32_t service_type) const {
    return excludes_.find(service_type) != excludes_.end();
  }
  void clear_excludes() { excludes_.clear(); }

  EncryptionState state() const { return state_; }
  void set_state(EncryptionState s) { state_ = s; }
  // 会话结束/断开时复位（等价于 AESManager 的 MSG_CONNECT_STATUS_DISCONNECTED 分支）。
  void reset();

  // 密钥材料的存活期与“本机是否接受加密”标志。都是进程级的：密钥不进日志。
  bool enabled() const { return state_ == EncryptionState::Ready; }
  void set_enabled(bool on) { enabled_flag_ = on; }
  bool enabled_flag() const { return enabled_flag_; }

 private:
  bool have_keypair_{};
  std::string public_key_b64_;
  std::vector<uint8_t> aes_key_;
  std::size_t aes_key_size_{};
  std::set<uint32_t> excludes_;
  EncryptionState state_{EncryptionState::Off};
  bool enabled_flag_{};
  void* pkey_{};  // EVP_PKEY*，放 void* 是为了本头文件不引入 OpenSSL
};

// Injectable phone-verification override. Without an override, Session uses
// SdkPhoneVerifier by default; trust/dev remain explicit test-only modes.
class VerifySeam {
 public:
  virtual ~VerifySeam() = default;
  // mdInfoAuthen = mdInfo.brand + mdInfo.model + mdInfo.sdk + connectTime（见 5.4 与
  // ConnectionEstablishHandler.kt:213）；encryptValue 是手机回到 MD_AUTH_RESPONSE 的值。
  virtual bool verify(const std::string& md_info_authen, const std::string& encrypt_value) = 0;
  virtual const char* name() const = 0;
};

// Linux receiver-side equivalent of the supplied SDK's getVerifyResult.
// This verifies the PHONE; it does not issue activation tokens or implement the
// Android application-signature check in getVerifyCode. See API_AUDIT/AUTH.md.
class SdkPhoneVerifier final : public VerifySeam {
 public:
  bool verify(const std::string& md_info_authen, const std::string& encrypt_value) override;
  const char* name() const override { return "sdk-phone-md5"; }
};

// Base64（NO_WRAP 语义，不带换行）—— 与 Java Base64.encodeToString(..., NO_WRAP) 一致。
std::string base64_encode(const uint8_t* data, std::size_t size);
std::string base64_encode(const std::string& s);
bool base64_decode(const std::string& text, std::vector<uint8_t>* out);

// AES key 的生成：照抄 AesEncryptor.withRandomKey() —— UUID 前 16 字符。
// （只用于“我们自己当手机端”的本地测试与 mdsim。）
std::string random_aes_key_16();

}  // namespace carlife
