#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::infineon_bgt60tr13c {

inline constexpr uint8_t MAX_RX_ANTENNAS = 3;

/// Streaming per-frame aggregates. Samples arrive in FIFO order: chirp-major, then sample,
/// with active RX channels interleaved per sample (RX order of the enabled channel mask).
class FrameStats {
 public:
  struct Antenna {
    float mean{NAN};
    float peak{NAN};
    uint16_t min{0};
    uint16_t max{0};
  };

  bool configure(uint16_t samples_per_chirp, uint16_t chirps, uint8_t rx) {
    if (samples_per_chirp == 0 || chirps == 0 || rx == 0 || rx > MAX_RX_ANTENNAS)
      return false;
    this->samples_per_chirp_ = samples_per_chirp;
    this->chirps_ = chirps;
    this->rx_ = rx;
    this->profile_.assign(size_t(samples_per_chirp) * rx, 0.0f);
    this->previous_.assign(this->profile_.size(), 0.0f);
    this->has_previous_ = false;
    this->begin();
    return true;
  }

  void begin() {
    this->position_ = 0;
    this->samples_ = 0;
    this->ac_power_ = 0.0;
    this->chirp_blocks_ = 0;
    for (auto &value : this->profile_)
      value = 0.0f;
    for (uint8_t a = 0; a < MAX_RX_ANTENNAS; a++) {
      this->sum_[a] = 0.0;
      this->chirp_sum_[a] = 0.0;
      this->chirp_sumsq_[a] = 0.0;
      this->min_[a] = UINT16_MAX;
      this->max_[a] = 0;
    }
  }

  void add(const uint16_t *samples, size_t count) {
    const size_t chirp_length = this->profile_.size();
    for (size_t i = 0; i < count; i++) {
      const uint16_t sample = samples[i];
      const uint8_t antenna = this->position_ % this->rx_;
      const double value = sample;
      this->sum_[antenna] += value;
      this->chirp_sum_[antenna] += value;
      this->chirp_sumsq_[antenna] += value * value;
      if (sample < this->min_[antenna])
        this->min_[antenna] = sample;
      if (sample > this->max_[antenna])
        this->max_[antenna] = sample;
      this->profile_[this->position_] += sample;
      this->samples_++;
      if (++this->position_ == chirp_length) {
        this->position_ = 0;
        this->close_chirp_();
      }
    }
  }

  /// Returns false if the delivered sample count does not match the configured frame.
  bool finish() {
    if (this->samples_ != this->frame_samples() || this->position_ != 0)
      return false;
    const size_t per_antenna = size_t(this->samples_per_chirp_) * this->chirps_;
    for (uint8_t a = 0; a < this->rx_; a++) {
      auto &out = this->antennas_[a];
      out.mean = float(this->sum_[a] / double(per_antenna));
      out.min = this->min_[a];
      out.max = this->max_[a];
      out.peak = std::fmax(float(out.max) - out.mean, out.mean - float(out.min));
    }
    this->signal_rms_ = float(std::sqrt(this->ac_power_ / double(this->chirp_blocks_)));

    // Mean chirp per antenna with its DC removed, compared against the previous frame.
    for (auto &value : this->profile_)
      value /= float(this->chirps_);
    std::array<float, MAX_RX_ANTENNAS> dc{};
    for (size_t k = 0; k < this->profile_.size(); k++)
      dc[k % this->rx_] += this->profile_[k];
    for (uint8_t a = 0; a < this->rx_; a++)
      dc[a] /= float(this->samples_per_chirp_);
    double motion = 0.0;
    for (size_t k = 0; k < this->profile_.size(); k++) {
      const float current = this->profile_[k] - dc[k % this->rx_];
      const double delta = current - this->previous_[k];
      motion += delta * delta;
      this->previous_[k] = current;
    }
    this->motion_rms_ = this->has_previous_ ? float(std::sqrt(motion / double(this->profile_.size()))) : NAN;
    this->has_previous_ = true;
    return true;
  }

  size_t frame_samples() const { return size_t(this->samples_per_chirp_) * this->chirps_ * this->rx_; }
  uint8_t rx() const { return this->rx_; }
  const Antenna &antenna(uint8_t index) const { return this->antennas_[index]; }
  /// RMS of each chirp's AC component, averaged over chirps and antennas (12-bit LSB).
  float signal_rms() const { return this->signal_rms_; }
  /// RMS change of the DC-free mean chirp versus the previous frame (LSB); NAN for the first frame.
  float motion_rms() const { return this->motion_rms_; }

 protected:
  void close_chirp_() {
    const double n = this->samples_per_chirp_;
    for (uint8_t a = 0; a < this->rx_; a++) {
      const double mean = this->chirp_sum_[a] / n;
      const double variance = this->chirp_sumsq_[a] / n - mean * mean;
      this->ac_power_ += variance > 0.0 ? variance : 0.0;
      this->chirp_blocks_++;
      this->chirp_sum_[a] = 0.0;
      this->chirp_sumsq_[a] = 0.0;
    }
  }

  std::vector<float> profile_;
  std::vector<float> previous_;
  std::array<double, MAX_RX_ANTENNAS> sum_{};
  std::array<double, MAX_RX_ANTENNAS> chirp_sum_{};
  std::array<double, MAX_RX_ANTENNAS> chirp_sumsq_{};
  std::array<uint16_t, MAX_RX_ANTENNAS> min_{};
  std::array<uint16_t, MAX_RX_ANTENNAS> max_{};
  std::array<Antenna, MAX_RX_ANTENNAS> antennas_{};
  double ac_power_{0.0};
  size_t position_{0};
  size_t samples_{0};
  uint32_t chirp_blocks_{0};
  float signal_rms_{NAN};
  float motion_rms_{NAN};
  uint16_t samples_per_chirp_{0};
  uint16_t chirps_{0};
  uint8_t rx_{1};
  bool has_previous_{false};
};

}  // namespace esphome::infineon_bgt60tr13c
