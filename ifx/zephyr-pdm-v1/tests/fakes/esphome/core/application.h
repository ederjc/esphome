#pragma once
#include <cstdint>
namespace esphome {
struct Application {
  uint32_t loop_interval{16};
  uint32_t get_loop_interval() const { return loop_interval; }
};
inline Application App;
}
