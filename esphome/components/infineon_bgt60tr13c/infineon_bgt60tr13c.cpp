#include "infineon_bgt60tr13c.h"
#include "esphome/core/log.h"

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

void BGT60TR13C::fail_(Result result) {
  ESP_LOGE(TAG, "Radar protocol failed: %u (transport=1 ID=2 status=3 readback=4 incomplete=5 argument=6 timeout=7)",
           unsigned(result));
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
  if (this->data_rate_ != spi::DATA_RATE_1MHZ || this->mode_ != spi::MODE0) {
    ESP_LOGE(TAG, "Only bench-confirmed mode 0 / 1 MHz is supported");
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
  ESP_LOGI(TAG, "BGT60TR13C diagnostic fixture boot v1");
  auto result = this->protocol_.initialize();
  if (result != Result::RESULT_OK) {
    this->fail_(result);
    return;
  }
  ESP_LOGW(TAG, "Acquisition disabled: no licensed, clock-confirmed FMCW profile; no presence/distance output");
}

void BGT60TR13C::update() {
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
           unsigned(fstat), unsigned(fstat & 0x3FFF), unsigned(this->irq_pin_->digital_read()));
}

void BGT60TR13C::dump_config() {
  ESP_LOGCONFIG(TAG, "BGT60TR13C diagnostic-only (physical acquisition/presence/distance unverified)");
  LOG_PIN("  Reset: ", this->reset_pin_);
  LOG_PIN("  IRQ: ", this->irq_pin_);
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace esphome::infineon_bgt60tr13c
