#pragma once

namespace esphome {
namespace setup_priority {
inline constexpr float BUS = 1000.0f;
inline constexpr float DATA = 600.0f;
}  // namespace setup_priority
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0.0f; }
};
}  // namespace esphome
