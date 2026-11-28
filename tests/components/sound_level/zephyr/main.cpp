#include <zephyr/ztest.h>
#include "esphome/components/sound_level/sound_level.h"
#include <cmath>
#include <vector>

namespace esphome::sound_level::testing {

static std::vector<uint8_t> pcm(size_t frames, int16_t left, int16_t right = 0, uint8_t channels = 1) {
  std::vector<uint8_t> bytes;
  for (size_t frame = 0; frame < frames; ++frame) {
    for (uint8_t channel = 0; channel < channels; ++channel) {
      const uint16_t sample = channel == 0 ? left : right;
      bytes.push_back(sample & 0xff);
      bytes.push_back(sample >> 8);
    }
  }
  return bytes;
}

struct Level {
  microphone::MicrophoneSource source;
  SoundLevelComponent component;
  sensor::Sensor peak, rms;
  explicit Level(uint8_t channels = 1, bool passive = false) : source(channels, passive) {
    this->component.set_microphone_source(&this->source);
    this->component.set_measurement_duration(50);
    this->component.set_peak_sensor(&this->peak);
    this->component.set_rms_sensor(&this->rms);
    this->component.setup();
  }
};

ZTEST(sound_level, test_windows_cross_callback_boundaries_without_losing_samples) {
  Level level;
  level.source.emit(pcm(25, 16384));
  level.component.loop();
  zassert_true(level.peak.states.empty());
  level.source.emit(pcm(35, 8192));
  level.component.loop();
  zassert_equal(level.rms.states.size(), 1);
  zassert_within(level.peak.states[0], -6.0206f, 0.001f);
  zassert_within(level.rms.states[0], -8.0618f, 0.001f);
  level.source.emit(pcm(40, 8192));
  level.component.loop();
  zassert_equal(level.rms.states.size(), 2);
  zassert_within(level.rms.states[1], -12.0412f, 0.001f);
}

ZTEST(sound_level, test_independent_mono_sources_do_not_pool_stereo_channels) {
  Level left(1, true), right(1, true);
  left.source.select(0);
  right.source.select(1);
  left.source.set_running(true);
  right.source.set_running(true);
  const auto stereo = pcm(50, 16384, 8192, 2);
  left.source.emit(stereo, 2);
  right.source.emit(stereo, 2);
  left.component.loop();
  right.component.loop();
  zassert_within(left.peak.states.back(), -6.0206f, 0.001f);
  zassert_within(left.rms.states.back(), -6.0206f, 0.001f);
  zassert_within(right.peak.states.back(), -12.0412f, 0.001f);
  zassert_within(right.rms.states.back(), -12.0412f, 0.001f);
  left.component.stop();
  zassert_true(left.source.is_running());
  zassert_true(right.source.is_running());
}

ZTEST(sound_level, test_int16_min_and_silence_use_full_scale_reference) {
  Level level;
  level.source.emit(pcm(50, INT16_MIN));
  level.source.emit(pcm(50, 0));
  level.component.loop();
  zassert_equal(level.peak.states.size(), 2);
  zassert_within(level.peak.states[0], 0.0f, 0.0001f);
  zassert_within(level.rms.states[0], 0.0f, 0.0001f);
  zassert_true(std::isinf(level.peak.states[1]) && level.peak.states[1] < 0);
  zassert_true(std::isinf(level.rms.states[1]) && level.rms.states[1] < 0);
}

ZTEST(sound_level, test_stop_restart_discards_partial_window) {
  Level level;
  level.source.emit(pcm(25, 16384));
  level.component.stop();
  level.component.loop();
  zassert_true(std::isnan(level.rms.states.back()));
  level.component.start();
  level.source.emit(pcm(50, 8192));
  level.component.loop();
  zassert_equal(level.rms.states.size(), 2);
  zassert_within(level.rms.states.back(), -12.0412f, 0.001f);
  zassert_false(level.component.status_has_warning());
}

ZTEST(sound_level, test_queue_overflow_is_explicit_and_restart_recovers) {
  Level level;
  level.source.emit(pcm(250, 16384));
  level.component.loop();
  zassert_equal(level.rms.states.size(), 1);
  zassert_true(std::isnan(level.rms.states.back()));
  zassert_true(level.component.status_has_warning());
  level.component.loop();
  zassert_equal(level.rms.states.size(), 1);
  level.component.stop();
  level.component.loop();
  level.component.start();
  level.source.emit(pcm(50, INT16_MIN));
  level.component.loop();
  zassert_within(level.rms.states.back(), 0.0f, 0.0001f);
}

ZTEST(sound_level, test_invalid_stereo_setup_and_malformed_capture_are_rejected) {
  Level invalid(2);
  zassert_true(invalid.component.is_failed());
  invalid.component.start();
  invalid.component.loop();
  zassert_false(invalid.source.is_running());
  Level level;
  const uint8_t sample[] = {0, 0x40};
  level.source.emit_raw({sample, {16, 2, 1000}});
  level.component.loop();
  zassert_true(std::isnan(level.rms.states.back()));
  level.component.stop();
  level.component.loop();
  level.component.start();
  level.source.emit_raw({{sample, 1}, {16, 1, 1000}});
  level.component.loop();
  zassert_true(level.component.status_has_warning());
}

ZTEST(sound_level, test_passive_stop_observed_by_loop_discards_partial_window) {
  Level level(1, true);
  level.source.set_running(true);
  level.source.emit(pcm(25, 16384));
  level.source.set_running(false);
  level.component.loop();
  zassert_true(std::isnan(level.rms.states.back()));
  level.source.set_running(true);
  level.source.emit(pcm(50, 8192));
  level.component.loop();
  zassert_within(level.rms.states.back(), -12.0412f, 0.001f);
}

ZTEST(sound_level, test_peak_callback_stop_suppresses_old_rms_and_queued_windows) {
  Level level;
  bool first = true;
  level.peak.on_publish = [&](float) {
    if (first) {
      first = false;
      level.component.stop();
    }
  };
  level.source.emit(pcm(100, 16384));
  level.component.loop();
  zassert_equal(level.peak.states.size(), 1);
  zassert_true(level.rms.states.empty());
  level.component.loop();
  level.component.start();
  level.source.emit(pcm(50, 8192));
  level.component.loop();
  zassert_within(level.rms.states.back(), -12.0412f, 0.001f);
}

ZTEST(sound_level, test_stopped_peak_nan_restart_suppresses_stale_rms_nan) {
  Level level;
  level.source.emit(pcm(25, 16384));
  level.component.stop();
  bool restarted = false;
  level.peak.on_publish = [&](float value) {
    if (std::isnan(value) && !restarted) {
      restarted = true;
      level.component.start();
    }
  };
  level.component.loop();
  zassert_true(restarted);
  zassert_true(level.rms.states.empty());
  level.source.emit(pcm(50, 8192));
  level.component.loop();
  zassert_equal(level.rms.states.size(), 1);
  zassert_within(level.rms.states.back(), -12.0412f, 0.001f);
}

ZTEST(sound_level, test_fault_peak_nan_restart_suppresses_stale_rms_nan) {
  for (bool overrun : {false, true}) {
    Level level;
    bool restarted = false;
    level.peak.on_publish = [&](float value) {
      if (std::isnan(value) && !restarted) {
        restarted = true;
        level.component.start();
      }
    };
    if (overrun) {
      level.source.emit(pcm(250, 16384));
    } else {
      const uint8_t malformed[] = {0x40};
      level.source.emit_raw({malformed, {16, 1, 1000}});
    }
    level.component.loop();
    zassert_true(restarted);
    zassert_true(level.rms.states.empty());
    level.source.emit(pcm(50, 8192));
    level.component.loop();
    zassert_equal(level.rms.states.size(), 1);
    zassert_within(level.rms.states.back(), -12.0412f, 0.001f);
  }
}

ZTEST(sound_level, test_passive_stopped_peak_nan_backend_restart_suppresses_stale_rms_nan) {
  Level level(1, true);
  level.source.set_running(true);
  level.source.emit(pcm(25, 16384));
  level.source.set_running(false);
  bool restarted = false;
  level.peak.on_publish = [&](float value) {
    if (std::isnan(value) && !restarted) {
      restarted = true;
      level.source.set_running(true);
    }
  };
  level.component.loop();
  zassert_true(restarted);
  zassert_true(level.source.is_running());
  zassert_equal(level.peak.states.size(), 1);
  zassert_true(std::isnan(level.peak.states.back()));
  zassert_true(level.rms.states.empty());
  level.source.emit(pcm(25, 8192));
  level.component.loop();
  zassert_true(level.rms.states.empty());
  level.source.emit(pcm(25, 8192));
  level.component.loop();
  zassert_equal(level.rms.states.size(), 1);
  zassert_within(level.rms.states.back(), -12.0412f, 0.001f);
  zassert_false(level.component.status_has_warning());
}

ZTEST(sound_level, test_passive_fault_backend_restart_preserves_latched_nan) {
  for (bool overrun : {false, true}) {
    Level level(1, true);
    level.source.set_running(true);
    bool restarted = false;
    level.peak.on_publish = [&](float value) {
      if (std::isnan(value) && !restarted) {
        restarted = true;
        level.source.set_running(false);
        level.source.set_running(true);
      }
    };
    if (overrun) {
      level.source.emit(pcm(250, 16384));
    } else {
      const uint8_t malformed[] = {0x40};
      level.source.emit_raw({malformed, {16, 1, 1000}});
    }
    level.component.loop();
    zassert_true(restarted);
    zassert_equal(level.rms.states.size(), 1);
    zassert_true(std::isnan(level.rms.states.back()));
    level.source.emit(pcm(50, 8192));
    level.component.loop();
    zassert_equal(level.rms.states.size(), 1);
    zassert_true(level.component.status_has_warning());
    level.source.set_running(false);
    level.component.loop();
    level.source.set_running(true);
    level.source.emit(pcm(50, 8192));
    level.component.loop();
    zassert_equal(level.rms.states.size(), 2);
    zassert_within(level.rms.states.back(), -12.0412f, 0.001f);
    zassert_false(level.component.status_has_warning());
  }
}

ZTEST_SUITE(sound_level, NULL, NULL, NULL, NULL, NULL);
}  // namespace esphome::sound_level::testing
