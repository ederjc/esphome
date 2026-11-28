#pragma once
#include <cinttypes>
#define ESPHOME_LOG_TAG(name, text) [[maybe_unused]] static const char *name = text
#define ESP_LOGCONFIG(...) \
  do { \
  } while (0)
#define ESP_LOGW(...) \
  do { \
  } while (0)
#define ESP_LOGE(...) \
  do { \
  } while (0)
#define LOG_STR(text) text
