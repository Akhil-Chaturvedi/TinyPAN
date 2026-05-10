/*
 * Stub esp_log.h for ESP32 HAL compilation test.
 */
#ifndef ESP_LOG_H
#define ESP_LOG_H

#include <stdio.h>

/* Log level enum */
typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

/* Stub macros — just discard the log output */
#define ESP_LOGE(tag, fmt, ...)  do { } while(0)
#define ESP_LOGW(tag, fmt, ...)  do { } while(0)
#define ESP_LOGI(tag, fmt, ...)  do { } while(0)
#define ESP_LOGD(tag, fmt, ...)  do { } while(0)
#define ESP_LOGV(tag, fmt, ...)  do { } while(0)

#endif /* ESP_LOG_H */
