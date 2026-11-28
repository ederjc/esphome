#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/components/spi/spi.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#include "acquisition.h"

namespace esphome::infineon_bgt60tr13c {

class BGT60TR13C : public PollingComponent,
                   public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING,
                                         spi::DATA_RATE_1MHZ>,
                   protected Transport,
                   protected AcquisitionHost {
 public:
  BGT60TR13C() : acquisition_(*this, *this) {}
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_irq_pin(InternalGPIOPin *pin) { this->irq_pin_ = pin; }
  void set_registers(const uint32_t *registers, size_t count) {
    this->registers_ = registers;
    this->register_count_ = count;
  }
  void set_frame_geometry(uint16_t samples_per_chirp, uint16_t chirps_per_frame, uint8_t rx_antennas) {
    this->samples_per_chirp_ = samples_per_chirp;
    this->chirps_per_frame_ = chirps_per_frame;
    this->rx_antennas_ = rx_antennas;
  }
  void set_fifo_threshold(uint16_t samples) { this->fifo_threshold_ = samples; }
  void set_acquisition_mode(AcquisitionMode mode) { this->acquisition_.config().mode = mode; }
  void set_use_interrupt(bool use_interrupt) { this->acquisition_.config().use_interrupt = use_interrupt; }
  /// First frame after FRAME_START and, in continuous mode, each frame after the previous one.
  void set_frame_timeout(uint32_t timeout_ms) {
    this->acquisition_.config().first_frame_timeout_ms = timeout_ms;
    this->acquisition_.config().next_frame_timeout_ms = timeout_ms;
  }
  void set_frame_log_interval(uint32_t interval) { this->frame_log_interval_ = interval; }
#ifdef USE_SENSOR
  void set_frame_rate_sensor(sensor::Sensor *sensor) { this->frame_rate_sensor_ = sensor; }
  void set_signal_rms_sensor(sensor::Sensor *sensor) { this->signal_rms_sensor_ = sensor; }
  void set_motion_rms_sensor(sensor::Sensor *sensor) { this->motion_rms_sensor_ = sensor; }
#endif

 protected:
  bool begin() override;
  bool transfer(uint8_t *bytes, size_t length) override;
  void end() override { this->disable(); }
  void delay_ms(uint32_t ms) override { delay(ms); }
  uint32_t now_ms() override { return millis(); }
  bool irq_level() override { return this->irq_pin_->digital_read(); }
  void on_frame(uint32_t read_ms) override;
  void on_discard(const char *reason, Result result) override;
  void on_restart_failed(Result result, unsigned attempts) override;
  void on_failed(Result result) override { this->fail_(result); }
  void fail_(Result result);
  void diagnostic_update_();
  bool configure_();
  static void irq_handler_(BGT60TR13C *self);

  Acquisition acquisition_;
  GPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *irq_pin_{nullptr};
  const uint32_t *registers_{nullptr};
  size_t register_count_{0};
  uint32_t summaries_{0};
  uint32_t frames_since_update_{0};
  uint32_t last_update_ms_{0};
  uint32_t frame_log_interval_{1};
  uint16_t samples_per_chirp_{0};
  uint16_t chirps_per_frame_{0};
  uint16_t fifo_threshold_{0};
  uint8_t rx_antennas_{1};
  bool diagnostic_{true};
  bool readback_mismatch_{false};
#ifdef USE_SENSOR
  sensor::Sensor *frame_rate_sensor_{nullptr};
  sensor::Sensor *signal_rms_sensor_{nullptr};
  sensor::Sensor *motion_rms_sensor_{nullptr};
#endif
};

}  // namespace esphome::infineon_bgt60tr13c
