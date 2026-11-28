#include "infineon_bgt60tr13c.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace esphome::infineon_bgt60tr13c {

ESPHOME_LOG_TAG(TAG, "infineon_bgt60tr13c");

// Watchdog cadence for reading FSTAT when no IRQ edge (or pin level) has signalled data.
static constexpr uint32_t POLL_INTERVAL_MS = 50;
// Upper bound on FIFO bursts per loop() call so slow SPI clocks do not stall the main loop.
static constexpr uint32_t READ_BUDGET_MS = 20;

bool BGT60TR13C::begin() {
  this->enable();
  return this->spi_is_ready() && this->supports_transaction_status() && !this->transaction_failed();
}

bool BGT60TR13C::transfer(uint8_t *bytes, size_t length) {
  this->transfer_array(bytes, length);
  return !this->transaction_failed();
}

void BGT60TR13C::irq_handler_(BGT60TR13C *self) { self->irq_pending_ = true; }

void BGT60TR13C::fail_(Result result) {
  ESP_LOGE(TAG, "Radar protocol failed: %u (transport=1 ID=2 status=3 readback=4 incomplete=5 argument=6 timeout=7)",
           unsigned(result));
  this->state_ = State::FAILED;
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
  auto result = this->protocol_.initialize();
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
  this->last_update_ms_ = millis();
  this->start_frame_();
}

bool BGT60TR13C::configure_() {
  if (!this->stats_.configure(this->samples_per_chirp_, this->chirps_per_frame_, this->rx_antennas_)) {
    this->fail_(Result::RESULT_INVALID_ARGUMENT);
    return false;
  }
  const uint32_t limit_words = this->fifo_threshold_ / 2;
  const bool triggered = this->mode_acq_ == AcquisitionMode::TRIGGERED;
  auto result = this->protocol_.soft_reset(MAIN_RESET_SW);
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
    result = this->protocol_.write_register(address, data);
    gsr_seen |= this->protocol_.gsr();
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
    result = this->protocol_.read_register_latched(address, actual);
    gsr_seen |= this->protocol_.gsr();
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
  result = this->protocol_.read_register_latched(REG_SFCTL, sfctl);
  if (result == Result::RESULT_OK && sfctl != sfctl_with_limit(sfctl, limit_words))
    result = this->protocol_.write_verified_latched(REG_SFCTL, sfctl_with_limit(sfctl, limit_words));
  if (result == Result::RESULT_OK && triggered) {
    result = this->protocol_.read_register_latched(REG_CCR2, ccr2);
    if (result == Result::RESULT_OK && (ccr2 & CCR2_MAX_FRAME_CNT) != 1)
      result = this->protocol_.write_verified_latched(REG_CCR2, (ccr2 & ~CCR2_MAX_FRAME_CNT) | 1);
  }
  if (result != Result::RESULT_OK) {
    this->fail_(result);
    return false;
  }
  if (this->use_interrupt_)
    this->irq_pin_->attach_interrupt(&BGT60TR13C::irq_handler_, this, gpio::INTERRUPT_RISING_EDGE);
  ESP_LOGI(TAG, "Configuration GSR0 bits seen: 0x%02X", gsr_seen);
  ESP_LOGI(TAG,
           "Configured: registers=%u readback_mismatches=%u mode=%s samples_per_chirp=%u chirps=%u rx=%u "
           "frame_samples=%u frame_words=%u fifo_limit_words=%u irq=%s",
           unsigned(this->register_count_), mismatches, triggered ? "triggered" : "continuous",
           unsigned(this->samples_per_chirp_), unsigned(this->chirps_per_frame_), unsigned(this->rx_antennas_),
           unsigned(this->stats_.frame_samples()), unsigned(this->stats_.frame_samples() / 2), unsigned(limit_words),
           this->use_interrupt_ ? "interrupt" : "poll");
  if (mismatches != 0)
    this->status_set_warning(LOG_STR("Radar register readback mismatch"));
  this->state_ = State::IDLE;
  return true;
}

void BGT60TR13C::start_frame_() {
  // Vendor stop sequence (FSM reset) plus FIFO reset, so every frame starts on an empty, aligned FIFO.
  auto result = this->protocol_.soft_reset(MAIN_RESET_FSM | MAIN_RESET_FIFO);
  if (result == Result::RESULT_OK) {
    this->irq_pending_ = false;
    result = this->protocol_.start_frame();
  }
  if (result != Result::RESULT_OK) {
    this->fail_(result);
    return;
  }
  this->frame_started_ms_ = millis();
  this->last_poll_ms_ = this->frame_started_ms_;
  this->state_ = State::WAITING;
}

void BGT60TR13C::loop() {
  if (this->state_ == State::WAITING) {
    this->poll_fifo_();
  } else if (this->state_ == State::READING) {
    this->read_frame_();
  }
}

void BGT60TR13C::poll_fifo_() {
  const uint32_t now = millis();
  if (this->irq_pending_) {
    this->irq_pending_ = false;
    this->irq_events_++;
    this->source_ = "irq";
  } else if (!this->use_interrupt_ && this->irq_pin_->digital_read()) {
    this->source_ = "pin";
  } else if (now - this->last_poll_ms_ >= POLL_INTERVAL_MS) {
    this->source_ = "poll";
  } else {
    return;
  }
  this->last_poll_ms_ = now;
  uint32_t fstat;
  auto result = this->protocol_.fifo_status(fstat);
  if (result != Result::RESULT_OK) {
    this->fill_words_ = fstat & FSTAT_FILL;
    this->discard_frame_("fifo_status", result);
    return;
  }
  this->fill_words_ = fstat & FSTAT_FILL;
  if (this->fill_words_ >= this->stats_.frame_samples() / 2) {
    this->stats_.begin();
    this->remaining_ = this->stats_.frame_samples();
    this->read_started_ms_ = now;
    this->state_ = State::READING;
    this->read_frame_();
  } else if (now - this->frame_started_ms_ > this->frame_timeout_ms_) {
    this->discard_frame_("timeout", Result::RESULT_TIMEOUT);
  }
}

void BGT60TR13C::read_frame_() {
  const uint32_t started = millis();
  while (this->remaining_ != 0) {
    const size_t count = std::min(this->remaining_, MAX_BURST_SAMPLES);
    auto result = this->protocol_.read_fifo(this->burst_.data(), count);
    if (result != Result::RESULT_OK) {
      this->discard_frame_("fifo_read", result);
      return;
    }
    this->stats_.add(this->burst_.data(), count);
    this->remaining_ -= count;
    if (this->remaining_ != 0 && millis() - started >= READ_BUDGET_MS)
      return;
  }
  this->finish_frame_();
}

void BGT60TR13C::finish_frame_() {
  if (!this->stats_.finish()) {
    this->discard_frame_("sample_count", Result::RESULT_INCOMPLETE);
    return;
  }
  this->frames_++;
  this->frames_since_update_++;
  if (this->frame_log_interval_ != 0 && (this->frames_ - 1) % this->frame_log_interval_ == 0) {
    char antennas[192];
    size_t pos = 0;
    for (uint8_t a = 0; a < this->stats_.rx() && pos < sizeof(antennas); a++) {
      const auto &rx = this->stats_.antenna(a);
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
             this->frames_, this->source_, unsigned(this->stats_.frame_samples()), this->fill_words_,
             millis() - this->read_started_ms_, antennas, this->stats_.signal_rms(), this->stats_.motion_rms());
  }
  if (this->mode_acq_ == AcquisitionMode::TRIGGERED) {
    this->state_ = State::IDLE;
  } else {
    // A backlog keeps IRQ high without a new edge; check FSTAT again on the next loop.
    this->state_ = State::WAITING;
    this->frame_started_ms_ = millis();
    this->last_poll_ms_ = this->frame_started_ms_ - POLL_INTERVAL_MS;
  }
  this->status_clear_warning();
}

void BGT60TR13C::discard_frame_(const char *reason, Result result) {
  // A partially consumed FIFO cannot be realigned: drop the whole frame and restart from reset.
  this->discarded_++;
  this->remaining_ = 0;
  ESP_LOGW(TAG, "Frame discarded: reason=%s result=%u fill=%" PRIu32 " discarded=%" PRIu32, reason, unsigned(result),
           this->fill_words_, this->discarded_);
  this->status_set_warning(LOG_STR("Radar frame discarded"));
  if (this->mode_acq_ == AcquisitionMode::TRIGGERED) {
    auto reset = this->protocol_.soft_reset(MAIN_RESET_FSM | MAIN_RESET_FIFO);
    if (reset != Result::RESULT_OK) {
      this->fail_(reset);
      return;
    }
    this->state_ = State::IDLE;
  } else {
    this->start_frame_();
  }
}

void BGT60TR13C::update() {
  if (this->state_ == State::DIAGNOSTIC) {
    this->diagnostic_update_();
    return;
  }
  if (this->state_ == State::FAILED)
    return;
  const uint32_t now = millis();
  const uint32_t elapsed = now - this->last_update_ms_;
  const float rate = elapsed == 0 ? 0.0f : this->frames_since_update_ * 1000.0f / elapsed;
  ESP_LOGI(TAG, "Acquisition summary: frames=%" PRIu32 " rate_hz=%.2f discarded=%" PRIu32 " irq_events=%" PRIu32,
           this->frames_, rate, this->discarded_, this->irq_events_);
#ifdef USE_SENSOR
  if (this->frame_rate_sensor_ != nullptr)
    this->frame_rate_sensor_->publish_state(rate);
  if (this->frames_since_update_ != 0) {
    if (this->signal_rms_sensor_ != nullptr)
      this->signal_rms_sensor_->publish_state(this->stats_.signal_rms());
    if (this->motion_rms_sensor_ != nullptr && !std::isnan(this->stats_.motion_rms()))
      this->motion_rms_sensor_->publish_state(this->stats_.motion_rms());
  }
#endif
  this->frames_since_update_ = 0;
  this->last_update_ms_ = now;
  if (this->state_ == State::IDLE)
    this->start_frame_();
}

void BGT60TR13C::diagnostic_update_() {
  uint32_t chip_id, sfctl, fstat;
  auto result = this->protocol_.read_register(REG_CHIP_ID, chip_id);
  if (result == Result::RESULT_OK && chip_id != CHIP_ID)
    result = Result::RESULT_CHIP_ID_ERROR;
  if (result == Result::RESULT_OK)
    result = this->protocol_.read_register(REG_SFCTL, sfctl);
  if (result == Result::RESULT_OK && sfctl != 0)
    result = Result::RESULT_READBACK_ERROR;
  if (result == Result::RESULT_OK)
    result = this->protocol_.fifo_status(fstat);
  if (result != Result::RESULT_OK) {
    this->fail_(result);
    return;
  }
  ESP_LOGI(TAG, "Diagnostic #%u ID=0x%06X SFCTL=0x%06X GSR=0x%02X FSTAT=0x%06X words=%u IRQ=%u",
           unsigned(++this->summaries_), unsigned(chip_id), unsigned(sfctl), unsigned(this->protocol_.gsr()),
           unsigned(fstat), unsigned(fstat & FSTAT_FILL), unsigned(this->irq_pin_->digital_read()));
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
        unsigned(this->register_count_), this->mode_acq_ == AcquisitionMode::TRIGGERED ? "triggered" : "continuous",
        unsigned(this->samples_per_chirp_), unsigned(this->chirps_per_frame_), unsigned(this->rx_antennas_),
        unsigned(this->fifo_threshold_),
        this->use_interrupt_ ? "interrupt + FSTAT watchdog" : "IRQ pin level + FSTAT poll", this->frame_timeout_ms_);
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
