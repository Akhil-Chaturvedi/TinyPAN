/*
 * ESP-IDF Stub Implementations for Compilation Test
 *
 * Provides minimal stubs for all ESP-IDF and FreeRTOS functions used by
 * the TinyPAN ESP32 HAL. These allow the linker to succeed so we can
 * verify the HAL compiles against the correct API signatures.
 */

#include <string.h>

/* ===== FreeRTOS Stubs ===== */

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"

BaseType_t xTaskCreate(void *task_fn, const char *name, uint16_t stack_depth,
                       void *param, uint32_t priority, TaskHandle_t *handle) {
    (void)task_fn; (void)name; (void)stack_depth;
    (void)param; (void)priority; (void)handle;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }

QueueHandle_t xQueueCreate(uint32_t uxQueueLength, uint32_t uxItemSize) {
    (void)uxQueueLength; (void)uxItemSize;
    return (QueueHandle_t)1;
}

void vQueueDelete(QueueHandle_t queue) { (void)queue; }

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks) {
    (void)queue; (void)item; (void)ticks;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks) {
    (void)queue; (void)item; (void)ticks;
    return pdFALSE;
}

MessageBufferHandle_t xMessageBufferCreate(size_t bufferSizeBytes) {
    (void)bufferSizeBytes;
    return (MessageBufferHandle_t)1;
}

void vMessageBufferDelete(MessageBufferHandle_t msgbuf) { (void)msgbuf; }

size_t xMessageBufferSend(MessageBufferHandle_t msgbuf, const void *data,
                           size_t len, TickType_t ticks) {
    (void)msgbuf; (void)data; (void)ticks;
    return len;
}

size_t xMessageBufferReceive(MessageBufferHandle_t msgbuf, void *buf,
                              size_t buf_len, TickType_t ticks) {
    (void)msgbuf; (void)buf; (void)buf_len; (void)ticks;
    return 0;
}

size_t xMessageBufferSpacesAvailable(MessageBufferHandle_t msgbuf) {
    (void)msgbuf;
    return 4096;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)1;
}

void vSemaphoreDelete(SemaphoreHandle_t sem) { (void)sem; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    (void)sem; (void)ticks;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    (void)sem;
    return pdTRUE;
}

TimerHandle_t xTimerCreate(const char *name, TickType_t period, BaseType_t auto_reload,
                            void *id, TimerCallbackFunction_t callback) {
    (void)name; (void)period; (void)auto_reload;
    (void)id; (void)callback;
    return (TimerHandle_t)1;
}

BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t ticks) {
    (void)timer; (void)ticks;
    return pdPASS;
}

BaseType_t xTimerReset(TimerHandle_t timer, TickType_t ticks) {
    (void)timer; (void)ticks;
    return pdPASS;
}

EventGroupHandle_t xEventGroupCreate(void) {
    return (EventGroupHandle_t)1;
}

void vEventGroupDelete(EventGroupHandle_t group) { (void)group; }

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                 BaseType_t clear_on_exit, BaseType_t wait_for_all,
                                 TickType_t ticks) {
    (void)group; (void)bits; (void)clear_on_exit;
    (void)wait_for_all; (void)ticks;
    return bits;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    (void)group;
    return bits;
}

/* ===== ESP-IDF Stubs ===== */

#include "esp_err.h"
#include "esp_bt_device.h"
#include "esp_timer.h"
#include "esp_l2cap_bt_api.h"

const char *esp_err_to_name(esp_err_t code) {
    (void)code;
    return "ESP_OK";
}

const uint8_t *esp_bt_dev_get_address(void) {
    static const uint8_t addr[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    return addr;
}

int64_t esp_timer_get_time(void) {
    return 0;
}

esp_err_t esp_bt_l2cap_register_callback(esp_bt_l2cap_cb_t callback) {
    (void)callback;
    return ESP_OK;
}

esp_err_t esp_bt_l2cap_init(void) {
    return ESP_OK;
}

esp_err_t esp_bt_l2cap_deinit(void) {
    return ESP_OK;
}

esp_err_t esp_bt_l2cap_vfs_register(void) {
    return ESP_OK;
}

esp_err_t esp_bt_l2cap_vfs_unregister(void) {
    return ESP_OK;
}

esp_err_t esp_bt_l2cap_connect(esp_bt_l2cap_cntl_flags_t cntl_flag,
                                uint16_t remote_psm,
                                esp_bd_addr_t peer_bd_addr) {
    (void)cntl_flag; (void)remote_psm; (void)peer_bd_addr;
    return ESP_OK;
}

esp_err_t esp_bt_l2cap_start_srv(esp_bt_l2cap_cntl_flags_t cntl_flag, uint16_t local_psm) {
    (void)cntl_flag; (void)local_psm;
    return ESP_OK;
}

esp_err_t esp_bt_l2cap_stop_all_srv(void) {
    return ESP_OK;
}

esp_err_t esp_bt_l2cap_stop_srv(uint16_t local_psm) {
    (void)local_psm;
    return ESP_OK;
}

esp_err_t esp_bt_l2cap_get_protocol_status(esp_bt_l2cap_protocol_status_t *status) {
    (void)status;
    return ESP_OK;
}
