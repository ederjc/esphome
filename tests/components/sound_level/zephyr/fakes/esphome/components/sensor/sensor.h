#pragma once
#include <vector>
#include <functional>
namespace esphome::sensor {
class Sensor {
 public:
  void publish_state(float value) {
    this->states.push_back(value);
    if (this->on_publish)
      this->on_publish(value);
  }
  std::vector<float> states;
  std::function<void(float)> on_publish;
};
}  // namespace esphome::sensor
#define SUB_SENSOR(name) \
 public: \
  void set_##name##_sensor(esphome::sensor::Sensor *sensor) { this->name##_sensor_ = sensor; } \
\
 protected: \
  esphome::sensor::Sensor *name##_sensor_{nullptr};
#define LOG_SENSOR(...) \
  do { \
  } while (0)
#define PUBLISH_STATE(sensor, value) \
  do { \
    if (sensor != nullptr) \
      sensor->publish_state(value); \
  } while (0)
