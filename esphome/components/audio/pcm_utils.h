#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace esphome::audio {

// Portable, little-endian, signed PCM conversion for whole-byte sample widths.
inline void copy_pcm_frames_with_gain(const uint8_t *source, uint8_t *destination, uint8_t source_bytes,
                                      uint8_t source_channels, uint8_t target_bytes, const uint8_t *channel_map,
                                      uint8_t target_channels, uint32_t gain, size_t frames) {
  const int64_t target_limit = int64_t{1} << (target_bytes * 8 - 1);
  for (size_t frame = 0; frame < frames; frame++) {
    for (uint8_t channel = 0; channel < target_channels; channel++) {
      const uint8_t *sample = source + (frame * source_channels + channel_map[channel]) * source_bytes;
      uint32_t raw = 0;
      for (uint8_t byte = 0; byte < source_bytes; byte++)
        raw |= static_cast<uint32_t>(sample[byte]) << (byte * 8);
      const int64_t source_limit = int64_t{1} << (source_bytes * 8 - 1);
      int64_t value = raw;
      if (value >= source_limit)
        value -= source_limit * 2;
      value *= gain;
      if (target_bytes > source_bytes) {
        value *= int64_t{1} << ((target_bytes - source_bytes) * 8);
      } else if (target_bytes < source_bytes) {
        const int64_t divisor = int64_t{1} << ((source_bytes - target_bytes) * 8);
        value = value < 0 ? -((-value + divisor / 2) / divisor) : (value + divisor / 2) / divisor;
      }
      value = std::clamp(value, -target_limit, target_limit - 1);
      const uint32_t output = static_cast<uint32_t>(value);
      for (uint8_t byte = 0; byte < target_bytes; byte++)
        *destination++ = static_cast<uint8_t>(output >> (byte * 8));
    }
  }
}

}  // namespace esphome::audio
