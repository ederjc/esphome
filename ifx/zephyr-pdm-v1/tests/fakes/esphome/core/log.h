#pragma once
#include <zephyr/sys/printk.h>
#define ESP_LOGE(tag, format, ...) printk(format "\n", ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) printk(format "\n", ##__VA_ARGS__)
#define ESP_LOGCONFIG(tag, format, ...) printk(format "\n", ##__VA_ARGS__)
