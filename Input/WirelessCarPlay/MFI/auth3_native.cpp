#include "auth3_core.hpp"
#include "auth3_safety.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <linux/i2c-dev.h>

namespace {
using mfi::auth3::Clock;
using mfi::auth3::Transport;

struct Options {
  unsigned bus = 1;
  unsigned address = 0x10;
  bool json = false;
  bool help = false;
  std::string command;
  bool random = false;
  bool verify_challenge = true;
  std::string challenge;
  unsigned timeout_ms = 800;
};

bool parse_unsigned(std::string_view text, unsigned base, unsigned maximum, unsigned& output) {
  if (text.empty() || text.front() == '+' || text.front() == '-') return false;
  unsigned value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (error != std::errc{} || end != text.data() + text.size() || value > maximum) return false;
  output = value;
  return true;
}

std::string serial_display(const std::array<std::uint8_t, mfi::auth3::kSerialSize>& serial) {
  for (const auto byte : serial) if (byte > 0x7fU) return mfi::auth3::hex_encode(serial.data(), serial.size());
  return std::string(reinterpret_cast<const char*>(serial.data()), serial.size());
}

bool parse_address(std::string_view text, unsigned& output) {
  unsigned base = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { text.remove_prefix(2); base = 16; }
  return parse_unsigned(text, base, 0x7f, output);
}

bool need_value(int& i, int argc, char** argv, std::string_view option, std::string& value, std::string& error) {
  if (++i >= argc) { error = std::string(option) + " requires a value"; return false; }
  value = argv[i]; return true;
}

bool parse_options(int argc, char** argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") { options.help = true; return true; }
    if (arg == "--json") { options.json = true; continue; }
    if (arg == "--random") { options.random = true; continue; }
    if (arg == "--no-verify-challenge") { options.verify_challenge = false; continue; }
    std::string value;
    if (arg == "--bus") { if (!need_value(i, argc, argv, arg, value, error) || !parse_unsigned(value, 10, 9999, options.bus)) { if (error.empty()) error = "--bus must be a decimal adapter number from 0 to 9999"; return false; } continue; }
    if (arg == "--addr") { if (!need_value(i, argc, argv, arg, value, error) || !parse_address(value, options.address)) { if (error.empty()) error = "--addr must be a 7-bit decimal or 0x hexadecimal address"; return false; } continue; }
    if (arg == "--challenge") { if (!need_value(i, argc, argv, arg, value, error)) return false; options.challenge = value; continue; }
    if (arg == "--timeout-ms") { if (!need_value(i, argc, argv, arg, value, error) || !parse_unsigned(value, 10, 60000, options.timeout_ms) || options.timeout_ms == 0) { if (error.empty()) error = "--timeout-ms must be a decimal value from 1 to 60000"; return false; } continue; }
    if (!arg.empty() && arg.front() == '-') { error = "unknown option: " + std::string(arg); return false; }
    if (!options.command.empty()) { error = "only one command is allowed"; return false; }
    options.command = arg;
  }
  if (options.command.empty()) { error = "a command is required"; return false; }
  if (options.command != "info" && options.command != "selftest" && options.command != "serial" && options.command != "certlen" && options.command != "sleep" && options.command != "auth") { error = "unknown command: " + options.command; return false; }
  if (options.command != "auth" && (options.random || !options.challenge.empty() || !options.verify_challenge || options.timeout_ms != 800)) { error = "auth options require the auth command"; return false; }
  if (options.command == "auth" && (options.random == !options.challenge.empty())) { error = "auth requires exactly one of --random or --challenge"; return false; }
  return true;
}

void usage(std::ostream& output) {
  output << "auth3-native [--bus N] [--addr 0x10] [--json] COMMAND\n"
            "Commands: info selftest serial certlen sleep auth (--random | --challenge HEX) [--no-verify-challenge] [--timeout-ms N]\n\n"
            "WARNING: default bus 1/address 0x10 is compatibility only; verify adapter mapping with i2cdetect -l.\n"
            "Never contact an AC200/kernel-bound address. Use the actual external 40-pin adapter with --bus.\n";
}

std::string json_escape(std::string_view input) { std::string output; output.reserve(input.size() + 8); for (unsigned char c : input) { if (c == '\\' || c == '"') { output += '\\'; output += static_cast<char>(c); } else if (c < 0x20) { char escaped[7]{}; std::snprintf(escaped, sizeof(escaped), "\\u%04x", c); output += escaped; } else output += static_cast<char>(c); } return output; }
void print_error(bool json, std::string_view error) { if (json) std::cout << "{\"ok\":false,\"error\":\"" << json_escape(error) << "\"}\n"; else std::cerr << "ERROR: " << error << "\n"; }

class SystemClock final : public Clock {
 public:
  void sleep_ms(unsigned milliseconds) override { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
  std::uint64_t monotonic_ms() override { const auto now = std::chrono::steady_clock::now().time_since_epoch(); return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()); }
};

class LinuxI2cTransport final : public Transport {
 public:
  LinuxI2cTransport(unsigned bus, unsigned address, std::string& error) {
    char path[64]{}; std::snprintf(path, sizeof(path), "/dev/i2c-%u", bus);
    fd_ = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) { error = std::string(path) + " unavailable: " + std::strerror(errno); return; }
    if (::ioctl(fd_, I2C_SLAVE, static_cast<unsigned long>(address)) < 0) { error = "I2C_SLAVE failed: " + std::string(std::strerror(errno)); ::close(fd_); fd_ = -1; }
  }
  ~LinuxI2cTransport() override { if (fd_ >= 0) ::close(fd_); }
  bool ready() const { return fd_ >= 0; }
  bool write(const std::uint8_t* data, std::size_t size, std::string& error) override { const auto result = ::write(fd_, data, size); if (result == static_cast<ssize_t>(size)) return true; error = result < 0 ? std::strerror(errno) : "short I2C write"; return false; }
  bool read(std::uint8_t* data, std::size_t size, std::string& error) override { const auto result = ::read(fd_, data, size); if (result == static_cast<ssize_t>(size)) return true; error = result < 0 ? std::strerror(errno) : "short I2C read"; return false; }
 private: int fd_{-1};
};

class LinuxSysfsReader final : public mfi::auth3::SysfsReader {
 public:
  bool exists(const std::string& path) const override { struct stat status{}; return ::lstat(path.c_str(), &status) == 0; }
  std::string read_text(const std::string& path) const override {
    FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) return {};
    std::array<char, 128> text{};
    const std::size_t count = std::fread(text.data(), 1, text.size() - 1, file);
    std::fclose(file);
    return std::string(text.data(), count);
  }
};

bool random_bytes(std::array<std::uint8_t, mfi::auth3::kChallengeSize>& output, std::string& error) {
  std::size_t offset = 0;
  while (offset < output.size()) { const auto n = ::getrandom(output.data() + offset, output.size() - offset, 0); if (n > 0) { offset += static_cast<std::size_t>(n); continue; } if (n < 0 && errno == EINTR) continue; error = "getrandom failed: " + std::string(std::strerror(errno)); return false; }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  Options options; std::string error;
  if (!parse_options(argc, argv, options, error)) { print_error(options.json, error); return 1; }
  if (options.help) { usage(std::cout); return 0; }
  // Validate supplied hex before touching sysfs or an I2C adapter.
  if (options.command == "auth" && !options.random) {
    auto checked = mfi::auth3::parse_challenge(options.challenge);
    if (!checked.ok) { print_error(options.json, checked.error.message); return 1; }
    mfi::auth3::secure_clear(checked.value.data(), checked.value.size());
  }
  LinuxSysfsReader sysfs;
  const auto safety = mfi::auth3::check_i2c_target_safety(sysfs, options.bus, options.address);
  if (!safety.allowed) { print_error(options.json, safety.message); return 1; }
  LinuxI2cTransport transport(options.bus, options.address, error);
  if (!transport.ready()) { print_error(options.json, error); return 1; }
  SystemClock clock; mfi::auth3::Device device(transport, clock);
  if (options.command == "info") {
    auto info = device.info(); if (!info.ok) { print_error(options.json, info.error.message); return 1; }
    auto serial = device.serial(); if (!serial.ok) { print_error(options.json, serial.error.message); return 1; }
    auto length = device.certificate_length(); if (!length.ok) { print_error(options.json, length.error.message); return 1; }
    const auto id = mfi::auth3::hex_encode(info.value.device_id.data(), info.value.device_id.size()); const auto serial_text = serial_display(serial.value);
    if (options.json) std::cout << "{\"device_version\":\"0x" << mfi::auth3::hex_encode(&info.value.device_version, 1) << "\",\"authentication_revision\":\"0x" << mfi::auth3::hex_encode(&info.value.authentication_revision, 1) << "\",\"protocol_major\":\"0x" << mfi::auth3::hex_encode(&info.value.protocol_major, 1) << "\",\"protocol_minor\":\"0x" << mfi::auth3::hex_encode(&info.value.protocol_minor, 1) << "\",\"device_id\":\"" << id << "\",\"self_test\":\"0x" << mfi::auth3::hex_encode(&info.value.self_test, 1) << "\",\"certificate_present\":" << ((info.value.self_test & 0x80) ? "true" : "false") << ",\"private_key_present\":" << ((info.value.self_test & 0x40) ? "true" : "false") << ",\"certificate_serial\":\"" << json_escape(serial_text) << "\",\"certificate_length\":" << length.value << "}\n";
    else std::cout << "device_version: 0x" << mfi::auth3::hex_encode(&info.value.device_version, 1) << "\nauthentication_revision: 0x" << mfi::auth3::hex_encode(&info.value.authentication_revision, 1) << "\nprotocol_major: 0x" << mfi::auth3::hex_encode(&info.value.protocol_major, 1) << "\nprotocol_minor: 0x" << mfi::auth3::hex_encode(&info.value.protocol_minor, 1) << "\ndevice_id: " << id << "\nself_test: 0x" << mfi::auth3::hex_encode(&info.value.self_test, 1) << "\ncertificate_serial: " << serial_text << "\ncertificate_length: " << length.value << "\n";
    return 0;
  }
  if (options.command == "selftest") { auto value = device.self_test(); if (!value.ok) { print_error(options.json, value.error.message); return 1; } const bool ok = (value.value & 0xc0) == 0xc0; if (options.json) std::cout << "{\"raw\":\"0x" << mfi::auth3::hex_encode(&value.value, 1) << "\",\"certificate_present\":" << ((value.value & 0x80) ? "true" : "false") << ",\"private_key_present\":" << ((value.value & 0x40) ? "true" : "false") << ",\"ok\":" << (ok ? "true" : "false") << "}\n"; else std::cout << "Self-Test: 0x" << mfi::auth3::hex_encode(&value.value, 1) << "\nX.509 Certificate: " << ((value.value & 0x80) ? "PASS" : "FAIL") << "\nPrivate Key: " << ((value.value & 0x40) ? "PASS" : "FAIL") << "\n"; return ok ? 0 : 2; }
  if (options.command == "serial") { auto serial = device.serial(); if (!serial.ok) { print_error(options.json, serial.error.message); return 1; } const auto text = serial_display(serial.value); if (options.json) std::cout << "{\"certificate_serial\":\"" << json_escape(text) << "\"}\n"; else std::cout << text << "\n"; return 0; }
  if (options.command == "certlen") { auto length = device.certificate_length(); if (!length.ok) { print_error(options.json, length.error.message); return 1; } if (options.json) std::cout << "{\"certificate_length\":" << length.value << "}\n"; else std::cout << length.value << "\n"; return 0; }
  if (options.command == "sleep") { auto r = device.sleep(); if (!r.ok) { print_error(options.json, r.error.message); return 1; } if (options.json) std::cout << "{\"ok\":true,\"state\":\"sleep\"}\n"; else std::cout << "OK\n"; return 0; }
  std::array<std::uint8_t, mfi::auth3::kChallengeSize> challenge{};
  auto clear_challenge = [&]() { mfi::auth3::secure_clear(challenge.data(), challenge.size()); };
  if (options.random) { if (!random_bytes(challenge, error)) { clear_challenge(); print_error(options.json, error); return 1; } } else { auto parsed = mfi::auth3::parse_challenge(options.challenge); if (!parsed.ok) { print_error(options.json, parsed.error.message); return 1; } challenge = parsed.value; mfi::auth3::secure_clear(parsed.value.data(), parsed.value.size()); }
  auto response = device.authenticate(challenge, options.timeout_ms, 20, options.verify_challenge);
  if (!response.ok) { clear_challenge(); print_error(options.json, response.error.message); return 1; }
  const auto challenge_hex = mfi::auth3::hex_encode(challenge.data(), challenge.size()); const auto response_hex = mfi::auth3::hex_encode(response.value.data(), response.value.size());
  clear_challenge(); mfi::auth3::secure_clear(response.value.data(), response.value.size());
  if (options.json) std::cout << "{\"ok\":true,\"challenge\":\"" << challenge_hex << "\",\"response\":\"" << response_hex << "\",\"response_length\":64}\n"; else std::cout << "challenge=" << challenge_hex << "\nresponse=" << response_hex << "\n";
  return 0;
}
