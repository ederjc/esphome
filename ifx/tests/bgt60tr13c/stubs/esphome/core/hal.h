#pragma once

#include <cstddef>

namespace esphome {
namespace gpio {
enum Flags { FLAG_NONE = 0 };
}
class GPIOPin {
 public:
  virtual ~GPIOPin() = default;
  virtual void setup() = 0;
  virtual void pin_mode(gpio::Flags flags) = 0;
  virtual bool digital_read() = 0;
  virtual void digital_write(bool value) = 0;
  virtual size_t dump_summary(char *buffer, size_t len) const = 0;
  virtual gpio::Flags get_flags() const = 0;
  virtual bool is_internal() { return false; }
};
class InternalGPIOPin : public GPIOPin {
 public:
  virtual int get_pin() const = 0;
  virtual bool is_inverted() const = 0;
};
}  // namespace esphome
