#pragma once
#include <memory>
#include <vector>
#include <cstdint>
namespace esphome::ring_buffer {
class RingBuffer {
 public:
  static std::shared_ptr<RingBuffer> create(size_t) { return std::make_shared<RingBuffer>(); }
  void write(void *data, size_t size) {
    const auto *bytes = static_cast<uint8_t *>(data);
    this->bytes.insert(this->bytes.end(), bytes, bytes + size);
  }
  std::vector<uint8_t> bytes;
};
}  // namespace esphome::ring_buffer
