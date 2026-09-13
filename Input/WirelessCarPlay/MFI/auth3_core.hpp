#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mfi::auth3 {

constexpr std::size_t kChallengeSize = 32;
constexpr std::size_t kResponseSize = 64;
constexpr std::size_t kSerialSize = 32;

enum class ErrorKind { Transport, Protocol, Timeout, InvalidArgument, Random };

struct Error {
  ErrorKind kind{};
  std::string message;
};

template <typename T> struct Result {
  T value{};
  Error error{};
  bool ok{false};
  static Result success(T value) { return {std::move(value), {}, true}; }
  static Result failure(ErrorKind kind, std::string message) { return {{}, {kind, std::move(message)}, false}; }
};

template <> struct Result<void> {
  Error error{};
  bool ok{false};
  static Result success() { return {{}, true}; }
  static Result failure(ErrorKind kind, std::string message) { return {{kind, std::move(message)}, false}; }
};

class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool write(const std::uint8_t* data, std::size_t size, std::string& error) = 0;
  virtual bool read(std::uint8_t* data, std::size_t size, std::string& error) = 0;
};

class Clock {
 public:
  virtual ~Clock() = default;
  virtual void sleep_ms(unsigned milliseconds) = 0;
  virtual std::uint64_t monotonic_ms() = 0;
};

struct Info {
  std::uint8_t device_version{};
  std::uint8_t authentication_revision{};
  std::uint8_t protocol_major{};
  std::uint8_t protocol_minor{};
  std::array<std::uint8_t, 4> device_id{};
  std::uint8_t self_test{};
};

class Device {
 public:
  Device(Transport& transport, Clock& clock, unsigned retries = 5, unsigned retry_delay_ms = 2);
  Result<void> wake();
  Result<Info> info();
  Result<std::array<std::uint8_t, kSerialSize>> serial();
  Result<std::uint16_t> certificate_length();
  Result<std::uint8_t> self_test();
  Result<void> sleep();
  Result<std::array<std::uint8_t, kResponseSize>> authenticate(
      const std::array<std::uint8_t, kChallengeSize>& challenge, unsigned timeout_ms = 800,
      unsigned poll_interval_ms = 20, bool verify_challenge = true);

 private:
  Result<void> write_register(std::uint8_t reg, const std::uint8_t* data = nullptr, std::size_t size = 0);
  Result<void> read_register(std::uint8_t reg, std::uint8_t* output, std::size_t size, unsigned select_delay_ms = 2);
  Result<std::uint8_t> error_code();
  Transport& transport_;
  Clock& clock_;
  unsigned retries_;
  unsigned retry_delay_ms_;
};

Result<std::array<std::uint8_t, kChallengeSize>> parse_challenge(std::string_view text);
std::string hex_encode(const std::uint8_t* bytes, std::size_t size);
std::uint16_t be16(const std::uint8_t* bytes);
void secure_clear(void* ptr, std::size_t size);

}  // namespace mfi::auth3
