/*
 * Stub freertos/semphr.h for ESP32 HAL compilation test.
 */
#ifndef FREERTOS_SEMPHR_H
#define FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);

#endif /* FREERTOS_SEMPHR_H */
