// Copyright 2022 Infineon Technologies AG
// SPDX-License-Identifier: Apache-2.0
// Adapted from sensor-xensiv-bgt60trxx, with checked transport, bounded reads and readback.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::infineon_bgt60tr13c {

enum class Result {
  RESULT_OK,
  RESULT_TRANSPORT_ERROR,
  RESULT_CHIP_ID_ERROR,
  RESULT_STATUS_ERROR,
  RESULT_READBACK_ERROR,
  RESULT_INCOMPLETE,
  RESULT_INVALID_ARGUMENT,
  RESULT_TIMEOUT,
};

inline constexpr uint8_t REG_MAIN = 0x00;
inline constexpr uint8_t REG_CHIP_ID = 0x02;
inline constexpr uint8_t REG_SFCTL = 0x06;
inline constexpr uint8_t REG_CCR2 = 0x2E;
inline constexpr uint8_t REG_FSTAT = 0x5F;
inline constexpr uint8_t REG_FIFO = 0x60;
inline constexpr uint32_t CHIP_ID = 0x000303;
inline constexpr uint32_t FSTAT_ERRORS = 0xCE0000;
inline constexpr uint32_t FSTAT_FILL = 0x003FFF;
inline constexpr size_t FIFO_WORDS = 8192;
inline constexpr size_t MAX_BURST_SAMPLES = 256;

inline constexpr uint32_t MAIN_FRAME_START = 0x000001;
inline constexpr uint32_t MAIN_RESET_SW = 0x000002;
inline constexpr uint32_t MAIN_RESET_FSM = 0x000004;
inline constexpr uint32_t MAIN_RESET_FIFO = 0x000008;
inline constexpr uint32_t SFCTL_FIFO_CREF = 0x001FFF;
inline constexpr uint32_t SFCTL_MISO_HS_READ = 0x010000;
// Datasheet CCR2: MAX_FRAME_CNT occupies bits 11:0; 0 means unbounded.
inline constexpr uint32_t CCR2_MAX_FRAME_CNT = 0x000FFF;

// A configurator register word is a ready-made SPI write command: address in bits 31:25,
// write flag in bit 24 and 24-bit register data below it.
constexpr uint8_t config_word_address(uint32_t word) { return uint8_t(word >> 25); }
constexpr uint32_t config_word_data(uint32_t word) { return word & 0xFFFFFF; }
constexpr bool config_word_valid(uint32_t word) {
  return (word & 0x01000000) != 0 && config_word_address(word) < REG_FIFO;
}

// Vendor xensiv_bgt60trxx_config() semantics: the FIFO limit and MISO high-speed bit in
// SFCTL are owned by the driver, not by the generated list.
constexpr uint32_t sfctl_with_limit(uint32_t sfctl, uint32_t limit_words) {
  return (sfctl & ~(SFCTL_FIFO_CREF | SFCTL_MISO_HS_READ)) | ((limit_words - 1) & SFCTL_FIFO_CREF);
}

constexpr std::array<uint8_t, 4> register_packet(uint8_t address, uint32_t data = 0, bool write = false) {
  return {uint8_t((address << 1) | (write ? 1 : 0)), uint8_t(data >> 16), uint8_t(data >> 8), uint8_t(data)};
}

constexpr uint16_t next_test_word(uint16_t word) {
  return (word >> 1) | (((word << 11) ^ (word << 10) ^ (word << 9) ^ (word << 3)) & 0x0800);
}

inline bool unpack_fifo(const uint8_t *bytes, size_t length, uint16_t *samples, size_t count) {
  if (bytes == nullptr || samples == nullptr || count == 0 || count % 2 != 0 || length != count / 2 * 3)
    return false;
  for (size_t i = 0, j = 0; i < length; i += 3, j += 2) {
    samples[j] = (uint16_t(bytes[i]) << 4) | (bytes[i + 1] >> 4);
    samples[j + 1] = (uint16_t(bytes[i + 1] & 0x0F) << 8) | bytes[i + 2];
  }
  return true;
}

class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool begin() = 0;
  virtual bool transfer(uint8_t *bytes, size_t length) = 0;
  virtual void end() = 0;
  virtual void delay_ms(uint32_t ms) = 0;
};

class Protocol {
 public:
  explicit Protocol(Transport &transport) : transport_(transport) {}

  Result initialize() {
    auto packet = register_packet(REG_SFCTL, 0, true);
    if (!this->transaction_(packet.data(), packet.size()))
      return Result::RESULT_TRANSPORT_ERROR;
    uint32_t value;
    auto result = this->read_register(REG_CHIP_ID, value);
    if (result != Result::RESULT_OK)
      return result;
    if (value != CHIP_ID)
      return Result::RESULT_CHIP_ID_ERROR;
    result = this->read_register(REG_SFCTL, value);
    if (result != Result::RESULT_OK)
      return result;
    return value == 0 ? Result::RESULT_OK : Result::RESULT_READBACK_ERROR;
  }

  Result read_register(uint8_t address, uint32_t &value) { return this->read_register_(address, value, false); }

  Result write_verified(uint8_t address, uint32_t value) {
    if (address >= 0x60 || value > 0xFFFFFF)
      return Result::RESULT_INVALID_ARGUMENT;
    auto packet = register_packet(address, value, true);
    if (!this->transaction_(packet.data(), packet.size()))
      return Result::RESULT_TRANSPORT_ERROR;
    uint32_t actual;
    auto result = this->read_register(address, actual);
    if (result != Result::RESULT_OK)
      return result;
    return actual == value ? Result::RESULT_OK : Result::RESULT_READBACK_ERROR;
  }

  Result write_register(uint8_t address, uint32_t value) {
    if (address >= REG_FIFO || value > 0xFFFFFF)
      return Result::RESULT_INVALID_ARGUMENT;
    auto packet = register_packet(address, value, true);
    if (!this->transaction_(packet.data(), packet.size()))
      return Result::RESULT_TRANSPORT_ERROR;
    this->gsr_ = packet[0];
    return (this->gsr_ & 0x0D) == 0 ? Result::RESULT_OK : Result::RESULT_STATUS_ERROR;
  }

  // Vendor xensiv_bgt60trxx_soft_reset(): set MAIN reset bits, wait for self-clear, then settle 10 ms.
  // Bounded here instead of the vendor's effectively unbounded poll.
  Result soft_reset(uint32_t mask) {
    if (mask == 0 || (mask & ~(MAIN_RESET_SW | MAIN_RESET_FSM | MAIN_RESET_FIFO)) != 0)
      return Result::RESULT_INVALID_ARGUMENT;
    uint32_t main;
    auto result = this->read_register_(REG_MAIN, main, true);
    if (result != Result::RESULT_OK)
      return result;
    auto packet = register_packet(REG_MAIN, (main & ~MAIN_FRAME_START) | mask, true);
    if (!this->transaction_(packet.data(), packet.size()))
      return Result::RESULT_TRANSPORT_ERROR;
    for (unsigned i = 0; i < 10; i++) {
      result = this->read_register_(REG_MAIN, main, true);
      if (result != Result::RESULT_OK)
        return result;
      if ((main & mask) == 0) {
        this->transport_.delay_ms(10);
        return Result::RESULT_OK;
      }
      this->transport_.delay_ms(1);
    }
    return Result::RESULT_TIMEOUT;
  }

  // Vendor xensiv_bgt60trxx_start_frame(true): read-modify-write MAIN.FRAME_START.
  Result start_frame() {
    uint32_t main;
    auto result = this->read_register(REG_MAIN, main);
    if (result != Result::RESULT_OK)
      return result;
    return this->write_register(REG_MAIN, main | MAIN_FRAME_START);
  }

  Result fifo_status(uint32_t &status) {
    auto result = this->read_register(REG_FSTAT, status);
    if (result != Result::RESULT_OK)
      return result;
    if ((status & FSTAT_ERRORS) != 0 || (status & 0x3FFF) > FIFO_WORDS)
      return Result::RESULT_STATUS_ERROR;
    return Result::RESULT_OK;
  }

  // Only complete, bounded bursts are delivered. A failed read may have consumed FIFO data;
  // the caller must discard its entire frame and reset FIFO/FSM before restarting.
  Result read_fifo(uint16_t *samples, size_t count) {
    if (samples == nullptr || count == 0 || count % 2 != 0 || count > MAX_BURST_SAMPLES)
      return Result::RESULT_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; i++)
      samples[i] = 0;
    uint32_t status;
    auto result = this->fifo_status(status);
    if (result != Result::RESULT_OK)
      return result;
    if ((status & 0x3FFF) < count / 2)
      return Result::RESULT_INCOMPLETE;

    // TR13C FIFO address 0x60, open-ended burst: FF | (0x60 << 17).
    std::array<uint8_t, 4> header{0xFF, 0xC0, 0x00, 0x00};
    bool success = this->transport_.begin();
    if (success)
      success = this->transport_.transfer(header.data(), header.size());
    this->gsr_ = header[0];
    if (success && (this->gsr_ & 0x0D) != 0) {
      this->transport_.end();
      return Result::RESULT_STATUS_ERROR;
    }
    if (success) {
      this->bytes_.fill(0);
      success = this->transport_.transfer(this->bytes_.data(), count / 2 * 3);
    }
    this->transport_.end();
    if (!success)
      return Result::RESULT_TRANSPORT_ERROR;
    result = this->fifo_status(status);
    if (result != Result::RESULT_OK)
      return result;
    unpack_fifo(this->bytes_.data(), count / 2 * 3, samples, count);
    return Result::RESULT_OK;
  }

  Result reset_fifo() {
    uint32_t main;
    // Latched FIFO faults must not prevent the control access that clears them.
    auto result = this->read_register_(REG_MAIN, main, true);
    if (result != Result::RESULT_OK)
      return result;
    auto packet = register_packet(REG_MAIN, main | 0x08, true);
    if (!this->transaction_(packet.data(), packet.size()))
      return Result::RESULT_TRANSPORT_ERROR;
    for (unsigned i = 0; i < 10; i++) {
      result = this->read_register_(REG_MAIN, main, true);
      if (result != Result::RESULT_OK)
        return result;
      if ((main & 0x08) == 0) {
        this->transport_.delay_ms(10);
        uint32_t status;
        result = this->fifo_status(status);
        if (result != Result::RESULT_OK)
          return result;
        return (status & 0x3FFF) == 0 ? Result::RESULT_OK : Result::RESULT_READBACK_ERROR;
      }
      this->transport_.delay_ms(1);
    }
    return Result::RESULT_TIMEOUT;
  }

  uint8_t gsr() const { return this->gsr_; }

 protected:
  Result read_register_(uint8_t address, uint32_t &value, bool allow_latched_error) {
    value = 0;
    if (address >= 0x60)
      return Result::RESULT_INVALID_ARGUMENT;
    auto packet = register_packet(address);
    if (!this->transaction_(packet.data(), packet.size()))
      return Result::RESULT_TRANSPORT_ERROR;
    this->gsr_ = packet[0];
    if (!allow_latched_error && (this->gsr_ & 0x0D) != 0)
      return Result::RESULT_STATUS_ERROR;
    value = (uint32_t(packet[1]) << 16) | (uint32_t(packet[2]) << 8) | packet[3];
    return Result::RESULT_OK;
  }

  bool transaction_(uint8_t *bytes, size_t length) {
    bool success = this->transport_.begin();
    if (success)
      success = this->transport_.transfer(bytes, length);
    this->transport_.end();
    return success;
  }

  Transport &transport_;
  std::array<uint8_t, MAX_BURST_SAMPLES / 2 * 3> bytes_{};
  uint8_t gsr_{0};
};

}  // namespace esphome::infineon_bgt60tr13c
