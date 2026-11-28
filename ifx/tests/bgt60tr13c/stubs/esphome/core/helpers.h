#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
inline uint16_t byteswap(uint16_t value) { return (value << 8) | (value >> 8); }
}  // namespace esphome
