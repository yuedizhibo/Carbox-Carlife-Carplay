#include "auth3_core.hpp"
#include "auth3_safety.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#define CHECK(expression) do { if (!(expression)) { std::cerr << "CHECK failed: " #expression " at " << __FILE__ << ':' << __LINE__ << '\n'; std::abort(); } } while (false)

namespace {
class FakeSysfs final : public mfi::auth3::SysfsReader {
 public:
  struct Entry { std::string path; std::string text; };
  bool exists(const std::string& path) const override { for (const auto& entry : entries) if (entry.path == path) return true; return false; }
  std::string read_text(const std::string& path) const override { for (const auto& entry : entries) if (entry.path == path) return entry.text; return {}; }
  void add(std::string path, std::string text = {}) { entries[count++] = {std::move(path), std::move(text)}; }
  std::array<Entry, 8> entries{};
  std::size_t count{};
};

class FakeClock final : public mfi::auth3::Clock {
 public:
  void sleep_ms(unsigned milliseconds) override { now += milliseconds; }
  std::uint64_t monotonic_ms() override { return now; }
  std::uint64_t now{};
};

class FakeTransport final : public mfi::auth3::Transport {
 public:
  bool write(const std::uint8_t* data, std::size_t size, std::string& error) override {
    ++writes;
    if (nack_writes != 0) { --nack_writes; error = "NACK"; return false; }
    if (size == 0) { error = "empty write"; return false; }
    selected = data[0];
    if (size > 1) {
      std::memcpy(registers[selected].data(), data + 1, size - 1);
      lengths[selected] = size - 1;
    }
    return true;
  }
  bool read(std::uint8_t* data, std::size_t size, std::string& error) override {
    ++reads;
    if (selected == 0x10 && nack_status_reads != 0) { --nack_status_reads; error = "NACK"; return false; }
    if (selected == 0x10 && status_index < status_count) data[0] = statuses[status_index++];
    else std::memcpy(data, registers[selected].data(), size);
    return true;
  }
  void set(std::uint8_t reg, const std::uint8_t* data, std::size_t size) { std::memcpy(registers[reg].data(), data, size); lengths[reg] = size; }
  std::array<std::array<std::uint8_t, 64>, 256> registers{};
  std::array<std::size_t, 256> lengths{};
  std::array<std::uint8_t, 8> statuses{};
  std::size_t status_count{};
  std::size_t status_index{};
  std::uint8_t selected{};
  unsigned nack_writes{};
  unsigned nack_status_reads{};
  unsigned writes{};
  unsigned reads{};
};

void configure(FakeTransport& transport, bool good_response = true) {
  const std::uint8_t version = 0x07; transport.set(0x00, &version, 1);
  const std::uint8_t challenge_length[] = {0, 32}; transport.set(0x20, challenge_length, 2);
  const std::uint8_t response_length[] = {0, 64}; transport.set(0x11, response_length, 2);
  std::array<std::uint8_t, 64> response{}; if (good_response) for (std::size_t i = 0; i < response.size(); ++i) response[i] = static_cast<std::uint8_t>(i + 1); transport.set(0x12, response.data(), response.size());
  transport.statuses[0] = 0x10; transport.status_count = 1;
}

std::array<std::uint8_t, 32> challenge() { std::array<std::uint8_t, 32> result{}; for (std::size_t i = 0; i < result.size(); ++i) result[i] = static_cast<std::uint8_t>(i); return result; }

void successful_authentication_and_stop_reads() {
  FakeTransport transport; FakeClock clock; configure(transport);
  mfi::auth3::Device device(transport, clock);
  const auto input = challenge();
  const auto result = device.authenticate(input);
  CHECK(result.ok);
  CHECK(result.value[0] == 1 && result.value[63] == 64);
  CHECK(transport.reads != 0 && transport.writes > transport.reads); // every read had a separate selector write (STOP semantics).
}

void nack_is_retried() {
  FakeTransport transport; FakeClock clock; configure(transport); transport.nack_writes = 1;
  mfi::auth3::Device device(transport, clock);
  const auto result = device.authenticate(challenge());
  CHECK(result.ok);
  CHECK(clock.now >= 2);
}

void status_nacks_are_busy_until_success() {
  FakeTransport transport; FakeClock clock; configure(transport); transport.nack_status_reads = 7;
  mfi::auth3::Device device(transport, clock);
  const auto result = device.authenticate(challenge(), 200, 10);
  CHECK(result.ok);
  CHECK(transport.nack_status_reads == 0);
}

void error_status_is_reported() {
  FakeTransport transport; FakeClock clock; configure(transport); transport.statuses[0] = 0x80; transport.status_count = 1;
  const std::uint8_t code = 0x05; transport.set(0x05, &code, 1);
  mfi::auth3::Device device(transport, clock);
  const auto result = device.authenticate(challenge());
  CHECK(!result.ok && result.error.kind == mfi::auth3::ErrorKind::Protocol);
}

void timeout_is_reported() {
  FakeTransport transport; FakeClock clock; configure(transport); transport.statuses.fill(0); transport.status_count = transport.statuses.size();
  mfi::auth3::Device device(transport, clock);
  const auto result = device.authenticate(challenge(), 30, 10);
  CHECK(!result.ok && result.error.kind == mfi::auth3::ErrorKind::Timeout);
}

void bad_lengths_and_zero_response_are_rejected() {
  FakeTransport transport; FakeClock clock; configure(transport);
  const std::uint8_t wrong_challenge_length[] = {0, 31}; transport.set(0x20, wrong_challenge_length, 2);
  mfi::auth3::Device device(transport, clock);
  auto result = device.authenticate(challenge());
  CHECK(!result.ok && result.error.kind == mfi::auth3::ErrorKind::Protocol);
  configure(transport, false); transport.status_index = 0;
  result = device.authenticate(challenge());
  CHECK(!result.ok && result.error.kind == mfi::auth3::ErrorKind::Protocol);
}

void sysfs_safety_rejects_bound_targets_without_bus_hardcoding() {
  FakeSysfs sysfs;
  sysfs.add("/sys/class/i2c-dev/i2c-2/device/of_node/name", "i2c@5002c00\n");
  sysfs.add("/sys/bus/i2c/devices/2-0010");
  sysfs.add("/sys/bus/i2c/devices/2-0010/name", "ac200\n");
  auto ac200 = mfi::auth3::check_i2c_target_safety(sysfs, 2, 0x10);
  CHECK(!ac200.allowed && ac200.message.find("AC200") != std::string::npos);
  CHECK(ac200.adapter_identity.find("5002c00") != std::string::npos);

  FakeSysfs external;
  external.add("/sys/class/i2c-dev/i2c-1/device/of_node/name", "i2c@5002400\n");
  auto allowed = mfi::auth3::check_i2c_target_safety(external, 1, 0x10);
  CHECK(allowed.allowed && allowed.adapter_identity.find("5002400") != std::string::npos);

  FakeSysfs bound;
  bound.add("/sys/bus/i2c/devices/7-0010");
  bound.add("/sys/bus/i2c/devices/7-0010/driver");
  auto generic = mfi::auth3::check_i2c_target_safety(bound, 7, 0x10);
  CHECK(!generic.allowed && generic.message.find("kernel driver") != std::string::npos);
}

void hex_is_strict() {
  auto good = mfi::auth3::parse_challenge("00:01 02030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  CHECK(good.ok && good.value[31] == 31);
  CHECK(!mfi::auth3::parse_challenge("00").ok);
  CHECK(!mfi::auth3::parse_challenge(std::string(64, 'g')).ok);
  const std::uint8_t endian[] = {0x12, 0x34}; CHECK(mfi::auth3::be16(endian) == 0x1234);
}
}  // namespace

int main() {
  successful_authentication_and_stop_reads();
  nack_is_retried();
  status_nacks_are_busy_until_success();
  error_status_is_reported();
  timeout_is_reported();
  bad_lengths_and_zero_response_are_rejected();
  sysfs_safety_rejects_bound_targets_without_bus_hardcoding();
  hex_is_strict();
  std::cout << "auth3 native tests passed\n";
}
