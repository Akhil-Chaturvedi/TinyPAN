/*
 * Stub FreeRTOS.h for ESP32 HAL compilation test.
 */
#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>
#include <stddef.h>

/* Tick type */
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void * TaskHandle_t;
typedef void * QueueHandle_t;
typedef void * SemaphoreHandle_t;
typedef void * MessageBufferHandle_t;
typedef void * TimerHandle_t;
typedef void * EventGroupHandle_t;
typedef uint32_t EventBits_t;

#define pdTRUE          ((BaseType_t) 1)
#define pdFALSE         ((BaseType_t) 0)
#define pdPASS          (pdTRUE)
#define pdFAIL          (pdFALSE)
#define portMAX_DELAY   (TickType_t) 0xffffffff

/* configMAX_PRIORITIES must be defined for TINYPAN_ESP_RX_TASK_PRIO */
#ifndef configMAX_PRIORITIES
#define configMAX_PRIORITIES 7
#endif

static inline TickType_t pdMS_TO_TICKS(uint32_t ms) {
    (void)ms;
    return 1;  /* Stub */
}

/* Spinlock type (ESP-IDF port) */
typedef struct {
    int owner;
    int count;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { 0, 0 }

/* Critical section macros — stubs that just execute the block */
#define portENTER_CRITICAL_SAFE(mux) do { } while(0)
#define portEXIT_CRITICAL_SAFE(mux)  do { } while(0)
#define portENTER_CRITICAL(mux)      do { } while(0)
#define portEXIT_CRITICAL(mux)       do { } while(0)

/* Task functions */
BaseType_t xTaskCreate(void *task_fn, const char *name, uint16_t stack_depth,
                       void *param, uint32_t priority, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);

#endif /* FREERTOS_H */
