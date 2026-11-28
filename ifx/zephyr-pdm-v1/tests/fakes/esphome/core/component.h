#pragma once
namespace esphome {
namespace setup_priority { constexpr float DATA = 600; }
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual void on_shutdown() {}
  virtual float get_setup_priority() const { return 0; }
  void mark_failed() { failed_ = true; }
  bool is_failed() const { return failed_; }
  void status_set_error() { error_ = true; }
  void status_clear_error() { error_ = false; }
 protected:
  bool failed_{false};
  bool error_{false};
};
}
