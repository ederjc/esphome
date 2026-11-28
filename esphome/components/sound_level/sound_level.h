#pragma once

#if defined(USE_ESP32) || defined(USE_ZEPHYR)

#ifdef USE_ESP32
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#endif
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/sensor/sensor.h"

#include "esphome/core/component.h"

#ifdef USE_ZEPHYR
#include <zephyr/kernel.h>
#include <array>
#endif

namespace esphome::sound_level {

class SoundLevelComponent final : public Component {
  SUB_SENSOR(peak)
  SUB_SENSOR(rms)

 public:
  void dump_config() override;
  void setup() override;
  void loop() override;

  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_measurement_duration(uint32_t measurement_duration_ms) {
    this->measurement_duration_ms_ = measurement_duration_ms;
  }
  void set_microphone_source(microphone::MicrophoneSource *microphone_source) {
    this->microphone_source_ = microphone_source;
  }

  /// @brief Starts the MicrophoneSource to start measuring sound levels
  void start();

  /// @brief Stops the MicrophoneSource
  void stop();

 protected:
#ifdef USE_ESP32
  /// @brief Internal start command that, if necessary, allocates a ring buffer and a zero-copy
  /// ``RingBufferAudioSource`` that reads directly from it. ``ring_buffer_`` weakly references the
  /// ring buffer owned by ``audio_source_``. Returns true if allocations were successful.
  bool start_();

  /// @brief Internal stop command that deallocates ``audio_source_`` (which releases its ring buffer)
  void stop_();
#endif

  microphone::MicrophoneSource *microphone_source_{nullptr};

#ifdef USE_ESP32
  std::unique_ptr<audio::RingBufferAudioSource> audio_source_;
  std::weak_ptr<ring_buffer::RingBuffer> ring_buffer_;
#endif

#ifdef USE_ZEPHYR
  struct Measurement {
    int32_t squared_peak;
    uint64_t squared_sum;
    uint32_t samples;
  };
  static constexpr size_t MEASUREMENT_QUEUE_SIZE = 4;
  void consume_capture_(const microphone::AudioCapture &capture);
  uint32_t reset_capture_(bool listening);
  bool generation_current_(uint32_t generation);
  void publish_measurement_(const Measurement &measurement, uint32_t generation);
  bool measurement_current_(uint32_t generation);
  struct k_spinlock capture_lock_{};
  struct k_msgq measurements_{};
  std::array<char, MEASUREMENT_QUEUE_SIZE * sizeof(Measurement)> measurement_storage_{};
  bool listening_{false};
  bool capture_error_{false};
  bool capture_error_reported_{false};
  uint32_t capture_generation_{0};
#endif

  int32_t squared_peak_{0};
  uint64_t squared_samples_sum_{0};
  uint32_t sample_count_{0};

  uint32_t measurement_duration_ms_;
};

}  // namespace esphome::sound_level

#endif
