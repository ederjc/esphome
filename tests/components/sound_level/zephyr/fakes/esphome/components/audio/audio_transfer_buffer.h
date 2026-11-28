#pragma once
#include "esphome/components/ring_buffer/ring_buffer.h"
#include <algorithm>
namespace esphome::audio {
class RingBufferAudioSource {
 public:
  static std::unique_ptr<RingBufferAudioSource> create(std::shared_ptr<ring_buffer::RingBuffer> buffer, size_t maximum,
                                                       uint8_t) {
    return std::make_unique<RingBufferAudioSource>(buffer, maximum);
  }
  RingBufferAudioSource(std::shared_ptr<ring_buffer::RingBuffer> buffer, size_t maximum)
      : buffer_(buffer), maximum_(maximum) {}
  void fill(int) { this->available_ = std::min(this->maximum_, this->buffer_->bytes.size()); }
  size_t available() const { return this->available_; }
  const uint8_t *data() const { return this->buffer_->bytes.data(); }
  void consume(size_t size) {
    this->buffer_->bytes.erase(this->buffer_->bytes.begin(), this->buffer_->bytes.begin() + size);
    this->available_ -= size;
  }

 private:
  std::shared_ptr<ring_buffer::RingBuffer> buffer_;
  size_t maximum_, available_{0};
};
}  // namespace esphome::audio
