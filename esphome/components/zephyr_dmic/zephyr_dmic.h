#pragma once

#ifdef USE_ZEPHYR
#include "esphome/components/microphone/microphone.h"
#include "esphome/core/component.h"
#include <zephyr/audio/dmic.h>
#include <array>

namespace esphome::zephyr_dmic {

class ZephyrDMICMicrophone : public microphone::Microphone, public Component {
 public:
  ZephyrDMICMicrophone(const struct device *device, uint32_t sample_rate, uint8_t channels) : device_(device) {
    this->audio_stream_info_ = audio::AudioStreamInfo(16, channels, sample_rate);
  }
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override { this->stop(); }
  void start() override;
  void stop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  static constexpr size_t BLOCK_SIZE = 640;
  static constexpr size_t BLOCK_COUNT = 8;
  static constexpr uint32_t POLL_OVERHEAD_MS = 20;
  bool polling_budget_valid_() const;
  const struct device *device_;
  struct k_mem_slab slab_{};
  alignas(4) std::array<uint8_t, BLOCK_SIZE * BLOCK_COUNT> buffers_{};
  bool configured_{false};
};

}  // namespace esphome::zephyr_dmic
#endif
