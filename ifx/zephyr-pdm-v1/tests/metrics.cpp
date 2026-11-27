#include "../../../ifx/pdm_stereo_metrics.h"
#include <zephyr/ztest.h>

class Metrics : public PDMChannelMetrics {
 public:
  uint32_t windows() const { return windows_; }
  bool valid() const { return run_valid_; }
};

ZTEST(pdm_metrics, test_complete_stereo_windows_restart_and_quiet_gap) {
  Metrics metrics;
  static std::array<uint8_t, 640> data{};
  for (size_t i = 0; i < data.size(); i += 4) {
    data[i] = 1;
    data[i + 2] = 2;
  }
  esphome::microphone::AudioCapture capture{data, esphome::audio::AudioStreamInfo(16, 2, 15625), 0};
  esphome::test_now = 0;
  metrics.boot();
  metrics.stopped();
  for (unsigned int run = 0; run < 3; ++run) {
    esphome::test_now += 1000;
    zassert_true(metrics.starting());
    for (unsigned int i = 0; i <= 500; ++i) {
      metrics.consume(capture);
      esphome::test_now += 10;
    }
    zassert_equal(metrics.windows(), 1);
    zassert_true(metrics.valid());
    metrics.stopped();
  }
  esphome::test_now += 1000;
  zassert_true(metrics.complete());
  metrics.consume(capture);
  esphome::test_now += 1000;
  zassert_false(metrics.starting());
  zassert_false(metrics.complete());
}

ZTEST(pdm_metrics, test_short_blocks_do_not_count_as_complete_windows) {
  Metrics metrics;
  esphome::test_now = 0;
  metrics.stopped();
  esphome::test_now = 1000;
  zassert_true(metrics.starting());
  const uint8_t data[] = {1, 0, 2, 0};
  metrics.consume({data, esphome::audio::AudioStreamInfo(16, 2, 15625), 0});
  zassert_false(metrics.valid());
  zassert_equal(metrics.windows(), 0);
  metrics.stopped();
  esphome::test_now += 1000;
  zassert_false(metrics.starting());
  zassert_false(metrics.complete());
}

ZTEST(pdm_metrics, test_concurrent_start_and_early_stop_cannot_complete) {
  Metrics metrics;
  esphome::test_now = 0;
  metrics.stopped();
  esphome::test_now = 1000;
  zassert_true(metrics.starting());
  zassert_false(metrics.starting());
  metrics.stopped();
  esphome::test_now += 1000;
  zassert_false(metrics.starting());
  zassert_false(metrics.complete());
}

ZTEST(pdm_metrics, test_initial_quiet_gap_is_required_before_start) {
  Metrics metrics;
  esphome::test_now = 0;
  metrics.stopped();
  esphome::test_now = 999;
  zassert_false(metrics.starting());
  zassert_equal(metrics.windows(), 0);
  zassert_false(metrics.complete());
}

ZTEST_SUITE(pdm_metrics, NULL, NULL, NULL, NULL, NULL);
