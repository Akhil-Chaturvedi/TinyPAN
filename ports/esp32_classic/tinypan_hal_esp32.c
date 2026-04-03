/**
 * @file tinypan_hal_esp32.c
 * @brief Reference Hardware Abstraction Layer for ESP-IDF (Bluetooth Classic)
 * 
 * This file implements the TinyPAN `tinypan_hal.h` interface for an ESP32
 * running the official ESP-IDF framework with the Bluedroid host stack.
 * 
 * Target: PAN Client (BNEP) over Classic Bluetooth (BR/EDR).
 * 
 * @note Thread Safety
 * The ESP-IDF Bluetooth stack executes all callbacks (e.g. `esp_l2cap_cb`)
 * on a dedicated internal FreeRTOS task (`btu_task`). TinyPAN is strictly
 * single-threaded. Incoming data is bridged to the application thread via
 * are bridged via a small FreeRTOS event queue. The library uses the 
 * hal_mutex_x API (implemented here via FreeRTOS semaphores) to ensure 
 * transport queue integrity across task boundaries.
 */

#include "tinypan_hal.h"
#include "tinypan_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/message_buffer.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_l2cap_bt_api.h>
#include <esp_timer.h>
#include <string.h>
#include <stdlib.h>
#include "lwip/pbuf.h"

static const char* TAG = "TinyPAN_HAL";

/* ============================================================================
 * State & Sync Mechanisms
 * ============================================================================ */

static hal_l2cap_recv_callback_t s_recv_cb = NULL;
static void* s_recv_cb_data = NULL;

static hal_l2cap_event_callback_t s_event_cb = NULL;
static void* s_event_cb_data = NULL;

static void (*s_wakeup_cb)(void*) = NULL;
static void* s_wakeup_cb_data = NULL;

static bool s_hal_initialized = false;
/* Cross-task state: accessed by both the BTU callback task and the app task. 
 * Protected by s_state_spinlock (portENTER_CRITICAL) to ensure atomicity 
 * and hardware memory barriers on the dual-core Xtensa LX6. */
static portMUX_TYPE s_state_spinlock = portMUX_INITIALIZER_UNLOCKED;
static bool s_is_connected = false;
static uint32_t s_l2cap_handle = 0;
static bool s_tx_busy = false;
static bool s_tx_complete_pending = false;

/* Used to align and pack unaligned or non-contiguous data for DMA safety.
 * Declared as uint32_t to guarantee 4-byte alignment in the .bss section. */
static uint32_t s_tx_aligned_buf_raw[(TINYPAN_L2CAP_MTU + 32 + 3) / 4];
#define s_tx_aligned_buf ((uint8_t*)s_tx_aligned_buf_raw)

/* RX Message Buffer: Safely bridges raw Bluetooth payloads from the BT ISR
 * to the main application task without touching the lwIP heap allocators.
 * Size defaults to 2048 bytes to hold at least one maximum MTU burst. */
#ifndef TINYPAN_ESP_RX_MSG_BUF_SIZE
#define TINYPAN_ESP_RX_MSG_BUF_SIZE   2048
#endif

static MessageBufferHandle_t s_rx_msg_buf = NULL;
static uint8_t s_rx_poll_temp_buf[TINYPAN_L2CAP_MTU + 32];
static volatile uint8_t s_rx_ring_head = 0; /* Written by BT task, for diagnostic/counting only */
static volatile uint8_t s_rx_ring_tail = 0; /* Read by app task, for diagnostic/counting only */

typedef struct {
    int event_id;
    int status;
} esp_event_msg_t;

static QueueHandle_t s_event_queue = NULL;

/* ============================================================================
 * ESP-IDF L2CAP Callbacks
 * ============================================================================ */

static void esp_l2cap_cb(esp_bt_l2cap_cb_event_t event, esp_bt_l2cap_cb_param_t *param) {
    esp_event_msg_t event_msg = {0};

    switch (event) {
        case ESP_BT_L2CAP_INIT_EVT:
            ESP_LOGI(TAG, "L2CAP initialized");
            break;

        case ESP_BT_L2CAP_CL_INIT_EVT:
            if (param->cl_init.status == ESP_BT_L2CAP_SUCCESS) {
                portENTER_CRITICAL(&s_state_spinlock);
                s_l2cap_handle = param->cl_init.lcid;
                s_is_connected = true;
                s_tx_busy = false;
                portEXIT_CRITICAL(&s_state_spinlock);
                
                event_msg.event_id = HAL_L2CAP_EVENT_CONNECTED;
                if (xQueueSend(s_event_queue, &event_msg, 0) != pdTRUE) {
                    /* Drop event but preserve the hardware link; supervisor timeout will recover. */
                    ESP_LOGE(TAG, "Event queue full on CONNECTED; state desync possible");
                }
            } else {
                event_msg.event_id = HAL_L2CAP_EVENT_CONNECT_FAILED;
                event_msg.status = param->cl_init.status;
                /* Non-critical: if this drops, supervisor times out and retries. */
                xQueueSend(s_event_queue, &event_msg, 0);
                if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
            }
            break;

        case ESP_BT_L2CAP_CLOSE_EVT:
            portENTER_CRITICAL(&s_state_spinlock);
            s_is_connected = false;
            s_tx_busy = false;
            s_l2cap_handle = 0;
            portEXIT_CRITICAL(&s_state_spinlock);
            event_msg.event_id = HAL_L2CAP_EVENT_DISCONNECTED;
            if (xQueueSend(s_event_queue, &event_msg, 0) != pdTRUE) {
                /* Queue full: the supervisor cannot learn of this disconnect.
                 * The supervision timeout in the supervisor will eventually recover,
                 * but log the anomaly for diagnostics. */
                ESP_LOGE(TAG, "Event queue full on DISCONNECTED; supervisor will timeout");
            }
            if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
            break;

        case ESP_BT_L2CAP_DATA_IND_EVT:
            /* Thread-Safe RX Path: Push raw bytes into FreeRTOS MessageBuffer.
             * Crucially, we NEVER call lwIP functions (like pbuf_alloc) from this BT
             * callback task to avoid corrupting the unprotected lwIP heap or destroying
             * RTOS interrupt latency with lock-heavy operations. */
            if (param->data_ind.len > 0 && param->data_ind.data) {
                size_t sent = xMessageBufferSend(s_rx_msg_buf, param->data_ind.data, param->data_ind.len, 0);
                if (sent == param->data_ind.len) {
                    s_rx_ring_head++; /* Purely for statistics/health check */
                    if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
                } else {
                    ESP_LOGW(TAG, "RX MessageBuffer full, dropped frame (len=%u)", param->data_ind.len);
                }
            }
            break;

        case ESP_BT_L2CAP_CONG_EVT:
            if (param->cong.is_congested) {
                portENTER_CRITICAL(&s_state_spinlock);
                s_tx_busy = true;
                portEXIT_CRITICAL(&s_state_spinlock);
                ESP_LOGD(TAG, "L2CAP Controller Congested");
            } else {
                portENTER_CRITICAL(&s_state_spinlock);
                s_tx_busy = false;
                portEXIT_CRITICAL(&s_state_spinlock);
                ESP_LOGD(TAG, "L2CAP Controller Uncongested");
                event_msg.event_id = HAL_L2CAP_EVENT_CAN_SEND_NOW;
                /* Non-critical: loss of a CAN_SEND_NOW event is recovered by the
                 * next transmit attempt calling hal_bt_l2cap_request_can_send_now(). */
                xQueueSend(s_event_queue, &event_msg, 0);
                if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
            }
            break;



        default:
            ESP_LOGD(TAG, "Unhandled L2CAP event: %d", event);
            break;
    }
}

/* ============================================================================
 * TinyPAN Polling Thread Bridge
 * ============================================================================ */

void hal_bt_poll(void) {
    if (!s_hal_initialized) return;

    /* Drain L2CAP connection/disconnection events */
    esp_event_msg_t evt_msg;
    while (xQueueReceive(s_event_queue, &evt_msg, 0) == pdTRUE) {
        if (s_event_cb) {
            s_event_cb((hal_l2cap_event_t)evt_msg.event_id, evt_msg.status, s_event_cb_data);
        }
    }

    /* 1b. Fire deferred completion events (Breaks recursion loop from app thread) */
    if (s_tx_complete_pending) {
        s_tx_complete_pending = false;
        if (s_event_cb) {
            s_event_cb(HAL_L2CAP_EVENT_TX_COMPLETE, 0, s_event_cb_data);
        }
    }

    /* Drain L2CAP data from MessageBuffer */
    size_t rx_len;
    while ((rx_len = xMessageBufferReceive(s_rx_msg_buf, s_rx_poll_temp_buf, sizeof(s_rx_poll_temp_buf), 0)) > 0) {
        if (s_recv_cb) {
            s_recv_cb(s_rx_poll_temp_buf, (uint16_t)rx_len, s_recv_cb_data);
        }
        s_rx_ring_tail++; /* Purely for statistics/health check */
    }
}

/* ============================================================================
 * HAL Implementation
 * ============================================================================ */

int hal_bt_init(void) {
    if (s_hal_initialized) return 0;

    s_event_queue = xQueueCreate(TINYPAN_ESP_EVENT_QUEUE_SIZE, sizeof(esp_event_msg_t));
    s_rx_msg_buf = xMessageBufferCreate(TINYPAN_ESP_RX_MSG_BUF_SIZE);

    /* Reset diagnostic counters */
    s_rx_ring_head = 0;
    s_rx_ring_tail = 0;

    esp_err_t ret = esp_bt_l2cap_register_callback(esp_l2cap_cb);
    if (ret != ESP_OK) return -1;

    ret = esp_bt_l2cap_init();
    if (ret != ESP_OK) return -1;

    s_hal_initialized = true;
    return 0;
}

void hal_bt_deinit(void) {
    if (!s_hal_initialized) return;

    portENTER_CRITICAL(&s_state_spinlock);
    uint32_t handle = s_l2cap_handle;
    bool connected = s_is_connected;
    s_is_connected = false;
    s_tx_busy = false;
    s_l2cap_handle = 0;
    portEXIT_CRITICAL(&s_state_spinlock);
    
    if (connected) {
        esp_bt_l2cap_close(handle);
    }
    
    esp_bt_l2cap_deinit();
    
    if (s_event_queue) vQueueDelete(s_event_queue);
    
    if (s_rx_msg_buf) vMessageBufferDelete(s_rx_msg_buf);
    
    s_rx_ring_head = 0;
    s_rx_ring_tail = 0;
    
    s_hal_initialized = false;
}

int hal_bt_l2cap_connect(const uint8_t* remote_addr, uint16_t local_mtu) {
    (void)local_mtu;
    esp_err_t ret = esp_bt_l2cap_connect((uint8_t*)remote_addr, ESP_BT_PSM_BNEP, 0);
    return (ret == ESP_OK) ? 0 : -1;
}

void hal_bt_l2cap_disconnect(void) {
    portENTER_CRITICAL(&s_state_spinlock);
    uint32_t handle = s_l2cap_handle;
    bool connected = s_is_connected;
    portEXIT_CRITICAL(&s_state_spinlock);

    if (connected) {
        esp_bt_l2cap_close(handle);
    }
}

void hal_bt_l2cap_register_recv_callback(hal_l2cap_recv_callback_t cb, void* user_data) {
    s_recv_cb = cb;
    s_recv_cb_data = user_data;
}

void hal_bt_l2cap_register_event_callback(hal_l2cap_event_callback_t cb, void* user_data) {
    s_event_cb = cb;
    s_event_cb_data = user_data;
}

void hal_bt_set_wakeup_callback(void (*cb)(void*), void* user_data) {
    s_wakeup_cb = cb;
    s_wakeup_cb_data = user_data;
}

bool hal_bt_l2cap_is_connected(void) {
    bool connected;
    portENTER_CRITICAL(&s_state_spinlock);
    connected = s_is_connected;
    portEXIT_CRITICAL(&s_state_spinlock);
    return connected;
}

bool hal_bt_l2cap_can_send(void) {
    bool can_send;
    portENTER_CRITICAL(&s_state_spinlock);
    can_send = s_is_connected && !s_tx_busy;
    portEXIT_CRITICAL(&s_state_spinlock);
    return can_send;
}

void hal_bt_l2cap_request_can_send_now(void) {
    portENTER_CRITICAL(&s_state_spinlock);
    bool connected = s_is_connected;
    bool tx_busy = s_tx_busy;  /* Check congestion state */
    portEXIT_CRITICAL(&s_state_spinlock);

    if (connected && !tx_busy) {
        esp_event_msg_t msg;
        msg.event_id = HAL_L2CAP_EVENT_CAN_SEND_NOW;
        msg.status = 0;
        xQueueSend(s_event_queue, &msg, 0);
        if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
    }
}

int hal_bt_l2cap_send(const uint8_t* data, uint16_t len) {
    portENTER_CRITICAL(&s_state_spinlock);
    uint32_t handle = s_l2cap_handle;
    bool connected = s_is_connected;
    bool tx_busy = s_tx_busy;
    portEXIT_CRITICAL(&s_state_spinlock);

    if (!connected) return -1;
    if (tx_busy) return 1;

    /* Bounce to aligned driver buffer if needed */
    if (((uintptr_t)data & 3) != 0) {
        if (len > sizeof(s_tx_aligned_buf)) return -1;
        memcpy(s_tx_aligned_buf, data, len);
        esp_err_t ret = esp_bt_l2cap_data_write(handle, s_tx_aligned_buf, len);
        /* Contract: Do NOT fire TX_COMPLETE for contiguous send() */
        if (ret == ESP_OK) return 0;
        if (ret == ESP_ERR_NO_MEM) {
            portENTER_CRITICAL(&s_state_spinlock);
            s_tx_busy = true;
            portEXIT_CRITICAL(&s_state_spinlock);
            return 1;
        }
        return -1;
    }

    esp_err_t ret = esp_bt_l2cap_data_write(handle, (uint8_t*)data, len);
    /* Contract: Do NOT fire TX_COMPLETE for contiguous send() */
    if (ret == ESP_OK) return 0;
    if (ret == ESP_ERR_NO_MEM) {
        portENTER_CRITICAL(&s_state_spinlock);
        s_tx_busy = true;
        portEXIT_CRITICAL(&s_state_spinlock);
        return 1;
    }
    return -1;
}

int hal_bt_l2cap_send_iovec(const tinypan_iovec_t* iov, uint16_t iov_count) {
    /* Architectural Note: Bluedroid's `esp_bt_l2cap_data_write` requires a 
     * contiguous buffer. We copy the BNEP iovec into a static aligned buffer 
     * here. While this is NOT zero-copy on ESP32, the transport layer provides 
     * the infrastructure for DMA-native BLE/BT stacks (e.g. nRF52). */
    portENTER_CRITICAL(&s_state_spinlock);
    uint32_t handle = s_l2cap_handle;
    bool connected = s_is_connected;
    bool tx_busy = s_tx_busy;
    portEXIT_CRITICAL(&s_state_spinlock);

    if (!connected) return -1;
    if (tx_busy) return 1;

    uint32_t total_len = 0;
    if (iov_count == 1) {
        /* Optimize single-buffer case: avoid memcpy into bounce buffer if possible,
         * but ESP L2CAP requires the buffer to remain valid until the callback.
         * For DMA safety and API compliance, we always align/bounce in this HAL
         * unless the caller provided an explicitly DMA-capable buffer (we can't know). */
        if (iov[0].iov_len > TINYPAN_L2CAP_MTU) return -1;
        memcpy(s_tx_aligned_buf, iov[0].iov_base, iov[0].iov_len);
        total_len = iov[0].iov_len;
    } else {
        /* Scatter-Gather concatenation */
        for (int i = 0; i < iov_count; i++) {
            if (total_len + iov[i].iov_len > TINYPAN_L2CAP_MTU) return -1; /* Overflow */
            memcpy(s_tx_aligned_buf + total_len, iov[i].iov_base, iov[i].iov_len);
            total_len += iov[i].iov_len;
        }
    }
    esp_err_t err = esp_bt_l2cap_data_write(handle, s_tx_aligned_buf, total_len);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NO_MEM) {
            portENTER_CRITICAL(&s_state_spinlock);
            s_tx_busy = true;
            portEXIT_CRITICAL(&s_state_spinlock);
            return 1;
        }
        ESP_LOGE(TAG, "L2CAP write failed: %d", err);
        return -1;
    }
    
    /* Defer completion to hal_bt_poll to prevent recursion if app thread sends immediately. */
    s_tx_complete_pending = true;
    
    return 0;
}

void hal_get_local_bd_addr(uint8_t addr[HAL_BD_ADDR_LEN]) {
    const uint8_t* mac = esp_bt_dev_get_address();
    if (mac) {
        memcpy(addr, mac, HAL_BD_ADDR_LEN);
    } else {
        memset(addr, 0, HAL_BD_ADDR_LEN);
    }
}

uint32_t hal_get_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

uint32_t hal_bt_get_next_timeout_ms(void) {
    return 0xFFFFFFFF;
}

uint16_t hal_bt_l2cap_get_mtu(void) {
    return TINYPAN_L2CAP_MTU;
}

hal_mutex_t hal_mutex_create(void) {
    return (hal_mutex_t)xSemaphoreCreateMutex();
}

void hal_mutex_lock(hal_mutex_t mutex) {
    if (mutex) xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

void hal_mutex_unlock(hal_mutex_t mutex) {
    if (mutex) xSemaphoreGive((SemaphoreHandle_t)mutex);
}

void hal_mutex_destroy(hal_mutex_t mutex) {
    if (mutex) vSemaphoreDelete((SemaphoreHandle_t)mutex);
}
