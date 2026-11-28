#include "esphome/components/infineon_bgt60tr13c/frame_stats.h"
#include "esphome/components/infineon_bgt60tr13c/protocol.h"
#include "vendor_oracle.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace esphome::infineon_bgt60tr13c;

struct Step {
  std::vector<uint8_t> tx;
  std::vector<uint8_t> rx;
  bool success{true};
};

class MockTransport : public Transport {
 public:
  bool begin() override {
    assert(!active);
    active = true;
    begins++;
    return begin_success;
  }
  bool transfer(uint8_t *bytes, size_t length) override {
    assert(active && index < steps.size());
    const auto &step = steps[index++];
    assert(step.tx.size() == length && std::equal(step.tx.begin(), step.tx.end(), bytes));
    assert(step.rx.size() == length);
    std::copy(step.rx.begin(), step.rx.end(), bytes);
    return step.success;
  }
  void end() override {
    assert(active);
    active = false;
    ends++;
  }
  void delay_ms(uint32_t ms) override { delays += ms; }
  void complete() const { assert(!active && index == steps.size() && begins == ends); }
  std::vector<Step> steps;
  size_t index{0};
  unsigned begins{0}, ends{0}, delays{0};
  bool active{false}, begin_success{true};
};

static Step status_step(uint32_t status) {
  return {{0xBE, 0, 0, 0}, {0x70, uint8_t(status >> 16), uint8_t(status >> 8), uint8_t(status)}};
}

static void test_packets_and_boot() {
  for (unsigned addr = 0; addr < 0x60; addr++) {
    for (const auto data : {0U, 0x123456U, 0xFFFFFFU}) {
      const auto packet = register_packet(addr, data, true);
      const uint32_t oracle = (addr << VENDOR_SPI_REGADR_POS) | VENDOR_SPI_WR_OP_MSK | data;
      const uint32_t actual =
          (uint32_t(packet[0]) << 24) | (uint32_t(packet[1]) << 16) | (uint32_t(packet[2]) << 8) | packet[3];
      assert(actual == oracle);
    }
  }
  const uint32_t burst = VENDOR_SPI_BURST_MODE_CMD | (VENDOR_FIFO_ADDRESS << VENDOR_SPI_BURST_MODE_SADR_POS);
  assert(burst == 0xFFC00000U);
  // CHIP_ID/GSR response is the A1 board observation from the hardware-verified SPI base,
  // not an invented second device ID. The transport here is a mock, not hardware evidence.
  MockTransport transport;
  transport.steps = {
      {{0x0D, 0, 0, 0}, {0x70, 0, 0, 0}}, {{0x04, 0, 0, 0}, {0x70, 0, 3, 3}}, {{0x0C, 0, 0, 0}, {0x70, 0, 0, 0}}};
  Protocol protocol(transport);
  assert(protocol.initialize() == Result::RESULT_OK);
  transport.complete();

  for (const auto wrong_id : {0U, 0x000606U}) {
    MockTransport bad;
    bad.steps = {{{0x0D, 0, 0, 0}, {0x70, 0, 0, 0}},
                 {{0x04, 0, 0, 0}, {0x70, 0, uint8_t(wrong_id >> 8), uint8_t(wrong_id)}}};
    Protocol rejected(bad);
    assert(rejected.initialize() == Result::RESULT_CHIP_ID_ERROR);
    bad.complete();
  }
}

static void test_packing_and_fifo() {
  std::vector<uint8_t> packed;
  std::array<uint16_t, MAX_BURST_SAMPLES> expected{}, actual{};
  uint16_t word = VENDOR_INITIAL_TEST_WORD;
  for (auto &sample : expected) {
    sample = word;
    assert(next_test_word(word) == vendor_next_test_word(word));
    word = vendor_next_test_word(word);
  }
  for (size_t i = 0; i < expected.size(); i += 2) {
    const uint32_t pair = (uint32_t(expected[i]) << 12) | expected[i + 1];
    packed.push_back(pair >> 16);
    packed.push_back(pair >> 8);
    packed.push_back(pair);
  }
  assert(unpack_fifo(packed.data(), packed.size(), actual.data(), actual.size()));
  assert(actual == expected);
  assert(!unpack_fifo(packed.data(), packed.size() - 1, actual.data(), actual.size()));
  assert(!unpack_fifo(packed.data(), packed.size(), actual.data(), actual.size() - 1));
  assert(!unpack_fifo(nullptr, packed.size(), actual.data(), actual.size()));
  MockTransport transport;
  transport.steps = {status_step(128),
                     {{0xFF, 0xC0, 0, 0}, {0x70, 0, 0, 0}},
                     {std::vector<uint8_t>(packed.size(), 0), packed},
                     status_step(0x100000)};
  Protocol protocol(transport);
  assert(protocol.read_fifo(actual.data(), actual.size()) == Result::RESULT_OK);
  assert(actual == expected);
  assert(transport.begins == 3);  // Header and data keep a single CS assertion.
  transport.complete();
}

static void test_fifo_failures() {
  std::array<uint16_t, 4> samples{1, 2, 3, 4};
  MockTransport incomplete;
  incomplete.steps = {status_step(1)};
  Protocol protocol(incomplete);
  assert(protocol.read_fifo(samples.data(), samples.size()) == Result::RESULT_INCOMPLETE);
  assert((samples == std::array<uint16_t, 4>{0, 0, 0, 0}));
  incomplete.complete();
  assert(protocol.read_fifo(samples.data(), 3) == Result::RESULT_INVALID_ARGUMENT);
  assert(protocol.read_fifo(samples.data(), MAX_BURST_SAMPLES + 2) == Result::RESULT_INVALID_ARGUMENT);
  assert(protocol.read_fifo(nullptr, 4) == Result::RESULT_INVALID_ARGUMENT);

  for (const uint32_t error : {0x020000U, 0x040000U, 0x080000U, 0x400000U, 0x800000U, 8193U}) {
    MockTransport overflow;
    overflow.steps = {status_step(error)};
    Protocol bad(overflow);
    assert(bad.read_fifo(samples.data(), samples.size()) == Result::RESULT_STATUS_ERROR);
    overflow.complete();
  }
  for (const uint8_t gsr : {0x71, 0x74, 0x78}) {
    MockTransport bad_status;
    bad_status.steps = {status_step(2), {{0xFF, 0xC0, 0, 0}, {gsr, 0, 0, 0}}};
    Protocol bad(bad_status);
    assert(bad.read_fifo(samples.data(), samples.size()) == Result::RESULT_STATUS_ERROR);
    bad_status.complete();
  }
  MockTransport failed;
  failed.steps = {
      status_step(2), {{0xFF, 0xC0, 0, 0}, {0x70, 0, 0, 0}}, {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, false}};
  Protocol failed_read(failed);
  assert(failed_read.read_fifo(samples.data(), samples.size()) == Result::RESULT_TRANSPORT_ERROR);
  assert((samples == std::array<uint16_t, 4>{0, 0, 0, 0}));
  failed.complete();

  MockTransport post_error;
  post_error.steps = {status_step(2),
                      {{0xFF, 0xC0, 0, 0}, {0x70, 0, 0, 0}},
                      {{0, 0, 0, 0, 0, 0}, {0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF}},
                      status_step(0x800000)};
  Protocol post_failed(post_error);
  assert(post_failed.read_fifo(samples.data(), samples.size()) == Result::RESULT_STATUS_ERROR);
  assert((samples == std::array<uint16_t, 4>{0, 0, 0, 0}));
  post_error.complete();

  MockTransport failed_status;
  failed_status.steps = {{{0xBE, 0, 0, 0}, {0, 0, 0, 0}, false}};
  Protocol lost_status(failed_status);
  assert(lost_status.read_fifo(samples.data(), samples.size()) == Result::RESULT_TRANSPORT_ERROR);
  failed_status.complete();

  MockTransport failed_header;
  failed_header.steps = {status_step(2), {{0xFF, 0xC0, 0, 0}, {0, 0, 0, 0}, false}};
  Protocol lost_header(failed_header);
  assert(lost_header.read_fifo(samples.data(), samples.size()) == Result::RESULT_TRANSPORT_ERROR);
  failed_header.complete();

  // Zero samples are legitimate when the checked transport and both status reads succeed.
  MockTransport zero_data;
  zero_data.steps = {status_step(2),
                     {{0xFF, 0xC0, 0, 0}, {0x70, 0, 0, 0}},
                     {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
                     status_step(0x100000)};
  Protocol zeros(zero_data);
  assert(zeros.read_fifo(samples.data(), samples.size()) == Result::RESULT_OK);
  assert((samples == std::array<uint16_t, 4>{0, 0, 0, 0}));
  zero_data.complete();
}

static void test_readback_and_reset() {
  MockTransport verified;
  verified.steps = {{{0x0D, 0, 0, 0}, {0x70, 0, 0, 0}}, {{0x0C, 0, 0, 0}, {0x70, 0, 0, 0}}};
  Protocol checked_write(verified);
  assert(checked_write.write_verified(REG_SFCTL, 0) == Result::RESULT_OK);
  verified.complete();

  MockTransport failed_write;
  failed_write.steps = {{{0x0D, 0, 0, 0}, {0, 0, 0, 0}, false}};
  Protocol lost_write(failed_write);
  assert(lost_write.write_verified(REG_SFCTL, 0) == Result::RESULT_TRANSPORT_ERROR);
  failed_write.complete();

  MockTransport failed_readback;
  failed_readback.steps = {{{0x0D, 0, 0, 0}, {0x70, 0, 0, 0}}, {{0x0C, 0, 0, 0}, {0, 0, 0, 0}, false}};
  Protocol lost_readback(failed_readback);
  assert(lost_readback.write_verified(REG_SFCTL, 0) == Result::RESULT_TRANSPORT_ERROR);
  failed_readback.complete();

  MockTransport mismatch;
  mismatch.steps = {{{0x0D, 0, 0, 0}, {0x70, 0, 0, 0}}, {{0x0C, 0, 0, 0}, {0x70, 0, 0, 1}}};
  Protocol protocol(mismatch);
  assert(protocol.write_verified(REG_SFCTL, 0) == Result::RESULT_READBACK_ERROR);
  mismatch.complete();

  MockTransport failed_begin;
  failed_begin.begin_success = false;
  Protocol absent(failed_begin);
  assert(absent.initialize() == Result::RESULT_TRANSPORT_ERROR);
  failed_begin.complete();

  MockTransport timeout;
  timeout.steps = {{{0, 0, 0, 0}, {0x70, 0, 0, 0}}, {{1, 0, 0, 8}, {0x70, 0, 0, 0}}};
  for (unsigned i = 0; i < 10; i++)
    timeout.steps.push_back({{0, 0, 0, 0}, {0x70, 0, 0, 8}});
  Protocol stuck(timeout);
  assert(stuck.reset_fifo() == Result::RESULT_TIMEOUT);
  assert(timeout.delays == 10);
  timeout.complete();

  MockTransport recover;
  std::array<uint16_t, 4> expected{}, samples{};
  auto word = VENDOR_INITIAL_TEST_WORD;
  for (auto &sample : expected) {
    sample = word;
    word = vendor_next_test_word(word);
  }
  std::vector<uint8_t> packed;
  for (size_t i = 0; i < expected.size(); i += 2) {
    const uint32_t pair = (uint32_t(expected[i]) << 12) | expected[i + 1];
    packed.insert(packed.end(), {uint8_t(pair >> 16), uint8_t(pair >> 8), uint8_t(pair)});
  }
  recover.steps = {status_step(0x800000),
                   {{0, 0, 0, 0}, {0x71, 0, 0, 0}},
                   {{1, 0, 0, 8}, {0x71, 0, 0, 0}},
                   {{0, 0, 0, 0}, {0x70, 0, 0, 0}},
                   status_step(0x100000),
                   status_step(2),
                   {{0xFF, 0xC0, 0, 0}, {0x70, 0, 0, 0}},
                   {std::vector<uint8_t>(packed.size(), 0), packed},
                   status_step(0x100000)};
  Protocol recovery(recover);
  assert(recovery.read_fifo(samples.data(), samples.size()) == Result::RESULT_STATUS_ERROR);
  assert(recovery.reset_fifo() == Result::RESULT_OK);
  assert(recovery.read_fifo(samples.data(), samples.size()) == Result::RESULT_OK);
  assert(samples == expected);
  assert(recover.delays == 10);
  recover.complete();
}

static void test_acquisition_primitives() {
  assert(config_word_address(0x5D012345U) == REG_CCR2);
  assert(config_word_data(0x5D012345U) == 0x012345U);
  assert(config_word_valid(0x0DA12FFFU));
  assert(!config_word_valid(0x0CA12FFFU));  // Missing write flag.
  assert(!config_word_valid(0xC1000000U));  // FIFO address is not configurable.
  // SFCTL keeps unrelated bits, drops MISO_HS_READ and programs CREF = words - 1.
  assert(sfctl_with_limit(0xA12FFFU, 1024) == 0xA023FFU);
  assert(sfctl_with_limit(0x010000U, 1) == 0U);

  MockTransport reset;
  reset.steps = {{{0, 0, 0, 0}, {0x71, 0, 0, 1}},  // Latched GSR errors must not block a reset.
                 {{1, 0, 0, 2}, {0x70, 0, 0, 0}},
                 {{0, 0, 0, 0}, {0x70, 0, 0, 2}},
                 {{0, 0, 0, 0}, {0x70, 0, 0, 0}}};
  Protocol resetter(reset);
  assert(resetter.soft_reset(MAIN_RESET_SW) == Result::RESULT_OK);
  assert(reset.delays == 11);
  reset.complete();

  MockTransport none;
  Protocol invalid(none);
  assert(invalid.soft_reset(0) == Result::RESULT_INVALID_ARGUMENT);
  assert(invalid.soft_reset(MAIN_FRAME_START) == Result::RESULT_INVALID_ARGUMENT);
  assert(invalid.write_register(REG_FIFO, 0) == Result::RESULT_INVALID_ARGUMENT);
  assert(invalid.write_register(REG_MAIN, 0x1000000) == Result::RESULT_INVALID_ARGUMENT);
  none.complete();

  MockTransport start;
  start.steps = {{{0, 0, 0, 0}, {0x70, 0x12, 0x34, 0x50}}, {{1, 0x12, 0x34, 0x51}, {0x70, 0, 0, 0}}};
  Protocol starter(start);
  assert(starter.start_frame() == Result::RESULT_OK);
  start.complete();

  MockTransport rejected;
  rejected.steps = {{{0x5D, 0, 0x0A, 0xBC}, {0x78, 0, 0, 0}}};
  Protocol writer(rejected);
  assert(writer.write_register(REG_CCR2, 0x000ABC) == Result::RESULT_STATUS_ERROR);
  rejected.complete();
}

static void test_frame_stats() {
  FrameStats stats;
  assert(!stats.configure(4, 2, 0));
  assert(!stats.configure(4, 2, MAX_RX_ANTENNAS + 1));
  assert(stats.configure(4, 2, 2));
  assert(stats.frame_samples() == 16);
  // Two chirps of four samples, RX1/RX2 interleaved per sample.
  const std::array<uint16_t, 16> frame{100, 10, 300, 10, 100, 10, 300, 10, 100, 10, 300, 10, 100, 10, 300, 10};
  stats.begin();
  stats.add(frame.data(), 5);  // Split delivery must equal one burst.
  stats.add(frame.data() + 5, frame.size() - 5);
  assert(stats.finish());
  assert(stats.antenna(0).mean == 200.0f && stats.antenna(0).min == 100 && stats.antenna(0).max == 300);
  assert(stats.antenna(0).peak == 100.0f);
  assert(stats.antenna(1).mean == 10.0f && stats.antenna(1).peak == 0.0f);
  // RX1 chirps have variance 100^2, RX2 has none: mean over 4 chirp blocks = 5000.
  assert(std::fabs(stats.signal_rms() - std::sqrt(5000.0f)) < 1e-3f);
  assert(std::isnan(stats.motion_rms()));

  stats.begin();
  stats.add(frame.data(), frame.size());
  assert(stats.finish());
  assert(stats.motion_rms() == 0.0f);

  // Inverting the RX1 pattern changes its DC-free profile by 200 at each of four RX1 points.
  std::array<uint16_t, 16> moved = frame;
  for (size_t i = 0; i < moved.size(); i += 2)
    moved[i] = moved[i] == 100 ? 300 : 100;
  stats.begin();
  stats.add(moved.data(), moved.size());
  assert(stats.finish());
  assert(std::fabs(stats.motion_rms() - std::sqrt(4 * 200.0f * 200.0f / 8)) < 1e-3f);

  stats.begin();
  stats.add(frame.data(), frame.size() - 2);
  assert(!stats.finish());
}

int main() {
  test_packets_and_boot();
  test_packing_and_fifo();
  test_fifo_failures();
  test_readback_and_reset();
  test_acquisition_primitives();
  test_frame_stats();
  puts("BGT60TR13C portable protocol tests passed (mock transport, vendor-derived oracle; NOT board evidence)");
}
