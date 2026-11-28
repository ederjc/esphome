#pragma once

#include "esphome/core/component.h"
#include "esphome/components/spi/spi.h"
#include "protocol.h"

namespace esphome::infineon_bgt60tr13c {

class BGT60TR13C : public PollingComponent,
                   public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING,
                                         spi::DATA_RATE_1MHZ>,
                   protected Transport {
 public:
  BGT60TR13C() : protocol_(*this) {}
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_irq_pin(GPIOPin *pin) { this->irq_pin_ = pin; }

 protected:
  bool begin() override;
  bool transfer(uint8_t *bytes, size_t length) override;
  void end() override { this->disable(); }
  void delay_ms(uint32_t ms) override { delay(ms); }
  void fail_(Result result);
  Protocol protocol_;
  GPIOPin *reset_pin_{nullptr};
  GPIOPin *irq_pin_{nullptr};
  uint32_t summaries_{0};
};

}  // namespace esphome::infineon_bgt60tr13c
