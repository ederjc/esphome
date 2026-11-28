#pragma once
#include <cstdint>

namespace esphome {
namespace setup_priority {
constexpr float AFTER_CONNECTION = 100.0f;
}
class Component {
 public:
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0; }
  bool is_failed() const { return this->failed_; }
  void mark_failed() { this->failed_ = true; }
  bool status_has_warning() const { return this->warning_; }
  void status_set_warning(const char * = nullptr) { this->warning_ = true; }
  void status_clear_warning() { this->warning_ = false; }
  bool status_has_error() const { return this->error_; }
  void status_clear_error() { this->error_ = false; }
  void status_momentary_error(uint32_t) { this->error_ = true; }

 protected:
  bool failed_{false};
  bool warning_{false};
  bool error_{false};
};
}  // namespace esphome
