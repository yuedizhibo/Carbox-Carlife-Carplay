#include "auth3_safety.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

namespace mfi::auth3 {
namespace {
std::string target_base(unsigned bus, unsigned address) {
  std::array<char, 128> path{};
  std::snprintf(path.data(), path.size(), "/sys/bus/i2c/devices/%u-%04x", bus, address);
  return path.data();
}

bool contains_case_insensitive(std::string_view text, std::string_view needle) {
  if (needle.empty() || text.size() < needle.size()) return false;
  for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
    std::size_t i = 0;
    for (; i < needle.size(); ++i) {
      const auto left = static_cast<unsigned char>(text[start + i]);
      const auto right = static_cast<unsigned char>(needle[i]);
      if (std::tolower(left) != std::tolower(right)) break;
    }
    if (i == needle.size()) return true;
  }
  return false;
}
}  // namespace

SafetyDecision check_i2c_target_safety(const SysfsReader& sysfs, unsigned bus, unsigned address) {
  const std::string base = target_base(bus, address);
  const std::string adapter_node = "/sys/class/i2c-dev/i2c-" + std::to_string(bus) + "/device/of_node/name";
  SafetyDecision decision{};
  if (sysfs.exists(adapter_node)) decision.adapter_identity = sysfs.read_text(adapter_node);
  if (!sysfs.exists(base)) return decision;

  const std::string name_path = base + "/name";
  const std::string driver_path = base + "/driver";
  const std::string target_node_path = base + "/of_node";
  const std::string name = sysfs.exists(name_path) ? sysfs.read_text(name_path) : "";
  if (contains_case_insensitive(name, "ac200")) {
    decision.allowed = false;
    decision.message = "refusing access: selected adapter/address is AC200 (internal bus is unsafe)";
  } else if (sysfs.exists(driver_path)) {
    decision.allowed = false;
    decision.message = "refusing access: selected I2C address is bound to a kernel driver";
  } else if (sysfs.exists(target_node_path)) {
    decision.allowed = false;
    decision.message = "refusing access: selected I2C address belongs to a device-tree node";
  } else {
    decision.allowed = false;
    decision.message = "refusing access: selected I2C address already exists in sysfs";
  }
  return decision;
}
}  // namespace mfi::auth3
