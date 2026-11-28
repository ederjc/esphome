#include "sound_level.h"

#if defined(USE_ESP32) || defined(USE_ZEPHYR)

#include "esphome/core/log.h"

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace esphome::sound_level {

ESPHOME_LOG_TAG(TAG, "sound_level");

#ifdef USE_ESP32
static const uint32_t MAX_FILL_DURATION_MS = 30;
static const uint32_t RING_BUFFER_DURATION_MS = 120;
#endif

// Square INT16_MIN since INT16_MIN^2 > INT16_MAX^2
static const double MAX_SAMPLE_SQUARED_DENOMINATOR = INT16_MIN * INT16_MIN;

void SoundLevelComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sound Level Component:\n"
                "  Measurement Duration: %" PRIu32 " ms",
                measurement_duration_ms_);
  LOG_SENSOR("  ", "Peak:", this->peak_sensor_);

  LOG_SENSOR("  ", "RMS:", this->rms_sensor_);
}

void SoundLevelComponent::setup() {
#ifdef USE_ZEPHYR
  const auto stream_info = this->microphone_source_->get_audio_stream_info();
  if (stream_info.get_bits_per_sample() != 16 || stream_info.get_channels() != 1 ||
      stream_info.ms_to_samples(this->measurement_duration_ms_) == 0) {
    ESP_LOGE(TAG, "Zephyr sound_level requires mono signed 16-bit PCM and a nonempty measurement window");
    this->mark_failed();
    return;
  }
  k_msgq_init(&this->measurements_, this->measurement_storage_.data(), sizeof(Measurement), MEASUREMENT_QUEUE_SIZE);
  this->reset_capture_(true);
  this->microphone_source_->add_data_callback(
      [this](const microphone::AudioCapture &capture) { this->consume_capture_(capture); });
#else
  this->microphone_source_->add_data_callback([this](const microphone::AudioCapture &capture) {
    std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer = this->ring_buffer_.lock();
    if (temp_ring_buffer != nullptr) {
      temp_ring_buffer->write((void *) capture.data.data(), capture.data.size());
    }
  });
#endif

  if (!this->microphone_source_->is_passive()) {
    // Automatically start the microphone if not in passive mode
    this->microphone_source_->start();
  }
}

void SoundLevelComponent::loop() {
#ifdef USE_ZEPHYR
  if (this->is_failed())
    return;
#endif
  if ((this->peak_sensor_ == nullptr) && (this->rms_sensor_ == nullptr)) {
    // No sensors configured, nothing to do
    return;
  }

#ifdef USE_ZEPHYR
  if (!this->microphone_source_->is_running()) {
    const auto key = k_spin_lock(&this->capture_lock_);
    const bool listening = this->listening_;
    k_spin_unlock(&this->capture_lock_, key);
    const uint32_t generation = this->reset_capture_(listening);
    if (!this->status_has_warning()) {
      this->status_set_warning(LOG_STR("Microphone isn't running, can't compute statistics"));
      PUBLISH_STATE(this->peak_sensor_, NAN);
      if (this->generation_current_(generation) && !this->microphone_source_->is_running())
        PUBLISH_STATE(this->rms_sensor_, NAN);
    }
    return;
  }
  for (size_t i = 0; i < MEASUREMENT_QUEUE_SIZE; ++i) {
    Measurement measurement;
    const auto key = k_spin_lock(&this->capture_lock_);
    const bool error = this->capture_error_;
    const bool report_error = error && !this->capture_error_reported_;
    this->capture_error_reported_ |= report_error;
    const uint32_t generation = this->capture_generation_;
    const int result = error ? -EIO : k_msgq_get(&this->measurements_, &measurement, K_NO_WAIT);
    k_spin_unlock(&this->capture_lock_, key);
    if (error) {
      if (report_error) {
        this->status_set_warning(LOG_STR("Invalid mono PCM or sound-level measurement queue overflow"));
        PUBLISH_STATE(this->peak_sensor_, NAN);
        if (this->generation_current_(generation))
          PUBLISH_STATE(this->rms_sensor_, NAN);
      }
      return;
    }
    this->status_clear_warning();
    if (result != 0)
      break;
    this->publish_measurement_(measurement, generation);
    if (!this->measurement_current_(generation))
      break;
  }
#else
  if (this->microphone_source_->is_running() && !this->status_has_error()) {
    // Allocate buffers
    if (this->start_()) {
      this->status_clear_warning();
    }
  } else {
    if (!this->status_has_warning()) {
      this->status_set_warning(LOG_STR("Microphone isn't running, can't compute statistics"));

      // Deallocate buffers, if necessary
      this->stop_();

      // Reset sensor outputs
      PUBLISH_STATE(this->peak_sensor_, NAN);
      PUBLISH_STATE(this->rms_sensor_, NAN);

      // Reset accumulators
      this->squared_peak_ = 0;
      this->squared_samples_sum_ = 0;
      this->sample_count_ = 0;
    }

    return;
  }

  if (this->status_has_error()) {
    return;
  }

  // Expose a chunk of the ring buffer's internal storage - don't block to avoid slowing the main loop.
  this->audio_source_->fill(0);

  if (this->audio_source_->available() == 0) {
    // No new audio available for processing
    return;
  }

  const uint32_t samples_in_window =
      this->microphone_source_->get_audio_stream_info().ms_to_samples(this->measurement_duration_ms_);
  const uint32_t samples_available_to_process =
      this->microphone_source_->get_audio_stream_info().bytes_to_samples(this->audio_source_->available());
  const uint32_t samples_to_process = std::min(samples_in_window - this->sample_count_, samples_available_to_process);

  // MicrophoneSource always provides int16 samples due to Python codegen settings
  const int16_t *audio_data = reinterpret_cast<const int16_t *>(this->audio_source_->data());

  // Process all the new audio samples
  for (uint32_t i = 0; i < samples_to_process; ++i) {
    // Squaring int16 samples won't overflow an int32
    int32_t squared_sample = static_cast<int32_t>(audio_data[i]) * static_cast<int32_t>(audio_data[i]);

    if (this->peak_sensor_ != nullptr) {
      this->squared_peak_ = std::max(this->squared_peak_, squared_sample);
    }

    if (this->rms_sensor_ != nullptr) {
      // Squared sum is an uint64 type - at max levels, an uint32 type would overflow after ~8 samples
      this->squared_samples_sum_ += squared_sample;
    }

    ++this->sample_count_;
  }

  // Remove the processed samples from ``audio_source_``
  this->audio_source_->consume(this->microphone_source_->get_audio_stream_info().samples_to_bytes(samples_to_process));

  if (this->sample_count_ == samples_in_window) {
    // Processed enough samples for the measurement window, compute and publish the sensor values
    if (this->peak_sensor_ != nullptr) {
      const float peak_db = 10.0f * log10f(static_cast<float>(this->squared_peak_) / MAX_SAMPLE_SQUARED_DENOMINATOR);
      this->peak_sensor_->publish_state(peak_db);

      this->squared_peak_ = 0;  // reset accumulator
    }

    if (this->rms_sensor_ != nullptr) {
      // Calculations are done with doubles instead of floats - floats lose precision for even modest window durations
      const double rms_db = 10.0 * log10((this->squared_samples_sum_ / MAX_SAMPLE_SQUARED_DENOMINATOR) /
                                         static_cast<double>(samples_in_window));
      this->rms_sensor_->publish_state(rms_db);

      this->squared_samples_sum_ = 0;  // reset accumulator
    }

    this->sample_count_ = 0;  // reset counter
  }
#endif
}

#ifdef USE_ZEPHYR
uint32_t SoundLevelComponent::reset_capture_(bool listening) {
  const auto key = k_spin_lock(&this->capture_lock_);
  this->listening_ = listening;
  const uint32_t generation = ++this->capture_generation_;
  this->capture_error_ = false;
  this->capture_error_reported_ = false;
  this->squared_peak_ = 0;
  this->squared_samples_sum_ = 0;
  this->sample_count_ = 0;
  k_msgq_purge(&this->measurements_);
  k_spin_unlock(&this->capture_lock_, key);
  return generation;
}

bool SoundLevelComponent::generation_current_(uint32_t generation) {
  const auto key = k_spin_lock(&this->capture_lock_);
  const bool current = this->capture_generation_ == generation;
  k_spin_unlock(&this->capture_lock_, key);
  return current;
}

void SoundLevelComponent::consume_capture_(const microphone::AudioCapture &capture) {
  const auto key = k_spin_lock(&this->capture_lock_);
  if (!this->listening_ || this->capture_error_ || (this->peak_sensor_ == nullptr && this->rms_sensor_ == nullptr)) {
    k_spin_unlock(&this->capture_lock_, key);
    return;
  }
  const auto expected = this->microphone_source_->get_audio_stream_info();
  if (capture.stream_info.get_bits_per_sample() != 16 || capture.stream_info.get_channels() != 1 ||
      capture.stream_info.get_sample_rate() != expected.get_sample_rate() || capture.data.size() % 2 != 0) {
    this->capture_error_ = true;
    k_msgq_purge(&this->measurements_);
    k_spin_unlock(&this->capture_lock_, key);
    return;
  }
  const uint32_t samples_in_window = expected.ms_to_samples(this->measurement_duration_ms_);
  for (size_t offset = 0; offset < capture.data.size(); offset += 2) {
    const uint16_t raw = capture.data[offset] | (static_cast<uint16_t>(capture.data[offset + 1]) << 8);
    const int32_t sample = raw < 0x8000 ? raw : static_cast<int32_t>(raw) - 0x10000;
    const int32_t squared = sample * sample;
    this->squared_peak_ = std::max(this->squared_peak_, squared);
    this->squared_samples_sum_ += squared;
    if (++this->sample_count_ == samples_in_window) {
      const Measurement measurement{this->squared_peak_, this->squared_samples_sum_, this->sample_count_};
      if (k_msgq_put(&this->measurements_, &measurement, K_NO_WAIT) != 0) {
        this->capture_error_ = true;
        k_msgq_purge(&this->measurements_);
        break;
      }
      this->squared_peak_ = 0;
      this->squared_samples_sum_ = 0;
      this->sample_count_ = 0;
    }
  }
  k_spin_unlock(&this->capture_lock_, key);
}

bool SoundLevelComponent::measurement_current_(uint32_t generation) {
  const auto key = k_spin_lock(&this->capture_lock_);
  const bool current = this->listening_ && !this->capture_error_ && this->capture_generation_ == generation;
  k_spin_unlock(&this->capture_lock_, key);
  return current && this->microphone_source_->is_running();
}

void SoundLevelComponent::publish_measurement_(const Measurement &measurement, uint32_t generation) {
  if (this->peak_sensor_ != nullptr && this->measurement_current_(generation)) {
    const float peak_db = 10.0f * log10f(static_cast<float>(measurement.squared_peak) / MAX_SAMPLE_SQUARED_DENOMINATOR);
    this->peak_sensor_->publish_state(peak_db);
  }
  if (this->rms_sensor_ != nullptr && this->measurement_current_(generation)) {
    const double rms_db = 10.0 * log10((measurement.squared_sum / MAX_SAMPLE_SQUARED_DENOMINATOR) /
                                       static_cast<double>(measurement.samples));
    this->rms_sensor_->publish_state(rms_db);
  }
}
#endif

void SoundLevelComponent::start() {
  if (this->microphone_source_->is_passive()) {
    ESP_LOGW(TAG, "Can't start the microphone in passive mode");
    return;
  }
#ifdef USE_ZEPHYR
  if (this->is_failed())
    return;
  this->reset_capture_(true);
#endif
  this->microphone_source_->start();
}

void SoundLevelComponent::stop() {
  if (this->microphone_source_->is_passive()) {
    ESP_LOGW(TAG, "Can't stop microphone in passive mode");
    return;
  }
#ifdef USE_ZEPHYR
  if (this->is_failed())
    return;
  this->reset_capture_(false);
#endif
  this->microphone_source_->stop();
}

#ifdef USE_ESP32
bool SoundLevelComponent::start_() {
  if (this->audio_source_ != nullptr) {
    return true;
  }

  const auto &stream_info = this->microphone_source_->get_audio_stream_info();
  const size_t bytes_per_frame = stream_info.frames_to_bytes(1);

  // Allocate a ring buffer for the microphone callback to write into. Round the size down to a multiple
  // of bytes_per_frame so the wrap boundary stays frame-aligned and avoids unnecessary single-frame splices.
  this->ring_buffer_.reset();  // Reset pointer to any previous ring buffer allocation
  const size_t ring_buffer_size =
      (stream_info.ms_to_bytes(RING_BUFFER_DURATION_MS) / bytes_per_frame) * bytes_per_frame;
  std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer = ring_buffer::RingBuffer::create(ring_buffer_size);
  if (temp_ring_buffer == nullptr) {
    this->status_momentary_error(15000);
    return false;
  }

  // Zero-copy source that reads directly from the ring buffer's internal storage. Frame-aligned reads
  // ensure multi-channel frames are never split across the ring buffer's wrap boundary.
  this->audio_source_ = audio::RingBufferAudioSource::create(
      temp_ring_buffer, stream_info.ms_to_bytes(MAX_FILL_DURATION_MS), static_cast<uint8_t>(bytes_per_frame));
  if (this->audio_source_ == nullptr) {
    this->status_momentary_error(15000);
    return false;
  }
  this->ring_buffer_ = temp_ring_buffer;

  this->status_clear_error();
  return true;
}

void SoundLevelComponent::stop_() { this->audio_source_.reset(); }
#endif

}  // namespace esphome::sound_level

#endif
