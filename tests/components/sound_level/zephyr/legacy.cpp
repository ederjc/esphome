#undef USE_ZEPHYR
#define USE_ESP32
#define sound_level legacy_sound_level
#include "../../../../esphome/components/sound_level/sound_level.cpp"
#include <zephyr/ztest.h>

namespace esphome::legacy_sound_level::testing {

ZTEST(sound_level_esp32, test_existing_pooled_stereo_math_and_restart_are_preserved) {
  microphone::MicrophoneSource source(2);
  SoundLevelComponent level;
  sensor::Sensor peak, rms;
  level.set_microphone_source(&source);
  level.set_measurement_duration(50);
  level.set_peak_sensor(&peak);
  level.set_rms_sensor(&rms);
  level.setup();
  level.loop();
  std::vector<uint8_t> stereo;
  for (size_t i = 0; i < 50; ++i)
    stereo.insert(stereo.end(), {0, 0x40, 0, 0x20});
  source.emit(stereo, 2);
  level.loop();
  level.loop();
  zassert_equal(rms.states.size(), 1);
  zassert_within(peak.states.back(), -6.0206f, 0.001f);
  zassert_within(rms.states.back(), -8.0618f, 0.001f);
  source.emit({stereo.data(), 100}, 2);
  level.loop();
  level.stop();
  level.loop();
  zassert_true(std::isnan(rms.states.back()));
  level.start();
  level.loop();
  for (size_t i = 0; i < stereo.size(); i += 4)
    stereo[i + 1] = 0x20;
  source.emit(stereo, 2);
  level.loop();
  level.loop();
  zassert_within(rms.states.back(), -12.0412f, 0.001f);
}

ZTEST_SUITE(sound_level_esp32, NULL, NULL, NULL, NULL, NULL);
}  // namespace esphome::legacy_sound_level::testing
