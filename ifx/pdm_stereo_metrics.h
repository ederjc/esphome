#pragma once

#include "esphome/components/microphone/microphone.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <array>

class PDMChannelMetrics {
 public:
  void boot() { ESP_LOGI("pdm_bench", "BOOT stereo PDMv1 aggregate-only bench"); }

  void stopped() {
    if (this->run_ != 0) {
      if (!this->running_ || this->windows_ == 0 || !this->run_valid_) {
        this->bench_valid_ = false;
        ESP_LOGE("pdm_bench", "Run %u incomplete or invalid", static_cast<unsigned>(this->run_));
      } else {
        ++this->validated_runs_;
      }
    }
    this->running_ = false;
    this->stopped_at_ = esphome::millis();
    this->quiet_blocks_ = 0;
    ESP_LOGI("pdm_bench", "STOP run=%u full_windows=%u", static_cast<unsigned>(this->run_),
             static_cast<unsigned>(this->windows_));
    this->reset_window_();
  }

  bool starting() {
    if (this->running_ || this->completed_ || this->run_ >= 3 || !this->bench_valid_) {
      this->bench_valid_ = false;
      ESP_LOGE("pdm_bench", "Out-of-sequence START rejected");
      return false;
    }
    const uint32_t quiet_ms = esphome::millis() - this->stopped_at_;
    this->run_valid_ = quiet_ms >= 1000 && this->quiet_blocks_ == 0;
    if (!this->run_valid_) {
      this->bench_valid_ = false;
      ESP_LOGE("pdm_bench", "Invalid quiet gap: elapsed_ms=%u blocks=%u", static_cast<unsigned>(quiet_ms),
               static_cast<unsigned>(this->quiet_blocks_));
      return false;
    }
    ESP_LOGI("pdm_bench", "QUIET elapsed_ms=%u blocks=0", static_cast<unsigned>(quiet_ms));
    ++this->run_;
    this->windows_ = 0;
    this->running_ = true;
    this->first_block_ = true;
    this->reset_window_();
    ESP_LOGI("pdm_bench", "START run=%u", static_cast<unsigned>(this->run_));
    return true;
  }

  bool complete() {
    if (this->running_ || this->completed_ || !this->bench_valid_ || this->run_ != 3 ||
        this->validated_runs_ != 3 || this->quiet_blocks_ != 0 || esphome::millis() - this->stopped_at_ < 1000) {
      this->bench_valid_ = false;
      ESP_LOGE("pdm_bench", "INCOMPLETE runs=%u validated_runs=%u", static_cast<unsigned>(this->run_),
               static_cast<unsigned>(this->validated_runs_));
      return false;
    }
    this->completed_ = true;
    ESP_LOGI("pdm_bench", "COMPLETE runs=3 validated_runs=3 stopped_callbacks=0");
    return true;
  }

  void consume(const esphome::microphone::AudioCapture &capture) {
    const uint32_t now = esphome::millis();
    if (!this->running_) {
      ++this->quiet_blocks_;
      this->bench_valid_ = false;
      ESP_LOGE("pdm_bench", "PCM delivered during STOP");
      return;
    }
    if (capture.stream_info.get_channels() != 2 || capture.stream_info.get_bits_per_sample() != 16 ||
        capture.stream_info.get_sample_rate() != 15625 || capture.data.size() != 640) {
      this->run_valid_ = false;
      ESP_LOGE("pdm_bench", "Invalid stereo PCM shape");
      return;
    }
    if (this->first_block_) {
      this->first_block_ = false;
      ESP_LOGI("pdm_bench", "FIRST_BLOCK run=%u bytes=640 stereo_frames=160", static_cast<unsigned>(this->run_));
    } else if (now - this->last_block_ > 100) {
      this->run_valid_ = false;
      ESP_LOGE("pdm_bench", "Unexpected PCM timing gap");
    }
    this->last_block_ = now;
    if (this->blocks_ == 0)
      this->started_ = now;
    ++this->blocks_;
    this->frames_ += capture.data.size() / 4;
    for (size_t offset = 0; offset < capture.data.size(); offset += 4) {
      for (size_t channel = 0; channel < 2; ++channel) {
        const size_t sample = offset + channel * 2;
        this->nonzero_[channel] += (capture.data[sample] | capture.data[sample + 1]) != 0;
      }
    }
    const uint32_t elapsed = now - this->started_;
    if (elapsed >= 5000) {
      const uint32_t measured_hz = (this->frames_ - 160) * 1000 / elapsed;
      if (this->nonzero_[0] == 0 || this->nonzero_[1] == 0 || measured_hz < 14843 || measured_hz > 16407) {
        this->run_valid_ = false;
        ESP_LOGE("pdm_bench", "Stereo nonzero/rate gate failed");
      }
      ++this->windows_;
      ESP_LOGI("pdm_bench",
               "WINDOW run=%u window=%u blocks=%u frames=%u elapsed_ms=%u measured_hz=%u ch0_nonzero=%u ch1_nonzero=%u",
               static_cast<unsigned>(this->run_), static_cast<unsigned>(this->windows_),
               static_cast<unsigned>(this->blocks_), static_cast<unsigned>(this->frames_),
               static_cast<unsigned>(elapsed), static_cast<unsigned>(measured_hz),
               static_cast<unsigned>(this->nonzero_[0]), static_cast<unsigned>(this->nonzero_[1]));
      this->reset_window_();
    }
  }

 protected:
  void reset_window_() {
    this->blocks_ = 0;
    this->frames_ = 0;
    this->nonzero_.fill(0);
  }
  bool running_{false};
  bool completed_{false};
  bool bench_valid_{true};
  bool first_block_{true};
  bool run_valid_{true};
  uint32_t run_{0};
  uint32_t validated_runs_{0};
  uint32_t windows_{0};
  uint32_t stopped_at_{0};
  uint32_t quiet_blocks_{0};
  uint32_t started_{0};
  uint32_t last_block_{0};
  uint32_t blocks_{0};
  uint32_t frames_{0};
  std::array<uint32_t, 2> nonzero_{};
};

inline PDMChannelMetrics &pdm_bench_metrics() {
  static PDMChannelMetrics metrics;
  return metrics;
}
