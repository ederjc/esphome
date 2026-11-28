#include "esphome/components/infineon_bgt60tr13c/acquisition.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace esphome::infineon_bgt60tr13c;

// Register-level BGT60TR13C model: MAIN reset bits self-clear, FIFO reset empties the FIFO,
// FSTAT reports the fill level and bursts drain it. Scripted host behaviour, NOT board evidence.
class FakeChip : public Transport {
 public:
  bool begin() override {
    assert(!this->selected);
    this->selected = true;
    this->burst = false;
    return true;
  }
  bool transfer(uint8_t *bytes, size_t length) override {
    assert(this->selected);
    if (this->burst) {
      assert(length % 3 == 0 && length / 3 <= this->fill);
      this->now += this->ms_per_burst;
      if (this->fail_next_data) {
        this->fail_next_data = false;
        this->fill -= length / 3;  // The clocks still consumed FIFO words.
        return false;
      }
      for (size_t i = 0; i < length; i += 3) {
        bytes[i] = 0x80;
        bytes[i + 1] = 0x08;
        bytes[i + 2] = 0x00;
      }
      this->fill -= length / 3;
      return true;
    }
    assert(length == 4);
    if (bytes[0] == 0xFF) {
      this->burst = true;
      bytes[0] = this->gsr;
      return true;
    }
    const uint8_t address = bytes[0] >> 1;
    const bool write = bytes[0] & 1;
    const uint32_t data = (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
    if (address == REG_MAIN && this->fail_main_accesses != 0) {
      this->fail_main_accesses--;
      return false;
    }
    bytes[0] = this->gsr;
    if (write) {
      this->write_(address, data);
    } else {
      const uint32_t value = address == REG_FSTAT ? (this->fill | this->fstat_flags) : this->regs[address];
      bytes[1] = uint8_t(value >> 16);
      bytes[2] = uint8_t(value >> 8);
      bytes[3] = uint8_t(value);
    }
    return true;
  }
  void end() override {
    assert(this->selected);
    this->selected = false;
  }
  void delay_ms(uint32_t ms) override { this->now += ms; }

  uint32_t now{0};
  uint32_t ms_per_burst{0};
  uint32_t fill{0};
  uint32_t fstat_flags{0};
  unsigned starts{0};
  unsigned fifo_resets{0};
  unsigned fail_main_accesses{0};
  bool fail_next_data{false};
  uint8_t gsr{0x70};
  std::array<uint32_t, REG_FIFO> regs{};

 protected:
  void write_(uint8_t address, uint32_t data) {
    if (address != REG_MAIN) {
      this->regs[address] = data;
      return;
    }
    if (data & MAIN_RESET_FIFO) {
      this->fill = 0;
      this->fifo_resets++;
    }
    if (data & MAIN_FRAME_START)
      this->starts++;
    this->regs[REG_MAIN] = data & ~(MAIN_RESET_SW | MAIN_RESET_FSM | MAIN_RESET_FIFO | MAIN_FRAME_START);
  }

  bool selected{false};
  bool burst{false};
};

class TestHost : public AcquisitionHost {
 public:
  explicit TestHost(FakeChip &chip) : chip(chip) {}
  uint32_t now_ms() override { return this->chip.now; }
  bool irq_level() override { return this->level; }
  void on_frame(uint32_t read_ms) override { this->read_ms.push_back(read_ms); }
  void on_discard(const char *reason, Result result) override {
    this->discards.emplace_back(reason);
    this->discard_results.push_back(result);
  }
  void on_restart_failed(Result result, unsigned attempts) override {
    assert(result != Result::RESULT_OK);
    this->restart_attempts.push_back(attempts);
  }
  void on_failed(Result result) override {
    assert(result != Result::RESULT_OK);
    this->failures++;
  }

  FakeChip &chip;
  bool level{false};
  std::vector<uint32_t> read_ms;
  std::vector<std::string> discards;
  std::vector<Result> discard_results;
  std::vector<unsigned> restart_attempts;
  unsigned failures{0};
};

// 128 samples x 4 chirps x 1 RX = 512 samples = 256 FIFO words = two 256-sample bursts.
static constexpr uint32_t FRAME_WORDS = 256;

struct Fixture {
  explicit Fixture(AcquisitionMode mode, uint32_t first_timeout = 500, uint32_t next_timeout = 500)
      : host(chip), acquisition(chip, host) {
    assert(acquisition.stats().configure(128, 4, 1));
    auto &config = acquisition.config();
    config.mode = mode;
    config.first_frame_timeout_ms = first_timeout;
    config.next_frame_timeout_ms = next_timeout;
  }
  // Advance in loop-sized steps, running loop() after each step.
  void run_until(uint32_t until, uint32_t step = 10) {
    while (chip.now < until) {
      chip.now = std::min(chip.now + step, until);
      acquisition.loop();
    }
  }
  FakeChip chip;
  TestHost host;
  Acquisition acquisition;
};

static void test_triggered_happy_path() {
  Fixture f(AcquisitionMode::TRIGGERED);
  assert(f.acquisition.start() == Result::RESULT_OK);
  assert(f.chip.starts == 1 && f.chip.fifo_resets == 1);
  assert(f.acquisition.state() == Acquisition::State::WAITING);
  f.acquisition.loop();  // No IRQ and no watchdog poll due yet.
  assert(f.host.read_ms.empty());

  f.chip.now = 30;
  f.chip.fill = FRAME_WORDS;
  f.acquisition.notify_irq();
  f.acquisition.loop();
  assert(f.host.read_ms.size() == 1 && f.acquisition.frames() == 1);
  assert(f.acquisition.state() == Acquisition::State::IDLE);
  assert(std::string(f.acquisition.source()) == "irq" && f.acquisition.irq_events() == 1);
  assert(f.acquisition.fill_words() == FRAME_WORDS && f.chip.fill == 0);
  assert(f.acquisition.stats().antenna(0).mean == 2048.0f);
  // Triggered mode waits for update() to start the next frame.
  f.run_until(1000);
  assert(f.acquisition.state() == Acquisition::State::IDLE && f.host.discards.empty());
  f.acquisition.update();
  assert(f.chip.starts == 2 && f.acquisition.state() == Acquisition::State::WAITING);
}

static void test_read_budget_split() {
  Fixture f(AcquisitionMode::TRIGGERED);
  f.chip.ms_per_burst = READ_BUDGET_MS;
  assert(f.acquisition.start() == Result::RESULT_OK);
  f.chip.fill = FRAME_WORDS;
  f.acquisition.notify_irq();
  f.acquisition.loop();
  assert(f.acquisition.state() == Acquisition::State::READING && f.host.read_ms.empty());
  assert(f.chip.fill == FRAME_WORDS - MAX_BURST_SAMPLES / 2);
  f.acquisition.loop();
  assert(f.host.read_ms.size() == 1 && f.host.read_ms[0] == 2 * READ_BUDGET_MS);
  assert(f.acquisition.state() == Acquisition::State::IDLE && f.host.discards.empty());
}

static void test_irq_before_frame_complete() {
  Fixture f(AcquisitionMode::TRIGGERED);
  assert(f.acquisition.start() == Result::RESULT_OK);
  f.chip.now = 10;
  f.chip.fill = FRAME_WORDS / 2;  // FIFO threshold below one frame.
  f.acquisition.notify_irq();
  f.acquisition.loop();
  assert(f.acquisition.state() == Acquisition::State::WAITING && f.acquisition.irq_events() == 1);
  assert(f.host.read_ms.empty() && f.host.discards.empty() && f.chip.fill == FRAME_WORDS / 2);
  f.chip.fill = FRAME_WORDS;
  f.run_until(10 + POLL_INTERVAL_MS);  // The FSTAT watchdog picks up the rest without another edge.
  assert(f.host.read_ms.size() == 1 && std::string(f.acquisition.source()) == "poll");
  assert(f.host.discards.empty());
}

static void test_timeout_discards() {
  Fixture triggered(AcquisitionMode::TRIGGERED);
  assert(triggered.acquisition.start() == Result::RESULT_OK);
  triggered.run_until(500);
  assert(triggered.host.discards.empty());
  triggered.run_until(560);
  assert(triggered.host.discards == std::vector<std::string>{"timeout"});
  assert(triggered.host.discard_results[0] == Result::RESULT_TIMEOUT);
  assert(triggered.acquisition.discarded() == 1 && triggered.chip.fifo_resets == 2);
  assert(triggered.acquisition.state() == Acquisition::State::IDLE && triggered.chip.starts == 1);

  // Continuous: the first-frame timeout runs from FRAME_START and restarts immediately.
  Fixture continuous(AcquisitionMode::CONTINUOUS, 500, 1500);
  assert(continuous.acquisition.start() == Result::RESULT_OK);
  continuous.run_until(560);
  assert(continuous.host.discards == std::vector<std::string>{"timeout"});
  assert(continuous.chip.starts == 2 && continuous.acquisition.state() == Acquisition::State::WAITING);
}

static void test_fifo_read_error_discards_and_restarts() {
  Fixture f(AcquisitionMode::CONTINUOUS, 2000, 2000);
  f.chip.ms_per_burst = READ_BUDGET_MS;
  assert(f.acquisition.start() == Result::RESULT_OK);
  f.chip.fill = FRAME_WORDS;
  f.acquisition.notify_irq();
  f.acquisition.loop();
  assert(f.acquisition.state() == Acquisition::State::READING);
  f.chip.fail_next_data = true;
  f.acquisition.loop();
  assert(f.host.discards == std::vector<std::string>{"fifo_read"});
  assert(f.host.discard_results[0] == Result::RESULT_TRANSPORT_ERROR);
  // The partial frame is dropped: FIFO reset and a fresh FRAME_START, not a concatenation.
  assert(f.host.read_ms.empty() && f.chip.fill == 0 && f.chip.starts == 2);
  assert(f.acquisition.state() == Acquisition::State::WAITING);
  f.chip.fill = FRAME_WORDS;
  f.acquisition.notify_irq();
  f.acquisition.loop();
  f.acquisition.loop();
  assert(f.host.read_ms.size() == 1 && f.acquisition.discarded() == 1);
}

static void test_back_to_back_continuous_frames() {
  // The first frame arrives soon after FRAME_START; later frames follow the 1000 ms frame period,
  // which exceeds the first-frame timeout but not the per-frame tolerance.
  Fixture f(AcquisitionMode::CONTINUOUS, 500, 1500);
  assert(f.acquisition.start() == Result::RESULT_OK);
  for (uint32_t frame = 1; frame <= 3; frame++) {
    const uint32_t ready = 100 + (frame - 1) * 1000;
    f.run_until(ready - 10);
    f.chip.fill = FRAME_WORDS;
    f.acquisition.notify_irq();
    f.run_until(ready);
    assert(f.acquisition.frames() == frame);
  }
  // A backlog of two frames is drained without another IRQ edge.
  f.run_until(3090);
  f.chip.fill = 2 * FRAME_WORDS;
  f.acquisition.notify_irq();
  f.run_until(3110);
  assert(f.acquisition.frames() == 5 && f.chip.fill == 0);
  assert(f.host.discards.empty() && f.chip.starts == 1);
  assert(f.acquisition.state() == Acquisition::State::WAITING);
  // A missing frame is still caught by the per-frame tolerance.
  f.run_until(4600);
  assert(f.host.discards.empty());
  f.run_until(4700);
  assert(f.host.discards == std::vector<std::string>{"timeout"} && f.chip.starts == 2);
}

static void test_restart_failure_escalation() {
  // A latched GSR0 error must not stop a restart (vendor parity).
  Fixture latched(AcquisitionMode::TRIGGERED);
  latched.chip.gsr = 0x71;
  assert(latched.acquisition.start() == Result::RESULT_OK && latched.chip.starts == 1);

  Fixture f(AcquisitionMode::TRIGGERED);
  assert(f.acquisition.start() == Result::RESULT_OK);
  f.chip.fill = FRAME_WORDS;
  f.acquisition.notify_irq();
  f.acquisition.loop();
  assert(f.acquisition.frames() == 1);

  // A transient failure stays idle and recovers on the next update.
  f.chip.fail_main_accesses = 1;
  f.acquisition.update();
  assert(f.host.restart_attempts == std::vector<unsigned>{1} && f.host.failures == 0);
  assert(f.acquisition.state() == Acquisition::State::IDLE);
  f.acquisition.update();
  assert(f.acquisition.state() == Acquisition::State::WAITING && f.acquisition.restart_failures() == 0);

  // A failed reset after a discard counts as a restart failure too.
  f.chip.fstat_flags = 0x400000;  // FIFO FULL.
  f.chip.now += POLL_INTERVAL_MS;
  f.chip.fail_main_accesses = 1;
  f.acquisition.loop();
  f.chip.fstat_flags = 0;
  assert(f.host.discards == std::vector<std::string>{"fifo_status"});
  assert(f.host.restart_attempts == (std::vector<unsigned>{1, 1}) && f.host.failures == 0);
  assert(f.acquisition.state() == Acquisition::State::IDLE);

  // Persistent failures escalate after MAX_RESTART_FAILURES consecutive attempts, counting the one above.
  f.chip.fail_main_accesses = 1000;
  for (unsigned i = 2; i < MAX_RESTART_FAILURES; i++) {
    f.acquisition.update();
    assert(f.acquisition.state() == Acquisition::State::IDLE && f.host.failures == 0);
  }
  assert(f.host.restart_attempts.back() == MAX_RESTART_FAILURES - 1);
  f.acquisition.update();
  assert(f.acquisition.state() == Acquisition::State::FAILED && f.host.failures == 1);
  const unsigned remaining = f.chip.fail_main_accesses;
  f.acquisition.update();
  f.acquisition.loop();
  assert(f.chip.fail_main_accesses == remaining && f.host.failures == 1);
}

int main() {
  test_triggered_happy_path();
  test_read_budget_split();
  test_irq_before_frame_complete();
  test_timeout_discards();
  test_fifo_read_error_discards_and_restarts();
  test_back_to_back_continuous_frames();
  test_restart_failure_escalation();
  puts("BGT60TR13C acquisition state machine tests passed (register-model fake chip; NOT board evidence)");
}
