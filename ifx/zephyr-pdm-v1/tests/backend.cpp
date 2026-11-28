#include "esphome/components/zephyr_dmic/zephyr_dmic.h"
#include "esphome/core/application.h"
#include <zephyr/ztest.h>

extern "C" const struct device *pdm_test_reset();
extern "C" void pdm_test_inject(unsigned int, uint16_t, uint16_t);

class Backend : public esphome::zephyr_dmic::ZephyrDMICMicrophone {
 public:
  using ZephyrDMICMicrophone::ZephyrDMICMicrophone;
  uint32_t used() { return k_mem_slab_num_used_get(&slab_); }
};

ZTEST(pdm_backend, test_actual_publish_callback_stop_preserves_borrowed_block_and_restart) {
  const auto *device = pdm_test_reset();
  static Backend invalid(device, 0, 2);
  invalid.setup();
  zassert_true(invalid.is_failed());
  static Backend microphone(device, 15625, 2);
  microphone.setup();
  zassert_false(microphone.is_failed());
  static Backend duplicate(device, 15625, 2);
  duplicate.setup();
  zassert_true(duplicate.is_failed());
  unsigned int callbacks = 0;
  microphone.add_data_callback([&](const esphome::microphone::AudioCapture &capture) {
    zassert_equal(capture.data.size(), 640);
    zassert_equal(capture.stream_info.get_channels(), 2);
    zassert_equal(capture.stream_info.get_sample_rate(), 15625);
    zassert_equal(capture.capture_time_us, 0);
    const auto *samples = reinterpret_cast<const uint16_t *>(capture.data.data());
    const uint16_t left = callbacks == 0 ? 17 : 42;
    zassert_equal(samples[0], left);
    zassert_equal(samples[1], left + 1000);
    microphone.stop();
    zassert_equal(microphone.used(), 1);
    zassert_equal(samples[0], left);
    zassert_equal(samples[319], left + 1159);
    ++callbacks;
  });
  for (uint16_t left : {17, 42}) {
    microphone.start();
    zassert_true(microphone.is_running());
    pdm_test_inject(64, 0, 0);
    pdm_test_inject(80, left, left + 1000);
    pdm_test_inject(80, left + 80, left + 1080);
    pdm_test_inject(80, 500, 1500);
    pdm_test_inject(80, 580, 1580);
    microphone.loop();
    zassert_true(microphone.is_stopped());
    zassert_equal(microphone.used(), 0);
    microphone.loop();
  }
  zassert_equal(callbacks, 2);
  microphone.start();
  esphome::App.loop_interval = 21;
  microphone.loop();
  zassert_true(microphone.is_stopped());
  zassert_equal(microphone.used(), 0);
  esphome::App.loop_interval = 16;
}

ZTEST_SUITE(pdm_backend, NULL, NULL, NULL, NULL, NULL);
