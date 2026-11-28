#pragma once
#include "esphome/components/audio/pcm_utils.h"
#include <functional>
#include <span>
#include <vector>

namespace esphome::audio {
class AudioStreamInfo {
 public:
  AudioStreamInfo(uint8_t bits, uint8_t channels, uint32_t rate) : bits_(bits), channels_(channels), rate_(rate) {}
  uint8_t get_bits_per_sample() const { return this->bits_; }
  uint8_t get_channels() const { return this->channels_; }
  uint32_t get_sample_rate() const { return this->rate_; }
  uint32_t ms_to_samples(uint32_t ms) const { return static_cast<uint64_t>(ms) * this->rate_ * this->channels_ / 1000; }
  size_t ms_to_bytes(uint32_t ms) const { return this->ms_to_samples(ms) * 2; }
  size_t frames_to_bytes(uint32_t frames) const { return frames * this->channels_ * 2; }
  size_t samples_to_bytes(uint32_t samples) const { return samples * 2; }
  uint32_t bytes_to_samples(size_t bytes) const { return bytes / 2; }

 private:
  uint8_t bits_, channels_;
  uint32_t rate_;
};
}  // namespace esphome::audio

namespace esphome::microphone {
struct AudioCapture {
  std::span<const uint8_t> data;
  audio::AudioStreamInfo stream_info;
  int64_t capture_time_us{0};
};
class MicrophoneSource {
 public:
  explicit MicrophoneSource(uint8_t channels = 1, bool passive = false) : channels_(channels), passive_(passive) {}
  void add_data_callback(std::function<void(const AudioCapture &)> callback) { this->callback_ = callback; }
  audio::AudioStreamInfo get_audio_stream_info() const { return {16, this->channels_, 1000}; }
  void start() {
    if (!this->passive_)
      this->running_ = true;
  }
  void stop() {
    if (!this->passive_)
      this->running_ = false;
  }
  bool is_running() const { return this->running_; }
  bool is_passive() const { return this->passive_; }
  void set_running(bool running) { this->running_ = running; }
  void select(uint8_t channel) { this->selected_ = channel; }
  void emit(std::span<const uint8_t> data, uint8_t input_channels = 1) {
    if (!this->running_)
      return;
    const uint8_t map[] = {this->selected_, static_cast<uint8_t>(this->selected_ + 1)};
    const size_t frames = data.size() / (2 * input_channels);
    std::vector<uint8_t> selected(frames * this->channels_ * 2);
    audio::copy_pcm_frames_with_gain(data.data(), selected.data(), 2, input_channels, 2, map, this->channels_, 1,
                                     frames);
    this->callback_(AudioCapture{selected, this->get_audio_stream_info()});
  }
  void emit_raw(const AudioCapture &capture) { this->callback_(capture); }

 private:
  uint8_t channels_, selected_{0};
  bool passive_, running_{false};
  std::function<void(const AudioCapture &)> callback_;
};
}  // namespace esphome::microphone
