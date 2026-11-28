#include "bmm350.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::bmm350 {

static constexpr const char *const TAG = "bmm350";
static constexpr uint8_t REG_CHIP_ID = 0x00;
static constexpr uint8_t REG_AGGREGATION = 0x04;
static constexpr uint8_t REG_AXES = 0x05;
static constexpr uint8_t REG_COMMAND = 0x06;
static constexpr uint8_t REG_COMMAND_STATUS = 0x07;
static constexpr uint8_t REG_EFFECTIVE_CONFIGURATION = 0x08;
static constexpr uint8_t REG_INTERRUPT_CONTROL = 0x2E;
static constexpr uint8_t REG_INTERRUPT_STATUS = 0x30;
static constexpr uint8_t REG_DATA = 0x31;
static constexpr uint8_t REG_OTP_COMMAND = 0x50;
static constexpr uint8_t REG_OTP_DATA = 0x52;
static constexpr uint8_t REG_OTP_STATUS = 0x55;
static constexpr uint8_t REG_RESET = 0x7E;
static constexpr uint8_t COMMAND_NORMAL = 0x01;
static constexpr uint8_t MEASUREMENT_CONFIGURATION = 0x16;
static constexpr uint8_t COMMAND_FLUX_RESET = 0x05;
static constexpr uint8_t COMMAND_BIT_RESET = 0x07;

bool BMM350Component::read_data_(uint8_t reg, uint8_t *data, size_t length) {
  // Both I2C and I3C reads contain two dummy bytes before the register contents.
  std::array<uint8_t, 14> buffer{};
  if (length > buffer.size() - 2) {
    this->last_i2c_error_ = i2c::ERROR_INVALID_ARGUMENT;
    return false;
  }
  this->last_i2c_error_ = this->read_register(reg, buffer.data(), length + 2);
  if (this->last_i2c_error_ != i2c::ERROR_OK)
    return false;
  std::copy_n(buffer.data() + 2, length, data);
  return true;
}

bool BMM350Component::write_setup_register_(uint8_t reg, uint8_t value) {
  const auto error = this->write_register(reg, &value, 1);
  if (error == i2c::ERROR_OK)
    return true;
  ESP_LOGE(TAG, "I2C setup write failed: reg=0x%02X value=0x%02X error=%u", reg, value, error);
  if (reg == REG_COMMAND)
    this->log_pmu_registers_();
  return false;
}

void BMM350Component::setup() {
  this->ready_ = false;
  this->otp_index_ = 0;
  this->advance_(SetupStep::SETUP_STEP_RESET, 3);
}

void BMM350Component::advance_(SetupStep step, uint32_t delay_ms) {
  this->set_timeout("setup", delay_ms, [this, step]() { this->setup_step_(step); });
}

void BMM350Component::fail_setup_(const char *reason) {
  ESP_LOGE(TAG, "Setup failed: %s", reason);
  this->mark_failed();
}

void BMM350Component::log_pmu_registers_() {
  // One read-only snapshot on failure; never retry the command or extend its delay.
  uint8_t registers[5];
  if (!this->read_data_(REG_AGGREGATION, registers, sizeof(registers))) {
    ESP_LOGE(TAG, "I2C PMU diagnostic read failed: reg=0x04 length=5 error=%u", this->last_i2c_error_);
    return;
  }
  ESP_LOGE(TAG, "PMU snapshot: aggr[04]=0x%02X axes[05]=0x%02X cmd[06]=0x%02X status0[07]=0x%02X status1[08]=0x%02X",
           registers[0], registers[1], registers[2], registers[3], registers[4]);
}

bool BMM350Component::check_command_(uint8_t command, bool require_normal) {
  uint8_t status;
  if (!this->read_data_(REG_COMMAND_STATUS, &status, 1)) {
    ESP_LOGE(TAG, "I2C PMU status read failed: expected_cmd=0x%02X reg=0x07 error=%u", command, this->last_i2c_error_);
    return false;
  }
  if ((status & 0x11) == 0 && (status >> 5) == command && (!require_normal || (status & 0x08) != 0))
    return true;
  ESP_LOGE(TAG,
           "PMU status mismatch: expected_cmd=0x%02X status0=0x%02X completed_cmd=0x%02X busy=%u illegal=%u normal=%u "
           "odr_override=%u avg_override=%u",
           command, status, status >> 5, status & 1, (status >> 4) & 1, (status >> 3) & 1, (status >> 1) & 1,
           (status >> 2) & 1);
  this->log_pmu_registers_();
  return false;
}

void BMM350Component::setup_step_(SetupStep step) {
  uint8_t value;
  switch (step) {
    case SetupStep::SETUP_STEP_RESET:
      if (!this->write_setup_register_(REG_RESET, 0xB6)) {
        this->fail_setup_("soft reset");
        return;
      }
      this->advance_(SetupStep::SETUP_STEP_ID, 24);
      break;
    case SetupStep::SETUP_STEP_ID:
      if (!this->read_data_(REG_CHIP_ID, &value, 1) || value != 0x33) {
        this->fail_setup_("BMM350 chip ID (expected 0x33)");
        return;
      }
      this->advance_(SetupStep::SETUP_STEP_OTP_START, 0);
      break;
    case SetupStep::SETUP_STEP_OTP_START:
      if (!this->write_setup_register_(REG_OTP_COMMAND, 0x20 | this->otp_index_)) {
        this->fail_setup_("OTP read command");
        return;
      }
      this->otp_attempts_ = 0;
      this->advance_(SetupStep::SETUP_STEP_OTP_WAIT, 1);
      break;
    case SetupStep::SETUP_STEP_OTP_WAIT: {
      if (!this->read_data_(REG_OTP_STATUS, &value, 1) || (value & 0xE0) != 0) {
        this->fail_setup_("OTP status");
        return;
      }
      if ((value & 1) == 0) {
        if (++this->otp_attempts_ >= 20) {
          this->fail_setup_("OTP timeout");
          return;
        }
        this->advance_(SetupStep::SETUP_STEP_OTP_WAIT, 1);
        return;
      }
      uint8_t word[2];
      if (!this->read_data_(REG_OTP_DATA, word, 2)) {
        this->fail_setup_("OTP data");
        return;
      }
      this->otp_[this->otp_index_] = (uint16_t{word[0]} << 8) | word[1];
      if (++this->otp_index_ < this->otp_.size()) {
        this->advance_(SetupStep::SETUP_STEP_OTP_START, 0);
        return;
      }
      this->compensation_.load(this->otp_);
      if (!this->write_setup_register_(REG_OTP_COMMAND, 0x80) ||
          !this->write_setup_register_(REG_COMMAND, COMMAND_BIT_RESET)) {
        this->fail_setup_("OTP power off / bit reset");
        return;
      }
      this->advance_(SetupStep::SETUP_STEP_BIT_RESET_DONE, 14);
      break;
    }
    case SetupStep::SETUP_STEP_BIT_RESET_DONE:
      if (!this->check_command_(COMMAND_BIT_RESET) || !this->write_setup_register_(REG_COMMAND, COMMAND_FLUX_RESET)) {
        this->fail_setup_("bit reset status / flux reset");
        return;
      }
      this->advance_(SetupStep::SETUP_STEP_FLUX_RESET_DONE, 18);
      break;
    case SetupStep::SETUP_STEP_FLUX_RESET_DONE:
      // Latch data-ready for polling; pulsed mode clears it after only 1.25 ms.
      if (!this->check_command_(COMMAND_FLUX_RESET) ||
          !this->write_setup_register_(REG_AGGREGATION, MEASUREMENT_CONFIGURATION) ||
          !this->write_setup_register_(REG_AXES, 0x07) || !this->write_setup_register_(REG_INTERRUPT_CONTROL, 0x81)) {
        this->fail_setup_("flux reset status / measurement configuration");
        return;
      }
      if (!this->write_setup_register_(REG_COMMAND, COMMAND_NORMAL)) {
        this->fail_setup_("normal mode command write");
        return;
      }
      // Magnetic reset returns to suspend; NM latches the selected ODR/AVG (datasheet Figure 2).
      this->advance_(SetupStep::SETUP_STEP_NORMAL_DONE, 38);
      break;
    case SetupStep::SETUP_STEP_NORMAL_DONE:
      if (!this->check_command_(COMMAND_NORMAL, true)) {
        this->fail_setup_("normal mode status");
        return;
      }
      if (!this->read_data_(REG_EFFECTIVE_CONFIGURATION, &value, 1)) {
        ESP_LOGE(TAG, "I2C effective configuration read failed: reg=0x08 error=%u", this->last_i2c_error_);
        this->fail_setup_("effective measurement configuration read");
        return;
      }
      if ((value & 0x3F) != MEASUREMENT_CONFIGURATION) {
        ESP_LOGE(TAG, "Effective configuration mismatch: expected=0x%02X status1=0x%02X", MEASUREMENT_CONFIGURATION,
                 value);
        this->log_pmu_registers_();
        this->fail_setup_("effective measurement configuration");
        return;
      }
      this->ready_ = true;
      ESP_LOGI(TAG, "BMM350 ready (25 Hz, two-sample averaging)");
      break;
  }
}

void BMM350Component::update() {
  if (!this->ready_ || this->is_failed())
    return;
  uint8_t status;
  uint8_t raw[12];
  BMM350Data data;
  if (!this->read_data_(REG_INTERRUPT_STATUS, &status, 1)) {
    this->status_set_warning("Data-ready read failed");
    return;
  }
  if ((status & 0x04) == 0) {
    this->status_set_warning("Data not ready");
    return;
  }
  if (!this->read_data_(REG_DATA, raw, sizeof(raw)) || !this->compensation_.convert(raw, data)) {
    this->status_set_warning("Measurement read or compensation failed");
    return;
  }
  for (size_t i = 0; i < this->field_sensors_.size(); i++) {
    if (this->field_sensors_[i] != nullptr)
      this->field_sensors_[i]->publish_state(data.field[i]);
  }
  if (this->temperature_sensor_ != nullptr)
    this->temperature_sensor_->publish_state(data.temperature);
  this->status_clear_warning();
}

void BMM350Component::dump_config() {
  ESP_LOGCONFIG(TAG, "BMM350:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Field Strength X", this->field_sensors_[0]);
  LOG_SENSOR("  ", "Field Strength Y", this->field_sensors_[1]);
  LOG_SENSOR("  ", "Field Strength Z", this->field_sensors_[2]);
  LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
}

}  // namespace esphome::bmm350
