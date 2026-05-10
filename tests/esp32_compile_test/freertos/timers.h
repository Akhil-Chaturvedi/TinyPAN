/*
 * Stub freertos/timers.h for ESP32 HAL compilation test.
 */
#ifndef FREERTOS_TIMERS_H
#define FREERTOS_TIMERS_H

#include "freertos/FreeRTOS.h"

typedef void (*TimerCallbackFunction_t)(TimerHandle_t);

TimerHandle_t xTimerCreate(const char *name, TickType_t period, BaseType_t auto_reload,
                            void *id, TimerCallbackFunction_t callback);
BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t ticks);
BaseType_t xTimerReset(TimerHandle_t timer, TickType_t ticks);

#endif /* FREERTOS_TIMERS_H */
