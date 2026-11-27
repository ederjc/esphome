// Copyright 2022 Infineon Technologies AG
// SPDX-License-Identifier: Apache-2.0
// Extracted from Infineon/sensor-xensiv-bgt60trxx at
// 78099697159a2a736504ef5bd1d33820b21f4e67, xensiv_bgt60trxx.c/.h.
// Only names/signature changed to keep this test oracle standalone.
// License: esphome/components/infineon_bgt60tr13c/LICENSE.txt
#pragma once
#include <cstdint>

inline uint16_t vendor_next_test_word(uint16_t cur_test_word) {
  uint16_t next_value =
      (cur_test_word >> 1) |
      (((cur_test_word << 11) ^ (cur_test_word << 10) ^ (cur_test_word << 9) ^ (cur_test_word << 3)) & 0x0800U);
  return next_value;
}

inline constexpr uint16_t VENDOR_INITIAL_TEST_WORD = 0x0001U;
inline constexpr uint32_t VENDOR_SPI_REGADR_POS = 25U;
inline constexpr uint32_t VENDOR_SPI_WR_OP_MSK = 0x01000000UL;
inline constexpr uint32_t VENDOR_SPI_BURST_MODE_CMD = 0xFF000000UL;
inline constexpr uint32_t VENDOR_SPI_BURST_MODE_SADR_POS = 17U;
inline constexpr uint32_t VENDOR_FIFO_ADDRESS = 0x60U;
