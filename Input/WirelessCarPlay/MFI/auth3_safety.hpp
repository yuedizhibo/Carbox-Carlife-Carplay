#pragma once

#include <string>

namespace mfi::auth3 {

class SysfsReader {
 public:
  virtual ~SysfsReader() = default;
  virtual bool exists(const std::string& path) const = 0;
  virtual std::string read_text(const std::string& path) const = 0;
};

struct SafetyDecision {
  bool allowed{true};
  std::string message;
  std::string adapter_identity;
};

// Reject a claimed/bound target without relying on a mutable I2C bus number.
SafetyDecision check_i2c_target_safety(const SysfsReader& sysfs, unsigned bus, unsigned address);

}  // namespace mfi::auth3
