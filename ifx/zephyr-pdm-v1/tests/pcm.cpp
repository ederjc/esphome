#include <zephyr/ztest.h>
#include "esphome/components/audio/pcm_utils.h"

ZTEST(pdm_pcm, test_select_both_channels_and_gain_saturates) {
  const uint8_t input[] = {1, 0, 0xff, 0xff, 0x00, 0x40, 0x00, 0xc0};
  const uint8_t both[] = {0, 1};
  uint8_t output[8]{};
  esphome::audio::copy_pcm_frames_with_gain(input, output, 2, 2, 2, both, 2, 2, 2);
  const uint8_t expected[] = {2, 0, 0xfe, 0xff, 0xff, 0x7f, 0x00, 0x80};
  zassert_mem_equal(output, expected, sizeof(expected));
  const uint8_t right[] = {1};
  uint8_t selected[4]{};
  esphome::audio::copy_pcm_frames_with_gain(input, selected, 2, 2, 2, right, 1, 1, 2);
  const uint8_t expected_right[] = {0xff, 0xff, 0x00, 0xc0};
  zassert_mem_equal(selected, expected_right, sizeof(expected_right));
}

ZTEST(pdm_pcm, test_narrowing_rounds_signed_samples_and_preserves_shape) {
  const uint8_t input[] = {0x80, 0, 0x80, 0xff, 0xff, 0x7f, 0, 0x80};
  const uint8_t both[] = {0, 1};
  uint8_t output[4]{};
  esphome::audio::copy_pcm_frames_with_gain(input, output, 2, 2, 1, both, 2, 1, 2);
  const uint8_t expected[] = {1, 0xff, 0x7f, 0x80};
  zassert_mem_equal(output, expected, sizeof(expected));
}

ZTEST_SUITE(pdm_pcm, NULL, NULL, NULL, NULL, NULL);
