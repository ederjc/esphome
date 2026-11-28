#pragma once

#define ESPHOME_LOG_TAG(name, value) [[maybe_unused]] static constexpr const char *name = value
#define ESP_LOGE(...) ((void) 0)
#define ESP_LOGW(...) ((void) 0)
#define esph_log_e(...) ((void) 0)
#define esph_log_d(...) ((void) 0)
