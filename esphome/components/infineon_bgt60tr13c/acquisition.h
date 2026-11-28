#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "frame_stats.h"
#include "protocol.h"

namespace esphome::infineon_bgt60tr13c {

enum class AcquisitionMode : uint8_t {
  /// One frame per update interval; MAX_FRAME_CNT is forced to 1 so slow SPI cannot overflow.
  TRIGGERED,
  /// Free-running frames from the register list; every frame must be drained in time.
  CONTINUOUS,
};

// Watchdog cadence for reading FSTAT when no IRQ edge (or pin level) has signalled data.
inline constexpr uint32_t POLL_INTERVAL_MS = 50;
// Upper bound on FIFO bursts per loop() call so slow SPI clocks do not stall the main loop.
inline constexpr uint32_t READ_BUDGET_MS = 20;
// Consecutive runtime frame-restart failures tolerated before the component is marked failed.
inline constexpr unsigned MAX_RESTART_FAILURES = 5;

/// Platform services and event sinks for Acquisition; the ESPHome component implements it.
class AcquisitionHost {
 public:
  virtual ~AcquisitionHost() = default;
  virtual uint32_t now_ms() = 0;
  /// Level of the data-ready pin, only sampled when interrupts are disabled.
  virtual bool irq_level() = 0;
  virtual void on_frame(uint32_t read_ms) = 0;
  virtual void on_discard(const char *reason, Result result) = 0;
  /// A runtime restart failed; acquisition is idle and retries on the next update().
  virtual void on_restart_failed(Result result, unsigned attempts) = 0;
  /// MAX_RESTART_FAILURES consecutive restarts failed; acquisition stopped.
  virtual void on_failed(Result result) = 0;
};

struct AcquisitionConfig {
  AcquisitionMode mode{AcquisitionMode::TRIGGERED};
  bool use_interrupt{true};
  /// Deadline for the first frame, measured from FRAME_START.
  uint32_t first_frame_timeout_ms{500};
  /// Continuous mode: deadline for each later frame, measured from detection of the previous one.
  /// Must exceed the profile's frame period.
  uint32_t next_frame_timeout_ms{500};
};

/// Portable frame acquisition state machine: start, wait for a whole frame in the FIFO, read it in
/// bounded bursts, and discard/restart on any error. Register configuration stays with the caller.
class Acquisition {
 public:
  enum class State : uint8_t { IDLE, WAITING, READING, FAILED };

  Acquisition(Transport &transport, AcquisitionHost &host) : protocol_(transport), host_(host) {}

  Protocol &protocol() { return this->protocol_; }
  FrameStats &stats() { return this->stats_; }
  const FrameStats &stats() const { return this->stats_; }
  AcquisitionConfig &config() { return this->config_; }
  const AcquisitionConfig &config() const { return this->config_; }

  /// ISR-safe data-ready notification.
  void notify_irq() { this->irq_pending_ = true; }

  /// Resets FSM and FIFO and starts a frame; on failure the state becomes IDLE. The caller decides
  /// whether a failure is fatal (setup) or retried (runtime, see update()).
  Result start() {
    // Vendor stop sequence (FSM reset) plus FIFO reset, so every frame starts on an empty, aligned FIFO.
    auto result = this->protocol_.soft_reset(MAIN_RESET_FSM | MAIN_RESET_FIFO);
    if (result == Result::RESULT_OK) {
      this->irq_pending_ = false;
      result = this->protocol_.start_frame();
    }
    if (result != Result::RESULT_OK) {
      this->state_ = State::IDLE;
      return result;
    }
    this->restart_failures_ = 0;
    this->timeout_ms_ = this->config_.first_frame_timeout_ms;
    this->frame_started_ms_ = this->host_.now_ms();
    this->last_poll_ms_ = this->frame_started_ms_;
    this->state_ = State::WAITING;
    return Result::RESULT_OK;
  }

  void loop() {
    if (this->state_ == State::WAITING) {
      this->poll_fifo_();
    } else if (this->state_ == State::READING) {
      this->read_frame_();
    }
  }

  /// Called once per update interval: starts the next triggered frame.
  void update() {
    if (this->state_ == State::IDLE)
      this->restart_();
  }

  void set_idle() { this->state_ = State::IDLE; }
  void set_failed() { this->state_ = State::FAILED; }

  State state() const { return this->state_; }
  uint32_t frames() const { return this->frames_; }
  uint32_t discarded() const { return this->discarded_; }
  uint32_t irq_events() const { return this->irq_events_; }
  uint32_t fill_words() const { return this->fill_words_; }
  unsigned restart_failures() const { return this->restart_failures_; }
  const char *source() const { return this->source_; }

 protected:
  void restart_() {
    auto result = this->start();
    if (result != Result::RESULT_OK)
      this->restart_failed_(result);
  }

  // Runtime restart failures may be transient: stay idle and retry on the next update().
  void restart_failed_(Result result) {
    if (++this->restart_failures_ >= MAX_RESTART_FAILURES) {
      this->state_ = State::FAILED;
      this->host_.on_failed(result);
      return;
    }
    this->state_ = State::IDLE;
    this->host_.on_restart_failed(result, this->restart_failures_);
  }

  void poll_fifo_() {
    const uint32_t now = this->host_.now_ms();
    if (this->irq_pending_) {
      this->irq_pending_ = false;
      this->irq_events_++;
      this->source_ = "irq";
    } else if (!this->config_.use_interrupt && this->host_.irq_level()) {
      this->source_ = "pin";
    } else if (now - this->last_poll_ms_ >= POLL_INTERVAL_MS) {
      this->source_ = "poll";
    } else {
      return;
    }
    this->last_poll_ms_ = now;
    uint32_t fstat;
    auto result = this->protocol_.fifo_status(fstat);
    this->fill_words_ = fstat & FSTAT_FILL;
    if (result != Result::RESULT_OK) {
      this->discard_frame_("fifo_status", result);
      return;
    }
    if (this->fill_words_ >= this->stats_.frame_samples() / 2) {
      this->stats_.begin();
      this->remaining_ = this->stats_.frame_samples();
      this->read_started_ms_ = now;
      this->state_ = State::READING;
      this->read_frame_();
    } else if (now - this->frame_started_ms_ > this->timeout_ms_) {
      this->discard_frame_("timeout", Result::RESULT_TIMEOUT);
    }
  }

  void read_frame_() {
    const uint32_t started = this->host_.now_ms();
    while (this->remaining_ != 0) {
      const size_t count = std::min(this->remaining_, MAX_BURST_SAMPLES);
      auto result = this->protocol_.read_fifo(this->burst_.data(), count);
      if (result != Result::RESULT_OK) {
        this->discard_frame_("fifo_read", result);
        return;
      }
      this->stats_.add(this->burst_.data(), count);
      this->remaining_ -= count;
      if (this->remaining_ != 0 && this->host_.now_ms() - started >= READ_BUDGET_MS)
        return;
    }
    this->finish_frame_();
  }

  void finish_frame_() {
    if (!this->stats_.finish()) {
      this->discard_frame_("sample_count", Result::RESULT_INCOMPLETE);
      return;
    }
    this->frames_++;
    const uint32_t now = this->host_.now_ms();
    this->host_.on_frame(now - this->read_started_ms_);
    if (this->config_.mode == AcquisitionMode::TRIGGERED) {
      this->state_ = State::IDLE;
    } else {
      // A backlog keeps IRQ high without a new edge; check FSTAT again on the next loop.
      // The next frame completes one frame period after this one was detected, not after it was read.
      this->state_ = State::WAITING;
      this->timeout_ms_ = this->config_.next_frame_timeout_ms;
      this->frame_started_ms_ = this->read_started_ms_;
      this->last_poll_ms_ = now - POLL_INTERVAL_MS;
    }
  }

  void discard_frame_(const char *reason, Result result) {
    // A partially consumed FIFO cannot be realigned: drop the whole frame and restart from reset.
    this->discarded_++;
    this->remaining_ = 0;
    this->host_.on_discard(reason, result);
    if (this->config_.mode == AcquisitionMode::TRIGGERED) {
      auto reset = this->protocol_.soft_reset(MAIN_RESET_FSM | MAIN_RESET_FIFO);
      if (reset != Result::RESULT_OK) {
        this->restart_failed_(reset);
        return;
      }
      this->state_ = State::IDLE;
    } else {
      this->restart_();
    }
  }

  Protocol protocol_;
  AcquisitionHost &host_;
  FrameStats stats_;
  AcquisitionConfig config_;
  std::array<uint16_t, MAX_BURST_SAMPLES> burst_{};
  uint32_t frames_{0};
  uint32_t discarded_{0};
  uint32_t irq_events_{0};
  uint32_t frame_started_ms_{0};
  uint32_t timeout_ms_{0};
  uint32_t read_started_ms_{0};
  uint32_t last_poll_ms_{0};
  uint32_t fill_words_{0};
  size_t remaining_{0};
  unsigned restart_failures_{0};
  const char *source_{"poll"};
  volatile bool irq_pending_{false};
  State state_{State::IDLE};
};

}  // namespace esphome::infineon_bgt60tr13c
