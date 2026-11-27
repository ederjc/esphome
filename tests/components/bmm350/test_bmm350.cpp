#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

#include "esphome/components/bmm350/bmm350.h"
#include "esphome/core/application.h"

namespace esphome::bmm350::testing {

class BMM350Bus : public i2c::I2CBus {
 public:
  i2c::ErrorCode write_readv(uint8_t address, const uint8_t *data, size_t length, uint8_t *out,
                             size_t out_length) override {
    EXPECT_EQ(address, 0x15);
    if (length == 0)
      return i2c::ERROR_INVALID_ARGUMENT;
    const uint8_t reg = data[0];
    if (out_length != 0)
      this->reads.push_back({reg, static_cast<uint8_t>(out_length)});
    if (reg == this->fail_register)
      return i2c::ERROR_NOT_ACKNOWLEDGED;
    if (out_length == 0) {
      EXPECT_EQ(length, 2u);
      this->writes.push_back({reg, data[1]});
      this->write_times.push_back(millis());
      if (reg == 0x50 && (data[1] & 0xE0) == 0x20)
        this->otp_index = data[1] & 0x1F;
      if (reg == 0x04)
        this->aggregation = data[1];
      if (reg == 0x05)
        this->axes = data[1];
      if (reg == 0x06) {
        this->command = data[1];
        // Reproduce the kit's rejected ODR update in suspend, not just echo written commands.
        this->illegal_command = this->command == 2 && !this->normal;
        if (this->command == 1) {
          this->normal = true;
          if (this->latch_configuration)
            this->effective_configuration = this->aggregation;
        }
      }
      return i2c::ERROR_OK;
    }
    EXPECT_EQ(length, 1u);
    EXPECT_GE(out_length, 3u);
    out[0] = 0xA5;
    out[1] = 0x5A;
    std::fill_n(out + 2, out_length - 2, 0);
    const uint8_t pmu_status = this->command_status_error || this->illegal_command
                                   ? 0x10
                                   : (this->status_override >= 0 ? this->status_override
                                                                 : (this->command << 5) | (this->command == 1 ? 8 : 0));
    if (reg == 0x00)
      out[2] = this->chip_id;
    else if (reg == 0x55)
      out[2] = this->otp_status;
    else if (reg == 0x52) {
      EXPECT_EQ(out_length, 4u);
      out[2] = this->otp[this->otp_index] >> 8;
      out[3] = this->otp[this->otp_index] & 0xFF;
    } else if (reg == 0x04) {
      EXPECT_EQ(out_length, 7u);
      out[2] = this->aggregation;
      out[3] = this->axes;
      out[4] = this->command;
      out[5] = pmu_status;
      out[6] = this->effective_configuration;
    } else if (reg == 0x07)
      out[2] = pmu_status;
    else if (reg == 0x08)
      out[2] = this->effective_configuration;
    else if (reg == 0x30)
      out[2] = this->data_ready ? 4 : 0;
    else if (reg == 0x31) {
      EXPECT_EQ(out_length, 14u);
      std::copy(this->raw.begin(), this->raw.end(), out + 2);
    }
    return i2c::ERROR_OK;
  }

  std::array<uint16_t, 32> otp{};
  std::array<uint8_t, 12> raw{};
  std::vector<std::array<uint8_t, 2>> writes;
  std::vector<std::array<uint8_t, 2>> reads;
  std::vector<uint32_t> write_times;
  uint8_t chip_id{0x33};
  uint8_t otp_status{1};
  uint8_t otp_index{0};
  uint8_t command{0};
  uint8_t aggregation{0x14};
  uint8_t axes{0x07};
  uint8_t effective_configuration{0x14};
  uint8_t fail_register{0xFF};
  int status_override{-1};
  bool command_status_error{false};
  bool normal{false};
  bool illegal_command{false};
  bool latch_configuration{true};
  bool data_ready{true};
};

class TestDevice : public BMM350Component {
 public:
  using BMM350Component::SetupStep;
  using BMM350Component::setup_step_;
  bool ready() const { return this->ready_; }
  void cancel_setup() { this->cancel_timeout("setup"); }
};

class BMM350Test : public ::testing::Test {
 protected:
  void SetUp() override {
    this->device_.set_i2c_bus(&this->bus_);
    this->device_.set_i2c_address(0x15);
    this->device_.set_x_sensor(&this->x_);
    this->device_.set_temperature_sensor(&this->temperature_);
  }
  void TearDown() override {
    this->device_.cancel_setup();
    App.scheduler.call(millis());
  }
  void step(TestDevice::SetupStep step) { this->device_.setup_step_(step); }
  void initialize() {
    using Step = TestDevice::SetupStep;
    this->step(Step::SETUP_STEP_RESET);
    this->step(Step::SETUP_STEP_ID);
    for (size_t i = 0; i < 32 && !this->device_.is_failed(); i++) {
      this->step(Step::SETUP_STEP_OTP_START);
      this->step(Step::SETUP_STEP_OTP_WAIT);
    }
    if (this->device_.is_failed())
      return;
    this->step(Step::SETUP_STEP_BIT_RESET_DONE);
    this->step(Step::SETUP_STEP_FLUX_RESET_DONE);
    this->step(Step::SETUP_STEP_NORMAL_DONE);
  }
  BMM350Bus bus_;
  TestDevice device_;
  sensor::Sensor x_;
  sensor::Sensor temperature_;
};

TEST_F(BMM350Test, InitializesOtpAndMagneticResetBeforeNormalMode) {
  this->initialize();
  ASSERT_TRUE(this->device_.ready());
  ASSERT_FALSE(this->device_.is_failed());
  std::vector<std::array<uint8_t, 2>> expected{{0x7E, 0xB6}};
  for (uint8_t i = 0; i < 32; i++)
    expected.push_back({0x50, static_cast<uint8_t>(0x20 | i)});
  for (const auto &write : std::vector<std::array<uint8_t, 2>>{
           {0x50, 0x80}, {0x06, 0x07}, {0x06, 0x05}, {0x04, 0x16}, {0x05, 0x07}, {0x2E, 0x81}, {0x06, 0x01}})
    expected.push_back(write);
  EXPECT_EQ(this->bus_.writes, expected);
  EXPECT_EQ(this->bus_.effective_configuration, 0x16);
}

TEST_F(BMM350Test, SchedulerHonorsResetAndModeTransitionWaits) {
  const uint32_t started = millis();
  this->device_.setup();
  EXPECT_TRUE(this->bus_.writes.empty());
  while (!this->device_.ready() && !this->device_.is_failed() && millis() - started < 1000) {
    delay(1);
    App.scheduler.call(millis());
  }
  ASSERT_TRUE(this->device_.ready());
  ASSERT_FALSE(this->device_.is_failed());
  ASSERT_EQ(this->bus_.write_times.size(), 40u);
  EXPECT_GE(this->bus_.write_times[0] - started, 3u);
  EXPECT_GE(this->bus_.write_times[1] - this->bus_.write_times[0], 24u);
  EXPECT_GE(this->bus_.write_times[35] - this->bus_.write_times[34], 14u);
  EXPECT_GE(this->bus_.write_times[36] - this->bus_.write_times[35], 18u);
  EXPECT_GE(millis() - this->bus_.write_times[39], 38u);
}

TEST_F(BMM350Test, RejectsBmm150ChipId) {
  this->bus_.chip_id = 0x32;
  this->step(TestDevice::SetupStep::SETUP_STEP_ID);
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
}

TEST_F(BMM350Test, StopsAtFirstOtpReadFailure) {
  this->bus_.fail_register = 0x52;
  this->initialize();
  EXPECT_TRUE(this->device_.is_failed());
  ASSERT_EQ(this->bus_.writes.size(), 2u);
  EXPECT_EQ(this->bus_.writes.back()[1], 0x20);
}

TEST_F(BMM350Test, BoundsOtpBusy) {
  this->bus_.otp_status = 0;
  this->step(TestDevice::SetupStep::SETUP_STEP_OTP_START);
  for (int i = 0; i < 19; i++)
    this->step(TestDevice::SetupStep::SETUP_STEP_OTP_WAIT);
  EXPECT_FALSE(this->device_.is_failed());
  this->step(TestDevice::SetupStep::SETUP_STEP_OTP_WAIT);
  EXPECT_TRUE(this->device_.is_failed());
}

TEST_F(BMM350Test, RejectsOtpHardwareError) {
  this->bus_.otp_status = 0x21;
  this->initialize();
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
}

TEST_F(BMM350Test, RejectsIllegalPmuCommand) {
  this->bus_.command_status_error = true;
  this->initialize();
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
}

TEST_F(BMM350Test, NormalBusyStopsSetupAndTakesOneReadOnlySnapshot) {
  this->bus_.command = 1;
  this->bus_.status_override = 0x29;
  this->step(TestDevice::SetupStep::SETUP_STEP_NORMAL_DONE);
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
  EXPECT_TRUE(this->bus_.writes.empty());
  const std::vector<std::array<uint8_t, 2>> expected_reads{{0x07, 3}, {0x04, 7}};
  EXPECT_EQ(this->bus_.reads, expected_reads);
}

TEST_F(BMM350Test, NormalStatusReadFailureDoesNotUseInvalidStatus) {
  this->bus_.command = 1;
  this->bus_.fail_register = 0x07;
  this->step(TestDevice::SetupStep::SETUP_STEP_NORMAL_DONE);
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
  EXPECT_TRUE(this->bus_.writes.empty());
  const std::vector<std::array<uint8_t, 2>> expected_reads{{0x07, 3}};
  EXPECT_EQ(this->bus_.reads, expected_reads);
}

TEST_F(BMM350Test, NormalWriteFailureTakesOneSnapshotWithoutRetrying) {
  this->bus_.command = 5;
  this->bus_.fail_register = 0x06;
  this->step(TestDevice::SetupStep::SETUP_STEP_FLUX_RESET_DONE);
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
  EXPECT_EQ(this->bus_.command, 5);
  const std::vector<std::array<uint8_t, 2>> expected_reads{{0x07, 3}, {0x04, 7}};
  EXPECT_EQ(this->bus_.reads, expected_reads);
}

TEST_F(BMM350Test, FailedDiagnosticReadStillStopsSetup) {
  this->bus_.command = 1;
  this->bus_.status_override = 0xA0;
  this->bus_.fail_register = 0x04;
  this->step(TestDevice::SetupStep::SETUP_STEP_NORMAL_DONE);
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
  EXPECT_TRUE(this->bus_.writes.empty());
  EXPECT_EQ(this->bus_.reads.size(), 2u);
}

TEST_F(BMM350Test, NormalCompletionRequiresNormalPowerModeBit) {
  this->bus_.command = 1;
  this->bus_.status_override = 0x20;
  this->step(TestDevice::SetupStep::SETUP_STEP_NORMAL_DONE);
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
  EXPECT_TRUE(this->bus_.writes.empty());
}

TEST_F(BMM350Test, IllegalNormalCompletionIsNotMasked) {
  this->bus_.command = 1;
  this->bus_.status_override = 0x38;  // Correct completion and normal bit, but illegal command.
  this->step(TestDevice::SetupStep::SETUP_STEP_NORMAL_DONE);
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
}

TEST_F(BMM350Test, ObservedIllegalStartupStatusNeverBecomesReady) {
  this->bus_.command = 2;
  this->bus_.status_override = 0x10;
  this->step(TestDevice::SetupStep::SETUP_STEP_NORMAL_DONE);
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
  this->device_.update();
  EXPECT_FALSE(this->x_.has_state());
}

TEST_F(BMM350Test, RejectsStaleDefaultOdrAfterNormalStart) {
  this->bus_.latch_configuration = false;
  this->initialize();
  EXPECT_EQ(this->bus_.aggregation, 0x16);
  EXPECT_EQ(this->bus_.effective_configuration, 0x14);
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
  this->device_.update();
  EXPECT_FALSE(this->x_.has_state());
}

TEST_F(BMM350Test, EffectiveConfigurationReadFailureStopsSetup) {
  this->bus_.fail_register = 0x08;
  this->initialize();
  EXPECT_TRUE(this->device_.is_failed());
  EXPECT_FALSE(this->device_.ready());
}

TEST_F(BMM350Test, PublishesSignedCompensatedDataAndRecoversFromReadFailure) {
  this->bus_.otp[14] = 0x0FFE;  // X offset -2 µT.
  this->initialize();
  ASSERT_TRUE(this->device_.ready());
  this->bus_.raw[0] = 0xFF;
  this->bus_.raw[1] = 0xFF;
  this->bus_.raw[2] = 0xFF;  // X raw -1, not unsigned 0xFFFFFF.
  this->device_.update();
  EXPECT_NEAR(this->x_.state, -2.00706998f, 0.00001f);
  EXPECT_NEAR(this->temperature_.state, -25.49f, 0.00001f);
  const float previous = this->x_.state;
  this->bus_.fail_register = 0x31;
  this->device_.update();
  EXPECT_TRUE(this->device_.status_has_warning());
  EXPECT_EQ(this->x_.state, previous);
  this->bus_.fail_register = 0xFF;
  this->bus_.data_ready = false;
  this->device_.update();
  EXPECT_EQ(this->x_.state, previous);
  this->bus_.data_ready = true;
  this->bus_.raw.fill(0);
  this->device_.update();
  EXPECT_FALSE(this->device_.status_has_warning());
  EXPECT_FLOAT_EQ(this->x_.state, -2.0f);
}

TEST_F(BMM350Test, DoesNotPublishDuringSetupOrOnFailedReads) {
  unsigned x_publications = 0;
  unsigned temperature_publications = 0;
  this->x_.add_on_state_callback([&](float) { x_publications++; });
  this->temperature_.add_on_state_callback([&](float) { temperature_publications++; });
  this->device_.setup();
  this->device_.update();
  EXPECT_FALSE(this->x_.has_state());
  EXPECT_FALSE(this->temperature_.has_state());
  this->initialize();
  ASSERT_TRUE(this->device_.ready());
  for (uint8_t reg : {0x30, 0x31}) {
    this->bus_.fail_register = reg;
    this->device_.update();
    EXPECT_EQ(x_publications, 0u);
    EXPECT_EQ(temperature_publications, 0u);
    EXPECT_FALSE(this->x_.has_state());
    EXPECT_FALSE(this->temperature_.has_state());
  }
  this->bus_.fail_register = 0xFF;
  this->device_.update();
  EXPECT_EQ(x_publications, 1u);
  EXPECT_EQ(temperature_publications, 1u);
  this->bus_.fail_register = 0x31;
  this->device_.update();
  EXPECT_EQ(x_publications, 1u);
  EXPECT_EQ(temperature_publications, 1u);
}

TEST(BMM350CompensationTest, AppliesPackedSignedTrimAndCrossAxisCorrection) {
  std::array<uint16_t, 32> otp{};
  otp[14] = 10;
  otp[15] = 20;
  otp[16] = 30;
  otp[21] = 0xE810;  // xy=+0.02, yx=-0.03.
  otp[22] = 0x04F8;  // zx=-0.01, zy=+0.005.
  BMM350Compensation compensation;
  compensation.load(otp);
  std::array<uint8_t, 12> raw{};
  BMM350Data data;
  ASSERT_TRUE(compensation.convert(raw.data(), data));
  EXPECT_NEAR(data.field[0], 9.59424345f, 0.00001f);
  EXPECT_NEAR(data.field[1], 20.28782730f, 0.00001f);
  EXPECT_NEAR(data.field[2], 29.99450330f, 0.00001f);
}

TEST(BMM350CompensationTest, CompensatesTemperatureGainAndAllThreeAxes) {
  std::array<uint16_t, 32> otp{};
  otp[13] = 0xFEFB;
  otp[14] = 0xDFFE;
  otp[15] = 0x08FC;
  otp[16] = 0x1000;
  otp[17] = 0xF820;
  otp[18] = 0x02E0;
  otp[19] = 0xFC10;
  otp[20] = 0x0818;
  otp[21] = 0xE810;
  otp[22] = 0x04F8;
  otp[24] = 0xFE00;
  const uint8_t raw[12] = {0xA0, 0x86, 0x01, 0xB0, 0x3C, 0xFF, 0xA8, 0x61, 0x00, 0xA0, 0x86, 0x01};
  BMM350Compensation compensation;
  compensation.load(otp);
  BMM350Data data;
  ASSERT_TRUE(compensation.convert(raw, data));
  EXPECT_NEAR(data.temperature, 71.35444233f, 0.0001f);
  EXPECT_NEAR(data.field[0], 713.20887532f, 0.001f);
  EXPECT_NEAR(data.field[1], -878.45551037f, 0.001f);
  EXPECT_NEAR(data.field[2], -1782.45822537f, 0.001f);
}

TEST(BMM350CompensationTest, MatchesPinnedVendorOracleForPositiveNegativeAndZeroTemperature) {
  // Generated by unchanged Bosch v1.10.0 (3daf377...) compiled with GCC 14.3 and executed in QEMU.
  static constexpr int32_t INPUTS[6][4] = {
      {100000, -50000, 25000, 100000},
      {100000, -50000, 25000, -100000},
      {100000, -50000, 25000, 0},
      {-1, 1, -1, 1},
      {-1, 1, -1, -1},
      {0, 0, 0, 0},
  };
  static constexpr float EXPECTED[2][6][4] = {
      {{706.99786377f, -353.49893188f, 179.37409973f, 72.63819122f},
       {706.99786377f, -353.49893188f, 179.37409973f, -123.61818695f},
       {706.99786377f, -353.49893188f, 179.37409973f, -25.48999977f},
       {-0.00706997840f, 0.00706997840f, -0.00717496406f, -25.48901939f},
       {-0.00706997840f, 0.00706997840f, -0.00717496406f, -25.49098206f},
       {0.0f, 0.0f, 0.0f, -25.48999977f}},
      {{713.20886230f, -878.45550537f, -1782.45812988f, 71.35444641f},
       {930.07928467f, -924.85937500f, -2122.34130859f, -124.13529968f},
       {820.36145020f, -902.31976318f, -1944.04345703f, -26.39042854f},
       {57.30075836f, -532.16058350f, -2131.50366211f, -26.38945198f},
       {57.30274582f, -532.16125488f, -2131.50732422f, -26.39140701f},
       {57.30945587f, -532.16851807f, -2131.49853516f, -26.39042854f}},
  };
  std::array<uint16_t, 32> otp{};
  for (size_t trim = 0; trim < 2; trim++) {
    if (trim != 0) {
      otp[13] = 0xFEFB;
      otp[14] = 0xDFFE;
      otp[15] = 0x08FC;
      otp[16] = 0x1000;
      otp[17] = 0xF820;
      otp[18] = 0x02E0;
      otp[19] = 0xFC10;
      otp[20] = 0x0818;
      otp[21] = 0xE810;
      otp[22] = 0x04F8;
      otp[24] = 0xFE00;
    }
    BMM350Compensation compensation;
    compensation.load(otp);
    for (size_t vector = 0; vector < 6; vector++) {
      uint8_t raw[12];
      for (size_t axis = 0; axis < 4; axis++) {
        const uint32_t word = static_cast<uint32_t>(INPUTS[vector][axis]);
        for (size_t byte = 0; byte < 3; byte++)
          raw[axis * 3 + byte] = word >> (byte * 8);
      }
      BMM350Data data;
      ASSERT_TRUE(compensation.convert(raw, data));
      for (size_t axis = 0; axis < 3; axis++)
        EXPECT_NEAR(data.field[axis], EXPECTED[trim][vector][axis], 0.001f) << trim << ":" << vector << ":" << axis;
      EXPECT_NEAR(data.temperature, EXPECTED[trim][vector][3], 0.00001f) << trim << ":" << vector;
    }
  }
}

}  // namespace esphome::bmm350::testing
