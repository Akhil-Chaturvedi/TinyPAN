/*
 * Stub freertos/queue.h for ESP32 HAL compilation test.
 */
#ifndef FREERTOS_QUEUE_H
#define FREERTOS_QUEUE_H

#include "freertos/FreeRTOS.h"

QueueHandle_t xQueueCreate(uint32_t uxQueueLength, uint32_t uxItemSize);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);

#endif /* FREERTOS_QUEUE_H */
