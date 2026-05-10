/*
 * Stub esp_err.h for ESP32 HAL compilation test.
 * Types and values match ESP-IDF v5.5.x.
 */
#ifndef ESP_ERR_H
#define ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM  0x101

const char *esp_err_to_name(esp_err_t code);

#endif /* ESP_ERR_H */
