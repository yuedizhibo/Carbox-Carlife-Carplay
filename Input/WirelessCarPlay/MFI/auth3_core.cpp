#include "auth3_core.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace mfi::auth3 {
namespace {
constexpr std::uint8_t kDeviceVersion = 0x00;
constexpr std::uint8_t kAuthRevision = 0x01;
constexpr std::uint8_t kProtocolMajor = 0x02;
constexpr std::uint8_t kProtocolMinor = 0x03;
constexpr std::uint8_t kDeviceId = 0x04;
constexpr std::uint8_t kErrorCode = 0x05;
constexpr std::uint8_t kAuthControlStatus = 0x10;
constexpr std::uint8_t kResponseLength = 0x11;
constexpr std::uint8_t kResponseData = 0x12;
constexpr std::uint8_t kChallengeLength = 0x20;
constexpr std::uint8_t kChallengeData = 0x21;
constexpr std::uint8_t kCertLength = 0x30;
constexpr std::uint8_t kSelfTest = 0x40;
constexpr std::uint8_t kCertSerial = 0x4e;
constexpr std::uint8_t kSleep = 0x60;

Result<void> protocol_error(const char* message) { return Result<void>::failure(ErrorKind::Protocol, message); }

struct ClearAtExit {
  void* data;
  std::size_t size;
  ~ClearAtExit() { secure_clear(data, size); }
};
}  // namespace

Device::Device(Transport& transport, Clock& clock, unsigned retries, unsigned retry_delay_ms)
    : transport_(transport), clock_(clock), retries_(retries == 0 ? 1 : retries), retry_delay_ms_(retry_delay_ms) {}

Result<void> Device::write_register(std::uint8_t reg, const std::uint8_t* data, std::size_t size) {
  if (size > kResponseSize) return Result<void>::failure(ErrorKind::InvalidArgument, "write length exceeds fixed buffer");
  std::array<std::uint8_t, kResponseSize + 1> payload{};
  payload[0] = reg;
  if (size != 0) std::copy_n(data, size, payload.data() + 1);
  std::string last_error;
  for (unsigned attempt = 0; attempt < retries_; ++attempt) {
    std::string error;
    if (transport_.write(payload.data(), size + 1, error)) return Result<void>::success();
    last_error = error.empty() ? "I2C write failed" : error;
    clock_.sleep_ms(retry_delay_ms_);
  }
  return Result<void>::failure(ErrorKind::Transport, "write register failed: " + last_error);
}

Result<void> Device::read_register(std::uint8_t reg, std::uint8_t* output, std::size_t size, unsigned select_delay_ms) {
  if (size == 0 || size > kResponseSize) return Result<void>::failure(ErrorKind::InvalidArgument, "invalid register read length");
  std::string last_error;
  for (unsigned attempt = 0; attempt < retries_; ++attempt) {
    std::string error;
    const std::uint8_t selector[] = {reg};
    // Deliberately separate write and read calls: selector receives STOP, not repeated-start.
    if (!transport_.write(selector, sizeof(selector), error)) {
      last_error = error.empty() ? "I2C register select failed" : error;
      clock_.sleep_ms(retry_delay_ms_);
      continue;
    }
    clock_.sleep_ms(select_delay_ms);
    error.clear();
    if (transport_.read(output, size, error)) return Result<void>::success();
    last_error = error.empty() ? "I2C read failed" : error;
    clock_.sleep_ms(retry_delay_ms_);
  }
  char message[128]{};
  std::snprintf(message, sizeof(message), "read register 0x%02x failed: %s", reg, last_error.c_str());
  return Result<void>::failure(ErrorKind::Transport, message);
}

Result<void> Device::wake() {
  const std::uint8_t selector[] = {kDeviceVersion};
  std::string ignored;
  (void)transport_.write(selector, sizeof(selector), ignored);
  clock_.sleep_ms(retry_delay_ms_);
  std::uint8_t version{};
  auto read = read_register(kDeviceVersion, &version, 1);
  if (!read.ok) return read;
  if (version != 0x07) return protocol_error("wake Device Version is not 0x07");
  return Result<void>::success();
}

Result<Info> Device::info() {
  Info result{};
  auto one = [&](std::uint8_t reg, std::uint8_t& value) { return read_register(reg, &value, 1); };
  auto r = one(kDeviceVersion, result.device_version); if (!r.ok) return Result<Info>::failure(r.error.kind, r.error.message);
  r = one(kAuthRevision, result.authentication_revision); if (!r.ok) return Result<Info>::failure(r.error.kind, r.error.message);
  r = one(kProtocolMajor, result.protocol_major); if (!r.ok) return Result<Info>::failure(r.error.kind, r.error.message);
  r = one(kProtocolMinor, result.protocol_minor); if (!r.ok) return Result<Info>::failure(r.error.kind, r.error.message);
  r = read_register(kDeviceId, result.device_id.data(), result.device_id.size()); if (!r.ok) return Result<Info>::failure(r.error.kind, r.error.message);
  r = one(kSelfTest, result.self_test); if (!r.ok) return Result<Info>::failure(r.error.kind, r.error.message);
  return Result<Info>::success(result);
}

Result<std::array<std::uint8_t, kSerialSize>> Device::serial() {
  std::array<std::uint8_t, kSerialSize> result{};
  auto r = read_register(kCertSerial, result.data(), result.size());
  if (!r.ok) return Result<decltype(result)>::failure(r.error.kind, r.error.message);
  return Result<decltype(result)>::success(result);
}

Result<std::uint16_t> Device::certificate_length() {
  std::array<std::uint8_t, 2> raw{};
  auto r = read_register(kCertLength, raw.data(), raw.size(), 12);  // tCERT max 10 ms.
  if (!r.ok) return Result<std::uint16_t>::failure(r.error.kind, r.error.message);
  return Result<std::uint16_t>::success(be16(raw.data()));
}

Result<std::uint8_t> Device::self_test() {
  std::uint8_t result{};
  auto r = read_register(kSelfTest, &result, 1);
  if (!r.ok) return Result<std::uint8_t>::failure(r.error.kind, r.error.message);
  return Result<std::uint8_t>::success(result);
}

Result<std::uint8_t> Device::error_code() {
  std::uint8_t value{};
  auto r = read_register(kErrorCode, &value, 1);
  if (!r.ok) return Result<std::uint8_t>::failure(r.error.kind, r.error.message);
  return Result<std::uint8_t>::success(value);
}

Result<void> Device::sleep() { const std::uint8_t value = 0xee; return write_register(kSleep, &value, 1); }

Result<std::array<std::uint8_t, kResponseSize>> Device::authenticate(
    const std::array<std::uint8_t, kChallengeSize>& challenge, unsigned timeout_ms,
    unsigned poll_interval_ms, bool verify_challenge) {
  auto wake_result = wake(); if (!wake_result.ok) return Result<std::array<std::uint8_t, kResponseSize>>::failure(wake_result.error.kind, wake_result.error.message);
  auto written = write_register(kChallengeData, challenge.data(), challenge.size()); if (!written.ok) return Result<std::array<std::uint8_t, kResponseSize>>::failure(written.error.kind, written.error.message);
  std::array<std::uint8_t, 2> length{};
  auto r = read_register(kChallengeLength, length.data(), length.size()); if (!r.ok) return Result<std::array<std::uint8_t, kResponseSize>>::failure(r.error.kind, r.error.message);
  if (be16(length.data()) != kChallengeSize) return Result<std::array<std::uint8_t, kResponseSize>>::failure(ErrorKind::Protocol, "Challenge Data Length is not 32");
  std::array<std::uint8_t, kChallengeSize> readback{};
  ClearAtExit clear_readback{readback.data(), readback.size()};
  if (verify_challenge) {
    r = read_register(kChallengeData, readback.data(), readback.size());
    if (!r.ok) return Result<std::array<std::uint8_t, kResponseSize>>::failure(r.error.kind, r.error.message);
    if (readback != challenge) return Result<std::array<std::uint8_t, kResponseSize>>::failure(ErrorKind::Protocol, "Challenge readback mismatch");
  }
  const std::uint8_t start = 1;
  r = write_register(kAuthControlStatus, &start, 1); if (!r.ok) return Result<std::array<std::uint8_t, kResponseSize>>::failure(r.error.kind, r.error.message);
  const std::uint64_t deadline = clock_.monotonic_ms() + timeout_ms;
  bool complete = false;
  while (clock_.monotonic_ms() < deadline) {
    clock_.sleep_ms(poll_interval_ms);
    std::uint8_t status{};
    r = read_register(kAuthControlStatus, &status, 1);
    // During tAUTH the coprocessor can NACK status accesses.  Treat exhausted
    // transport retries as busy until the overall authentication deadline.
    if (!r.ok) {
      if (r.error.kind == ErrorKind::Transport) continue;
      return Result<std::array<std::uint8_t, kResponseSize>>::failure(r.error.kind, r.error.message);
    }
    if ((status & 0x80U) != 0U) {
      auto code = error_code();
      if (!code.ok) return Result<std::array<std::uint8_t, kResponseSize>>::failure(code.error.kind, code.error.message);
      char message[80]{}; std::snprintf(message, sizeof(message), "authentication failed: Error Code 0x%02x", code.value);
      return Result<std::array<std::uint8_t, kResponseSize>>::failure(ErrorKind::Protocol, message);
    }
    if (((status >> 4U) & 0x07U) == 1U) { complete = true; break; }
  }
  if (!complete) return Result<std::array<std::uint8_t, kResponseSize>>::failure(ErrorKind::Timeout, "authentication calculation timed out");
  r = read_register(kResponseLength, length.data(), length.size()); if (!r.ok) return Result<std::array<std::uint8_t, kResponseSize>>::failure(r.error.kind, r.error.message);
  if (be16(length.data()) != kResponseSize) return Result<std::array<std::uint8_t, kResponseSize>>::failure(ErrorKind::Protocol, "Challenge Response Length is not 64");
  std::array<std::uint8_t, kResponseSize> response{};
  ClearAtExit clear_response{response.data(), response.size()};
  r = read_register(kResponseData, response.data(), response.size());
  if (!r.ok) return Result<decltype(response)>::failure(r.error.kind, r.error.message);
  if (std::all_of(response.begin(), response.end(), [](std::uint8_t value) { return value == 0; })) return Result<decltype(response)>::failure(ErrorKind::Protocol, "Challenge Response is all zero");
  return Result<decltype(response)>::success(response);
}

Result<std::array<std::uint8_t, kChallengeSize>> parse_challenge(std::string_view text) {
  std::array<char, kChallengeSize * 2> compact{};
  std::size_t n = 0;
  for (char c : text) {
    if (c == ' ' || c == ':') continue;
    if (n == compact.size()) return Result<std::array<std::uint8_t, kChallengeSize>>::failure(ErrorKind::InvalidArgument, "--challenge must contain exactly 64 hex characters");
    compact[n++] = c;
  }
  if (n != compact.size()) return Result<std::array<std::uint8_t, kChallengeSize>>::failure(ErrorKind::InvalidArgument, "--challenge must contain exactly 64 hex characters");
  auto nibble = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
  std::array<std::uint8_t, kChallengeSize> result{};
  for (std::size_t i = 0; i < result.size(); ++i) { int high = nibble(compact[i * 2]); int low = nibble(compact[i * 2 + 1]); if (high < 0 || low < 0) return Result<decltype(result)>::failure(ErrorKind::InvalidArgument, "--challenge contains invalid hex"); result[i] = static_cast<std::uint8_t>((high << 4) | low); }
  return Result<decltype(result)>::success(result);
}

std::string hex_encode(const std::uint8_t* bytes, std::size_t size) { static constexpr char hex[] = "0123456789abcdef"; std::string output; output.resize(size * 2); for (std::size_t i = 0; i < size; ++i) { output[i * 2] = hex[bytes[i] >> 4U]; output[i * 2 + 1] = hex[bytes[i] & 0x0fU]; } return output; }
std::uint16_t be16(const std::uint8_t* bytes) { return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]); }
void secure_clear(void* ptr, std::size_t size) { volatile auto* output = static_cast<volatile std::uint8_t*>(ptr); while (size-- != 0) *output++ = 0; }

}  // namespace mfi::auth3
