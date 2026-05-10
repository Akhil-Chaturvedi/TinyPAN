/*
 * Stub freertos/event_groups.h for ESP32 HAL compilation test.
 */
#ifndef FREERTOS_EVENT_GROUPS_H
#define FREERTOS_EVENT_GROUPS_H

#include "freertos/FreeRTOS.h"

EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t group);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                 BaseType_t clear_on_exit, BaseType_t wait_for_all,
                                 TickType_t ticks);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);

#endif /* FREERTOS_EVENT_GROUPS_H */
