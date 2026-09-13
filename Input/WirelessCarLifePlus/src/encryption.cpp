#include "carlife/encryption.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdio>
#include <cstring>
#include <random>

namespace carlife {

bool SdkPhoneVerifier::verify(const std::string& seed, const std::string& response) {
  if (seed.empty() || seed.size() > 4096 || response.size() != 32) return false;
  for (char c : response)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;

  // JNI GetStringUTFChars uses modified UTF-8: supplementary characters are
  // encoded as two UTF-16 surrogates. Protobuf strings arrive as standard UTF-8.
  std::string input = "CARLIFE";
  for (std::size_t i = 0; i < seed.size();) {
    const auto first = static_cast<unsigned char>(seed[i++]);
    uint32_t cp = first;
    unsigned trailing = 0;
    if (first == 0) return false;
    if (first >= 0xc2 && first <= 0xdf) { cp &= 0x1f; trailing = 1; }
    else if (first >= 0xe0 && first <= 0xef) { cp &= 0x0f; trailing = 2; }
    else if (first >= 0xf0 && first <= 0xf4) { cp &= 7; trailing = 3; }
    else if (first >= 0x80) return false;
    if (seed.size() - i < trailing) return false;
    for (unsigned n = 0; n < trailing; ++n) {
      const auto c = static_cast<unsigned char>(seed[i++]);
      if ((c & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3f);
    }
    if ((trailing == 1 && cp < 0x80) || (trailing == 2 && cp < 0x800) ||
        (trailing == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return false;
    auto append16 = [&](uint32_t u) {
      input.push_back(static_cast<char>(0xe0 | (u >> 12)));
      input.push_back(static_cast<char>(0x80 | ((u >> 6) & 63)));
      input.push_back(static_cast<char>(0x80 | (u & 63)));
    };
    if (cp > 0xffff) {
      cp -= 0x10000;
      append16(0xd800 | (cp >> 10));
      append16(0xdc00 | (cp & 1023));
    } else {
      input.append(seed, i - trailing - 1, trailing + 1);
    }
  }
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned len = 0;
  if (EVP_Digest(input.data(), input.size(), digest, &len, EVP_md5(), nullptr) != 1 ||
      len != 16) return false;  // Includes crypto-provider rejection; never trust fallback.
  constexpr char hex[] = "0123456789abcdef";
  unsigned different = 0;
  for (unsigned i = 0; i < 16; ++i) {
    different |= unsigned(hex[digest[i] >> 4] ^ response[i * 2]);
    different |= unsigned(hex[digest[i] & 15] ^ response[i * 2 + 1]);
  }
  return different == 0;
}

const char* toString(EncryptionState s) {
  switch (s) {
    case EncryptionState::Off: return "Off";
    case EncryptionState::Advertised: return "Advertised";
    case EncryptionState::KeyReceived: return "KeyReceived";
    case EncryptionState::Ready: return "Ready";
  }
  return "?";
}

// ---------------------------------------------------------------- base64
std::string base64_encode(const uint8_t* data, std::size_t size) {
  if (!data || !size) return std::string();
  // EVP_EncodeBlock 的输出长度 = 4*ceil(n/3)，且不会插换行（正是 NO_WRAP 语义）。
  const std::size_t cap = 4 * ((size + 2) / 3);
  std::string out(cap, '\0');
  const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), data,
                                static_cast<int>(size));
  if (n <= 0) return std::string();
  out.resize(static_cast<std::size_t>(n));
  return out;
}

std::string base64_encode(const std::string& s) {
  return base64_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

bool base64_decode(const std::string& text, std::vector<uint8_t>* out) {
  if (!out) return false;
  out->clear();
  if (text.empty()) return true;
  // 容忍换行/空白（有些实现回带 '\n'），但拒绝非法字符。
  int pad = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
    if (!ok) return false;
    if (c == '=') ++pad;
  }
  const int cap = static_cast<int>(4 * ((text.size() + 3) / 4));
  if (cap <= 0) return false;
  std::vector<uint8_t> buf(static_cast<std::size_t>(cap) + 4);
  const int n = EVP_DecodeBlock(buf.data(),
                                reinterpret_cast<const unsigned char*>(text.data()),
                                static_cast<int>(text.size()));
  if (n < 0) return false;
  const int len = n - pad;
  if (len < 0) return false;
  out->assign(buf.begin(), buf.begin() + len);
  return true;
}

std::string random_aes_key_16() {
  // 照抄 AesEncryptor.withRandomKey()：UUID 前 16 字符（可打印 ASCII）。
  static const char* kHex = "0123456789abcdef";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, 15);
  std::string s;
  s.reserve(kAesKeyPreferredBytes);
  for (std::size_t i = 0; i < kAesKeyPreferredBytes; ++i) s.push_back(kHex[dist(gen)]);
  return s;
}

// ---------------------------------------------------------------- cipher
ContentCipher::ContentCipher() = default;

ContentCipher::~ContentCipher() {
  if (pkey_) {
    EVP_PKEY_free(static_cast<EVP_PKEY*>(pkey_));
    pkey_ = nullptr;
  }
}

bool ContentCipher::ensure_keypair() {
  if (have_keypair_) return true;
  EVP_PKEY* pkey = nullptr;
  // 2048 位：与 RSAManager.java 的 keygen.initialize(2048, secrand) 一致。
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  if (!ctx) return false;
  bool ok = EVP_PKEY_keygen_init(ctx) == 1 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1 &&
            EVP_PKEY_keygen(ctx, &pkey) == 1;
  EVP_PKEY_CTX_free(ctx);
  if (!ok || !pkey) {
    if (pkey) EVP_PKEY_free(pkey);
    return false;
  }
  // getEncoded() 等价物：X.509 SubjectPublicKeyInfo DER。
  unsigned char* der = nullptr;
  const int der_len = i2d_PUBKEY(pkey, &der);
  if (der_len <= 0 || !der) {
    EVP_PKEY_free(pkey);
    return false;
  }
  public_key_b64_ = base64_encode(der, static_cast<std::size_t>(der_len));
  OPENSSL_free(der);
  pkey_ = pkey;
  have_keypair_ = true;
  return true;
}

bool ContentCipher::install_aes_key_raw(const uint8_t* key, std::size_t size) {
  if (!key) return false;
  if (size < kAesKeyMinBytes || size > kAesKeyMaxBytes) return false;
  // Java 的 SecretKeySpec(bytes,"AES") 只接受 16/24/32，与之一致。
  if (size != 16 && size != 24 && size != 32) return false;
  aes_key_.assign(key, key + size);
  aes_key_size_ = size;
  excludes_.clear();
  state_ = EncryptionState::KeyReceived;
  return true;
}

bool ContentCipher::install_aes_key_base64(const std::string& key_b64) {
  if (!pkey_) {
    // 没有私钥就没法解 —— 绝不猜一个密钥继续（那会把“没实现”伪装成“已实现”）。
    return false;
  }
  std::vector<uint8_t> sealed;
  if (!base64_decode(key_b64, &sealed) || sealed.empty()) return false;
  EVP_PKEY* pkey = static_cast<EVP_PKEY*>(pkey_);
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
  if (!ctx) return false;
  std::size_t out_len = 0;
  bool ok = EVP_PKEY_decrypt_init(ctx) == 1 &&
            // RSA/ECB/PKCS1Padding（EncryptConfig.TRANSFORMATION_SETTING）
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1 &&
            EVP_PKEY_decrypt(ctx, nullptr, &out_len, sealed.data(), sealed.size()) == 1;
  if (ok) {
    std::vector<uint8_t> plain(out_len);
    ok = EVP_PKEY_decrypt(ctx, plain.data(), &out_len, sealed.data(), sealed.size()) == 1;
    if (ok) {
      plain.resize(out_len);
      ok = install_aes_key_raw(plain.data(), plain.size());
      // 明文密钥用完立刻抹掉（本地缓冲，不影响调用方）。
      OPENSSL_cleanse(plain.data(), plain.size());
    }
  }
  EVP_PKEY_CTX_free(ctx);
  return ok;
}

bool ContentCipher::encrypt_payload(std::vector<uint8_t>& payload) const {
  if (!aes_key_size_) return false;
  if (payload.empty()) return true;  // 空载荷不该膨胀成 16 字节填充块
  const EVP_CIPHER* cipher = aes_key_size_ == 32   ? EVP_aes_256_ecb()
                             : aes_key_size_ == 24 ? EVP_aes_192_ecb()
                                                   : EVP_aes_128_ecb();
  const int block = 16;
  const std::size_t pad = static_cast<std::size_t>(block) - (payload.size() % block);
  std::vector<uint8_t> in = payload;
  in.insert(in.end(), pad, static_cast<uint8_t>(pad));  // PKCS5/PKCS7

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  std::vector<uint8_t> out(in.size() + block);
  int len = 0, total = 0;
  bool ok = EVP_EncryptInit_ex(ctx, cipher, nullptr, aes_key_.data(), nullptr) == 1 &&
            EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
            EVP_EncryptUpdate(ctx, out.data(), &len, in.data(), static_cast<int>(in.size())) == 1;
  total = len;
  if (ok) ok = EVP_EncryptFinal_ex(ctx, out.data() + total, &len) == 1;
  if (ok) {
    total += len;
    out.resize(static_cast<std::size_t>(total));
    payload.swap(out);
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

bool ContentCipher::decrypt_payload(std::vector<uint8_t>& payload) const {
  if (!aes_key_size_) return false;
  if (payload.empty()) return true;
  if (payload.size() % 16 != 0) return false;  // 不是块对齐的 AES 密文
  const EVP_CIPHER* cipher = aes_key_size_ == 32   ? EVP_aes_256_ecb()
                             : aes_key_size_ == 24 ? EVP_aes_192_ecb()
                                                   : EVP_aes_128_ecb();
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  std::vector<uint8_t> out(payload.size() + 16);
  int len = 0, total = 0;
  bool ok = EVP_DecryptInit_ex(ctx, cipher, nullptr, aes_key_.data(), nullptr) == 1 &&
            EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
            EVP_DecryptUpdate(ctx, out.data(), &len, payload.data(),
                              static_cast<int>(payload.size())) == 1;
  total = len;
  if (ok) ok = EVP_DecryptFinal_ex(ctx, out.data() + total, &len) == 1;
  if (ok) {
    total += len;
    out.resize(static_cast<std::size_t>(total));
    // 手工去 PKCS7 填充：填 0 或填值越界都视为解密失败。
    const uint8_t pad = out.empty() ? 0 : out.back();
    if (pad == 0 || pad > 16 || pad > out.size()) {
      ok = false;
    } else {
      for (std::size_t i = out.size() - pad; i < out.size(); ++i) {
        if (out[i] != pad) {
          ok = false;
          break;
        }
      }
      if (ok) out.resize(out.size() - pad);
    }
    if (ok) payload.swap(out);
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

void ContentCipher::reset() {
  if (!aes_key_.empty()) OPENSSL_cleanse(aes_key_.data(), aes_key_.size());
  aes_key_.clear();
  aes_key_size_ = 0;
  excludes_.clear();
  state_ = EncryptionState::Off;
  enabled_flag_ = false;
}

}  // namespace carlife
