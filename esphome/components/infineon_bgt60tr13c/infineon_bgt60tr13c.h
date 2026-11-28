#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/components/spi/spi.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#include "frame_stats.h"
#include "protocol.h"

namespace esphome::infineon_bgt60tr13c {

enum class AcquisitionMode : uint8_t {
  /// One frame per update interval; MAX_FRAME_CNT is forced to 1 so slow SPI cannot overflow.
  TRIGGERED,
  /// Free-running frames from the register list; every frame must be drained in time.
  CONTINUOUS,
};

class BGT60TR13C : public PollingComponent,
                   public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING,
                                         spi::DATA_RATE_1MHZ>,
                   protected Transport {
 public:
  BGT60TR13C() : protocol_(*this) {}
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
  void set_acquisition_mode(AcquisitionMode mode) { this->mode_acq_ = mode; }
  void set_use_interrupt(bool use_interrupt) { this->use_interrupt_ = use_interrupt; }
  void set_frame_timeout(uint32_t timeout_ms) { this->frame_timeout_ms_ = timeout_ms; }
  void set_frame_log_interval(uint32_t interval) { this->frame_log_interval_ = interval; }
#ifdef USE_SENSOR
  void set_frame_rate_sensor(sensor::Sensor *sensor) { this->frame_rate_sensor_ = sensor; }
  void set_signal_rms_sensor(sensor::Sensor *sensor) { this->signal_rms_sensor_ = sensor; }
  void set_motion_rms_sensor(sensor::Sensor *sensor) { this->motion_rms_sensor_ = sensor; }
#endif

 protected:
  enum class State : uint8_t { DIAGNOSTIC, IDLE, WAITING, READING, FAILED };

  bool begin() override;
  bool transfer(uint8_t *bytes, size_t length) override;
  void end() override { this->disable(); }
  void delay_ms(uint32_t ms) override { delay(ms); }
  void fail_(Result result);
  void diagnostic_update_();
  bool configure_();
  void start_frame_();
  void poll_fifo_();
  void read_frame_();
  void finish_frame_();
  void discard_frame_(const char *reason, Result result);
  static void irq_handler_(BGT60TR13C *self);

  Protocol protocol_;
  FrameStats stats_;
  GPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *irq_pin_{nullptr};
  const uint32_t *registers_{nullptr};
  size_t register_count_{0};
  std::array<uint16_t, MAX_BURST_SAMPLES> burst_{};
  uint32_t summaries_{0};
  uint32_t frames_{0};
  uint32_t frames_since_update_{0};
  uint32_t discarded_{0};
  uint32_t irq_events_{0};
  uint32_t frame_started_ms_{0};
  uint32_t read_started_ms_{0};
  uint32_t last_poll_ms_{0};
  uint32_t last_update_ms_{0};
  uint32_t frame_timeout_ms_{500};
  uint32_t frame_log_interval_{1};
  size_t remaining_{0};
  uint32_t fill_words_{0};
  uint16_t samples_per_chirp_{0};
  uint16_t chirps_per_frame_{0};
  uint16_t fifo_threshold_{0};
  uint8_t rx_antennas_{1};
  const char *source_{"poll"};
  volatile bool irq_pending_{false};
  bool use_interrupt_{true};
  AcquisitionMode mode_acq_{AcquisitionMode::TRIGGERED};
  State state_{State::DIAGNOSTIC};
#ifdef USE_SENSOR
  sensor::Sensor *frame_rate_sensor_{nullptr};
  sensor::Sensor *signal_rms_sensor_{nullptr};
  sensor::Sensor *motion_rms_sensor_{nullptr};
#endif
};

}  // namespace esphome::infineon_bgt60tr13c
