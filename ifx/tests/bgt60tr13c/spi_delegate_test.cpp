#include "esphome/components/spi/spi_zephyr.h"

#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
#include <vector>

using namespace esphome;
using namespace esphome::spi;

namespace {
struct Reply {
  size_t length;
  int status;
};
std::deque<Reply> replies;
size_t controller_calls = 0;

class Pin : public GPIOPin {
 public:
  void setup() override {}
  void pin_mode(gpio::Flags flags) override {}
  bool digital_read() override { return value; }
  void digital_write(bool state) override {
    value = state;
    writes.push_back(state);
  }
  size_t dump_summary(char *buffer, size_t len) const override { return 0; }
  gpio::Flags get_flags() const override { return gpio::FLAG_NONE; }
  bool value{true};
  std::vector<bool> writes;
};

void test_sticky_controller_error() {
  device dev;
  SPIComponent bus;
  bus.set_interface(&dev);
  Pin pin;
  ZephyrSPIDelegate delegate(&bus, 1000000, BIT_ORDER_MSB_FIRST, MODE0, &pin);
  assert(delegate.supports_transaction_status());
  replies = {{0, 0}, {2, -5}, {2, 0}};
  delegate.begin_transaction();
  assert(!pin.value && !delegate.transaction_failed());
  const uint8_t tx[2]{1, 2};
  uint8_t rx[2]{9, 9};
  delegate.transfer(tx, rx, sizeof(rx));
  assert(delegate.transaction_failed() && rx[0] == 0 && rx[1] == 0);
  delegate.transfer(tx, rx, sizeof(rx));
  assert(delegate.transaction_failed() && rx[0] == 0x5A);
  assert(!pin.value && pin.writes == std::vector<bool>({true, false}));
  delegate.end_transaction();
  assert(pin.value && delegate.transaction_failed());

  replies = {{2, 0}};
  delegate.begin_transaction();
  assert(!delegate.transaction_failed());
  delegate.transfer(tx, rx, sizeof(rx));
  assert(!delegate.transaction_failed() && rx[0] == 0x5A);
  delegate.end_transaction();
  assert(replies.empty());
}

void test_setup_error_and_retry() {
  device dev;
  SPIComponent bus;
  bus.set_interface(&dev);
  Pin pin;
  ZephyrSPIDelegate delegate(&bus, 1000000, BIT_ORDER_MSB_FIRST, MODE0, &pin);
  replies = {{0, -22}, {1, 0}};
  delegate.begin_transaction();
  assert(delegate.transaction_failed());
  delegate.transfer(1);
  assert(delegate.transaction_failed());
  delegate.end_transaction();
  replies = {{0, 0}, {1, 0}};
  delegate.begin_transaction();
  assert(!delegate.transaction_failed());
  delegate.transfer(1);
  assert(!delegate.transaction_failed());
  delegate.end_transaction();
  assert(replies.empty());
}

void test_unsupported_and_invalid_writes() {
  device dev;
  SPIComponent bus;
  bus.set_interface(&dev);
  Pin pin;
  ZephyrSPIDelegate delegate(&bus, 1000000, BIT_ORDER_MSB_FIRST, MODE0, &pin);
  SPIDelegate &api = delegate;
  replies = {{0, 0}};
  api.begin_transaction();
  const auto calls = controller_calls;
  api.write(0x12, 7);
  assert(api.transaction_failed() && controller_calls == calls);
  replies = {{1, 0}};
  const uint8_t data = 1;
  api.write_array(&data, 1);
  assert(api.transaction_failed());
  api.end_transaction();

  for (const auto invalid : std::array<std::array<size_t, 3>, 4>{{{7, 8, 1}, {40, 8, 1}, {8, 40, 1}, {8, 8, 2}}}) {
    api.begin_transaction();
    assert(!api.transaction_failed());
    const auto before = controller_calls;
    api.write_cmd_addr_data(invalid[0], 1, invalid[1], 1, &data, 1, invalid[2]);
    assert(api.transaction_failed() && controller_calls == before);
    api.end_transaction();
  }
  api.begin_transaction();
  const auto before = controller_calls;
  api.write_cmd_addr_data(8, 1, 8, 1, nullptr, 1, 1);
  assert(api.transaction_failed() && controller_calls == before);
  api.end_transaction();
  assert(replies.empty());
}

void test_command_and_data_errors() {
  device dev;
  SPIComponent bus;
  bus.set_interface(&dev);
  Pin pin;
  ZephyrSPIDelegate delegate(&bus, 1000000, BIT_ORDER_MSB_FIRST, MODE0, &pin);
  const uint8_t data = 1;
  replies = {{0, 0}, {2, -5}};
  delegate.begin_transaction();
  delegate.write_cmd_addr_data(8, 1, 8, 2, &data, 1, 1);
  assert(delegate.transaction_failed() && replies.empty());
  delegate.end_transaction();
  replies = {{2, 0}, {1, -5}, {1, 0}};
  delegate.begin_transaction();
  assert(!delegate.transaction_failed());
  delegate.write_cmd_addr_data(8, 1, 8, 2, &data, 1, 1);
  delegate.write_array(&data, 1);
  assert(delegate.transaction_failed() && replies.empty());
  delegate.end_transaction();
}
}  // namespace

int spi_transceive(const device *dev, const spi_config *config, const spi_buf_set *tx, const spi_buf_set *rx) {
  controller_calls++;
  const auto length = tx != nullptr ? tx->buffers[0].len : rx != nullptr ? rx->buffers[0].len : 0;
  // The real delegate sends a final zero-length controller-idle operation on destruction.
  if (replies.empty()) {
    assert(length == 0);
    return 0;
  }
  const auto reply = replies.front();
  replies.pop_front();
  assert(length == reply.length);
  if (reply.status == 0 && rx != nullptr && rx->buffers[0].buf != nullptr)
    std::memset(rx->buffers[0].buf, 0x5A, rx->buffers[0].len);
  return reply.status;
}

namespace esphome::spi {
// Core lifecycle functions are outside this standalone backend test.
void SPIComponent::setup() {}
void SPIComponent::dump_config() {}
bool SPIDelegate::is_ready() { return true; }
}  // namespace esphome::spi

int main() {
  test_sticky_controller_error();
  test_setup_error_and_retry();
  test_unsupported_and_invalid_writes();
  test_command_and_data_errors();
  std::cout << "Zephyr SPI delegate checked-transaction tests passed\n";
}
