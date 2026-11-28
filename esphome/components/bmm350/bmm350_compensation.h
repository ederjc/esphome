// Compensation equations and OTP layout derived from Bosch BMM350 SensorAPI v1.10.0.
// Copyright (c) 2025 Bosch Sensortec GmbH. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// See LICENSE.txt in this directory.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace esphome::bmm350 {

inline int32_t signed_value(uint32_t value, uint8_t bits) {
  const uint32_t sign = uint32_t{1} << (bits - 1);
  value &= (sign << 1) - 1;
  return (value & sign) ? static_cast<int32_t>(value) - static_cast<int32_t>(sign << 1) : static_cast<int32_t>(value);
}

struct BMM350Data {
  std::array<float, 3> field;
  float temperature;
};

struct BMM350Compensation {
  std::array<float, 3> offset{};
  std::array<float, 3> sensitivity{};
  std::array<float, 3> temperature_offset{};
  std::array<float, 3> temperature_sensitivity{};
  std::array<float, 4> cross_axis{};
  float temperature_zero{23.0f};
  float temperature_gain{0.0f};
  float temperature_bias{0.0f};

  void load(const std::array<uint16_t, 32> &otp) {
    this->offset[0] = signed_value(otp[14], 12);
    this->offset[1] = signed_value(((otp[14] & 0xF000) >> 4) | (otp[15] & 0xFF), 12);
    this->offset[2] = signed_value((otp[15] & 0x0F00) | (otp[16] & 0xFF), 12);
    this->sensitivity[0] = signed_value(otp[16] >> 8, 8) / 256.0f;
    this->sensitivity[1] = signed_value(otp[17], 8) / 256.0f;
    this->sensitivity[2] = signed_value(otp[17] >> 8, 8) / 256.0f;
    this->temperature_bias = signed_value(otp[13], 8) / 5.0f;
    this->temperature_gain = signed_value(otp[13] >> 8, 8) / 512.0f;
    for (size_t i = 0; i < 3; i++) {
      this->temperature_offset[i] = signed_value(otp[18 + i], 8) / 32.0f;
      this->temperature_sensitivity[i] = signed_value(otp[18 + i] >> 8, 8) / 16384.0f;
    }
    this->temperature_zero = signed_value(otp[24], 16) / 512.0f + 23.0f;
    this->cross_axis[0] = signed_value(otp[21], 8) / 800.0f;
    this->cross_axis[1] = signed_value(otp[21] >> 8, 8) / 800.0f;
    this->cross_axis[2] = signed_value(otp[22], 8) / 800.0f;
    this->cross_axis[3] = signed_value(otp[22] >> 8, 8) / 800.0f;
  }

  bool convert(const uint8_t *raw, BMM350Data &data) const {
    static constexpr float ADC_GAIN = 1.0f / 1.5f;
    static constexpr float LUT_GAIN = 0.714607238769531f;
    static constexpr float SCALE = 1000000.0f / 1048576.0f;
    static constexpr std::array<float, 4> FACTORS{
        SCALE / (14.55f * 19.46f * ADC_GAIN * LUT_GAIN),
        SCALE / (14.55f * 19.46f * ADC_GAIN * LUT_GAIN),
        SCALE / (9.0f * 31.0f * ADC_GAIN * LUT_GAIN),
        1.0f / (0.00204f * ADC_GAIN * LUT_GAIN * 1048576.0f),
    };
    std::array<float, 4> values{};
    for (size_t i = 0; i < values.size(); i++) {
      const uint32_t word = uint32_t{raw[3 * i]} | (uint32_t{raw[3 * i + 1]} << 8) | (uint32_t{raw[3 * i + 2]} << 16);
      values[i] = signed_value(word, 24) * FACTORS[i];
    }
    data.temperature = (values[3] - 25.49f) * (1.0f + this->temperature_gain) + this->temperature_bias;
    const float delta = data.temperature - this->temperature_zero;
    for (size_t i = 0; i < 3; i++) {
      const float denominator = 1.0f + this->temperature_sensitivity[i] * delta;
      if (denominator == 0.0f)
        return false;
      values[i] = (values[i] * (1.0f + this->sensitivity[i]) + this->offset[i] + this->temperature_offset[i] * delta) /
                  denominator;
    }
    const auto &c = this->cross_axis;
    const float determinant = 1.0f - c[0] * c[1];
    if (determinant == 0.0f)
      return false;
    data.field[0] = (values[0] - c[0] * values[1]) / determinant;
    data.field[1] = (values[1] - c[1] * values[0]) / determinant;
    data.field[2] = values[2] - c[2] * data.field[0] - c[3] * data.field[1];
    return std::isfinite(data.temperature) && std::isfinite(data.field[0]) && std::isfinite(data.field[1]) &&
           std::isfinite(data.field[2]);
  }
};

}  // namespace esphome::bmm350
