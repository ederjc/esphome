#include "infineon_bgt60tr13c.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace esphome::infineon_bgt60tr13c {

ESPHOME_LOG_TAG(TAG, "infineon_bgt60tr13c");

bool BGT60TR13C::begin() {
  this->enable();
  return this->spi_is_ready() && this->supports_transaction_status() && !this->transaction_failed();
}

bool BGT60TR13C::transfer(uint8_t *bytes, size_t length) {
  this->transfer_array(bytes, length);
  return !this->transaction_failed();
}

void BGT60TR13C::irq_handler_(BGT60TR13C *self) { self->acquisition_.notify_irq(); }

void BGT60TR13C::fail_(Result result) {
  ESP_LOGE(TAG, "Radar protocol failed: %u (transport=1 ID=2 status=3 readback=4 incomplete=5 argument=6 timeout=7)",
           unsigned(result));
  this->acquisition_.set_failed();
  this->status_set_error(LOG_STR("Radar protocol failure"));
  this->mark_failed();
}

void BGT60TR13C::setup() {
  this->spi_setup();
  if (!this->spi_is_ready() || !this->supports_transaction_status()) {
    ESP_LOGE(TAG, "SPI backend is unavailable or lacks checked transaction status");
    this->fail_(Result::RESULT_TRANSPORT_ERROR);
    return;
  }
  if (this->mode_ != spi::MODE0) {
    ESP_LOGE(TAG, "Only SPI mode 0 is supported");
    this->fail_(Result::RESULT_INVALID_ARGUMENT);
    return;
  }
  if (this->registers_ == nullptr && this->data_rate_ != spi::DATA_RATE_1MHZ) {
    ESP_LOGE(TAG, "Register diagnostics support only the bench-confirmed 1 MHz clock");
    this->fail_(Result::RESULT_INVALID_ARGUMENT);
    return;
  }
  this->disable();
  this->reset_pin_->setup();
  this->irq_pin_->setup();
  this->reset_pin_->digital_write(false);
  delay(10);
  this->reset_pin_->digital_write(true);
  delay(10);
  if (this->registers_ == nullptr) {
    ESP_LOGI(TAG, "BGT60TR13C diagnostic fixture boot v1");
  } else {
    ESP_LOGI(TAG, "BGT60TR13C acquisition boot v1");
  }
  auto result = this->acquisition_.protocol().initialize();
  if (result != Result::RESULT_OK) {
    this->fail_(result);
    return;
  }
  if (this->registers_ == nullptr) {
    ESP_LOGW(TAG, "Acquisition disabled: no register list configured; no presence/distance output");
    return;
  }
  if (!this->configure_())
    return;
  this->diagnostic_ = false;
  this->last_update_ms_ = millis();
  result = this->acquisition_.start();
  if (result != Result::RESULT_OK)
    this->fail_(result);
}

bool BGT60TR13C::configure_() {
  auto &protocol = this->acquisition_.protocol();
  if (!this->acquisition_.stats().configure(this->samples_per_chirp_, this->chirps_per_frame_, this->rx_antennas_)) {
    this->fail_(Result::RESULT_INVALID_ARGUMENT);
    return false;
  }
  const uint32_t limit_words = this->fifo_threshold_ / 2;
  const bool triggered = this->acquisition_.config().mode == AcquisitionMode::TRIGGERED;
  auto result = protocol.soft_reset(MAIN_RESET_SW);
  if (result != Result::RESULT_OK) {
    this->fail_(result);
    return false;
  }
  // Mirrors xensiv_bgt60trxx_config(): plain writes in list order, SFCTL limit bits owned by the driver.
  // GSR0 errors can stay latched after the SW reset, so writes and readbacks report but tolerate them.
  uint8_t gsr_seen = 0;
  for (size_t i = 0; i < this->register_count_; i++) {
    const uint32_t word = this->registers_[i];
    const uint8_t address = config_word_address(word);
    uint32_t data = config_word_data(word);
    if (!config_word_valid(word)) {
      ESP_LOGE(TAG, "Register word %u (0x%08" PRIX32 ") is not a write below the FIFO address", unsigned(i), word);
      this->fail_(Result::RESULT_INVALID_ARGUMENT);
      return false;
    }
    if (address == REG_SFCTL)
      data = sfctl_with_limit(data, limit_words);
    if (triggered && address == REG_CCR2)
      data = (data & ~CCR2_MAX_FRAME_CNT) | 1;
    result = protocol.write_register(address, data);
    gsr_seen |= protocol.gsr();
    if (result != Result::RESULT_OK) {
      ESP_LOGE(TAG, "Register 0x%02X write failed", address);
      this->fail_(result);
      return false;
    }
  }
  unsigned mismatches = 0;
  for (size_t i = 0; i < this->register_count_; i++) {
    const uint8_t address = config_word_address(this->registers_[i]);
    uint32_t expected = config_word_data(this->registers_[i]);
    if (address == REG_SFCTL)
      expected = sfctl_with_limit(expected, limit_words);
    if (triggered && address == REG_CCR2)
      expected = (expected & ~CCR2_MAX_FRAME_CNT) | 1;
    uint32_t actual;
    result = protocol.read_register_latched(address, actual);
    gsr_seen |= protocol.gsr();
    if (result != Result::RESULT_OK) {
      this->fail_(result);
      return false;
    }
    if (actual != expected) {
      mismatches++;
      ESP_LOGW(TAG, "Register 0x%02X readback 0x%06" PRIX32 " expected 0x%06" PRIX32, address, actual, expected);
    }
  }
  // Registers absent from the list still need the driver-owned settings.
  uint32_t sfctl, ccr2;
  result = protocol.read_register_latched(REG_SFCTL, sfctl);
  if (result == Result::RESULT_OK && sfctl != sfctl_with_limit(sfctl, limit_words))
    result = protocol.write_verified_latched(REG_SFCTL, sfctl_with_limit(sfctl, limit_words));
  if (result == Result::RESULT_OK && triggered) {
    result = protocol.read_register_latched(REG_CCR2, ccr2);
    if (result == Result::RESULT_OK && (ccr2 & CCR2_MAX_FRAME_CNT) != 1)
      result = protocol.write_verified_latched(REG_CCR2, (ccr2 & ~CCR2_MAX_FRAME_CNT) | 1);
  }
  if (result != Result::RESULT_OK) {
    this->fail_(result);
    return false;
  }
  if (this->acquisition_.config().use_interrupt)
    this->irq_pin_->attach_interrupt(&BGT60TR13C::irq_handler_, this, gpio::INTERRUPT_RISING_EDGE);
  ESP_LOGI(TAG, "Configuration GSR0 bits seen: 0x%02X", gsr_seen);
  ESP_LOGI(TAG,
           "Configured: registers=%u readback_mismatches=%u mode=%s samples_per_chirp=%u chirps=%u rx=%u "
           "frame_samples=%u frame_words=%u fifo_limit_words=%u irq=%s",
           unsigned(this->register_count_), mismatches, triggered ? "triggered" : "continuous",
           unsigned(this->samples_per_chirp_), unsigned(this->chirps_per_frame_), unsigned(this->rx_antennas_),
           unsigned(this->acquisition_.stats().frame_samples()),
           unsigned(this->acquisition_.stats().frame_samples() / 2), unsigned(limit_words),
           this->acquisition_.config().use_interrupt ? "interrupt" : "poll");
  if (mismatches != 0)
    this->status_set_warning(LOG_STR("Radar register readback mismatch"));
  return true;
}

void BGT60TR13C::loop() { this->acquisition_.loop(); }

void BGT60TR13C::on_frame(uint32_t read_ms) {
  const auto &stats = this->acquisition_.stats();
  const uint32_t frames = this->acquisition_.frames();
  this->frames_since_update_++;
  if (this->frame_log_interval_ != 0 && (frames - 1) % this->frame_log_interval_ == 0) {
    char antennas[192];
    size_t pos = 0;
    for (uint8_t a = 0; a < stats.rx() && pos < sizeof(antennas); a++) {
      const auto &rx = stats.antenna(a);
      int written =
          snprintf(antennas + pos, sizeof(antennas) - pos, " rx%u_mean=%.1f rx%u_min=%u rx%u_max=%u rx%u_peak=%.1f",
                   a + 1, rx.mean, a + 1, rx.min, a + 1, rx.max, a + 1, rx.peak);
      if (written < 0)
        break;
      pos += size_t(written);
    }
    antennas[sizeof(antennas) - 1] = '\0';
    ESP_LOGI(TAG,
             "Frame #%" PRIu32 " src=%s samples=%u fill=%" PRIu32 " read_ms=%" PRIu32
             "%s signal_rms=%.2f motion_rms=%.2f",
             frames, this->acquisition_.source(), unsigned(stats.frame_samples()), this->acquisition_.fill_words(),
             read_ms, antennas, stats.signal_rms(), stats.motion_rms());
  }
  this->status_clear_warning();
}

void BGT60TR13C::on_discard(const char *reason, Result result) {
  ESP_LOGW(TAG, "Frame discarded: reason=%s result=%u fill=%" PRIu32 " discarded=%" PRIu32, reason, unsigned(result),
           this->acquisition_.fill_words(), this->acquisition_.discarded());
  this->status_set_warning(LOG_STR("Radar frame discarded"));
}

void BGT60TR13C::update() {
  if (this->diagnostic_) {
    this->diagnostic_update_();
    return;
  }
  if (this->acquisition_.state() == Acquisition::State::FAILED)
    return;
  const uint32_t now = millis();
  const uint32_t elapsed = now - this->last_update_ms_;
  const float rate = elapsed == 0 ? 0.0f : this->frames_since_update_ * 1000.0f / elapsed;
  ESP_LOGI(TAG, "Acquisition summary: frames=%" PRIu32 " rate_hz=%.2f discarded=%" PRIu32 " irq_events=%" PRIu32,
           this->acquisition_.frames(), rate, this->acquisition_.discarded(), this->acquisition_.irq_events());
#ifdef USE_SENSOR
  if (this->frame_rate_sensor_ != nullptr)
    this->frame_rate_sensor_->publish_state(rate);
  if (this->frames_since_update_ != 0) {
    const auto &stats = this->acquisition_.stats();
    if (this->signal_rms_sensor_ != nullptr)
      this->signal_rms_sensor_->publish_state(stats.signal_rms());
    if (this->motion_rms_sensor_ != nullptr && !std::isnan(stats.motion_rms()))
      this->motion_rms_sensor_->publish_state(stats.motion_rms());
  }
#endif
  this->frames_since_update_ = 0;
  this->last_update_ms_ = now;
  this->acquisition_.update();
}

void BGT60TR13C::diagnostic_update_() {
  uint32_t chip_id, sfctl, fstat;
  auto &protocol = this->acquisition_.protocol();
  auto result = protocol.read_register(REG_CHIP_ID, chip_id);
  if (result == Result::RESULT_OK && chip_id != CHIP_ID)
    result = Result::RESULT_CHIP_ID_ERROR;
  if (result == Result::RESULT_OK)
    result = protocol.read_register(REG_SFCTL, sfctl);
  if (result == Result::RESULT_OK && sfctl != 0)
    result = Result::RESULT_READBACK_ERROR;
  if (result == Result::RESULT_OK)
    result = protocol.fifo_status(fstat);
  if (result != Result::RESULT_OK) {
    this->fail_(result);
    return;
  }
  ESP_LOGI(TAG, "Diagnostic #%u ID=0x%06X SFCTL=0x%06X GSR=0x%02X FSTAT=0x%06X words=%u IRQ=%u",
           unsigned(++this->summaries_), unsigned(chip_id), unsigned(sfctl), unsigned(protocol.gsr()), unsigned(fstat),
           unsigned(fstat & FSTAT_FILL), unsigned(this->irq_pin_->digital_read()));
}

void BGT60TR13C::dump_config() {
  if (this->registers_ == nullptr) {
    ESP_LOGCONFIG(TAG, "BGT60TR13C diagnostic-only (physical acquisition/presence/distance unverified)");
  } else {
    ESP_LOGCONFIG(
        TAG,
        "BGT60TR13C acquisition (user-supplied register list)\n"
        "  Registers: %u\n"
        "  Mode: %s\n"
        "  Frame: %u samples/chirp x %u chirps x %u RX\n"
        "  FIFO threshold: %u samples\n"
        "  Data ready: %s\n"
        "  Frame timeout: %" PRIu32 " ms",
        unsigned(this->register_count_),
        this->acquisition_.config().mode == AcquisitionMode::TRIGGERED ? "triggered" : "continuous",
        unsigned(this->samples_per_chirp_), unsigned(this->chirps_per_frame_), unsigned(this->rx_antennas_),
        unsigned(this->fifo_threshold_),
        this->acquisition_.config().use_interrupt ? "interrupt + FSTAT watchdog" : "IRQ pin level + FSTAT poll",
        this->acquisition_.config().frame_timeout_ms);
    if (this->data_rate_ != spi::DATA_RATE_1MHZ)
      ESP_LOGW(TAG, "  SPI clock differs from the bench-verified 1 MHz");
  }
  LOG_PIN("  Reset: ", this->reset_pin_);
  LOG_PIN("  IRQ: ", this->irq_pin_);
  LOG_UPDATE_INTERVAL(this);
#ifdef USE_SENSOR
  LOG_SENSOR("  ", "Frame rate", this->frame_rate_sensor_);
  LOG_SENSOR("  ", "Signal RMS", this->signal_rms_sensor_);
  LOG_SENSOR("  ", "Motion RMS", this->motion_rms_sensor_);
#endif
}

}  // namespace esphome::infineon_bgt60tr13c
