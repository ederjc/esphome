#pragma once

#include "bmm350_compensation.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

namespace esphome::bmm350 {

class BMM350Component : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_x_sensor(sensor::Sensor *sensor) { this->field_sensors_[0] = sensor; }
  void set_y_sensor(sensor::Sensor *sensor) { this->field_sensors_[1] = sensor; }
  void set_z_sensor(sensor::Sensor *sensor) { this->field_sensors_[2] = sensor; }
  void set_temperature_sensor(sensor::Sensor *sensor) { this->temperature_sensor_ = sensor; }

 protected:
  enum class SetupStep : uint8_t {
    SETUP_STEP_RESET,
    SETUP_STEP_ID,
    SETUP_STEP_OTP_START,
    SETUP_STEP_OTP_WAIT,
    SETUP_STEP_BIT_RESET_DONE,
    SETUP_STEP_FLUX_RESET_DONE,
    SETUP_STEP_NORMAL_DONE,
  };
  bool read_data_(uint8_t reg, uint8_t *data, size_t length);
  bool write_setup_register_(uint8_t reg, uint8_t value);
  bool check_command_(uint8_t command, bool require_normal = false);
  void log_pmu_registers_();
  void advance_(SetupStep step, uint32_t delay_ms);
  void setup_step_(SetupStep step);
  void fail_setup_(const char *reason);

  std::array<sensor::Sensor *, 3> field_sensors_{};
  sensor::Sensor *temperature_sensor_{nullptr};
  BMM350Compensation compensation_{};
  std::array<uint16_t, 32> otp_{};
  uint8_t otp_index_{0};
  uint8_t otp_attempts_{0};
  i2c::ErrorCode last_i2c_error_{i2c::ERROR_OK};
  bool ready_{false};
};

}  // namespace esphome::bmm350
