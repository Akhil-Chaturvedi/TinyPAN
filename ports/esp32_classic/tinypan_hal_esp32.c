/**
 * @file tinypan_hal_esp32.c
 * @brief Reference Hardware Abstraction Layer for ESP-IDF (Bluetooth Classic)
 * 
 * This file implements the TinyPAN `tinypan_hal.h` interface for an ESP32
 * running the official ESP-IDF framework with the Bluedroid host stack.
 * 
 * Target: Custom PAN Client (BNEP) over Classic Bluetooth (BR/EDR).
 * 
 * @note Thread Safety
 * The ESP-IDF Bluetooth stack executes all callbacks (like `esp_bt_l2cap_cb`)
 * on a dedicated internal FreeRTOS task (`btu_task`). However, TinyPAN is
 * strictly a single-threaded synchronous library.
 * To safely bridge this, this HAL uses a FreeRTOS `QueueHandle_t` to funnel
 * incoming data and events from the BT task into the application task where
 * `tinypan_process()` is polled.
 */

#include "tinypan_hal.h"
#include "tinypan_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_log.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_l2cap_bt_api.h>
#include <esp_timer.h>
#include <string.h>
#include <stdlib.h>

static const char* TAG = "TinyPAN_HAL";

/* ============================================================================
 * State & Sync Mechanisms
 * ============================================================================ */

static hal_l2cap_recv_callback_t s_recv_cb = NULL;
static void* s_recv_cb_data = NULL;

static hal_l2cap_event_callback_t s_event_cb = NULL;
static void* s_event_cb_data = NULL;

static bool s_hal_initialized = false;
static bool s_is_connected = false;
static uint32_t s_l2cap_handle = 0;
static bool s_tx_busy = false;

/* Used to align and pack unaligned or non-contiguous data for DMA safety */
static uint8_t s_tx_aligned_buf[TINYPAN_L2CAP_MTU + 32];

typedef struct {
    int event_id;
    int status;
} esp_event_msg_t;

typedef struct {
    uint8_t* data;
    uint16_t len;
} esp_rx_msg_t;

static QueueHandle_t s_event_queue = NULL;
static QueueHandle_t s_rx_queue = NULL;

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
                s_l2cap_handle = param->cl_init.lcid;
                s_is_connected = true;
                s_tx_busy = false;
                event_msg.event_id = HAL_L2CAP_EVENT_CONNECTED;
                xQueueSend(s_event_queue, &event_msg, 0);
            } else {
                event_msg.event_id = HAL_L2CAP_EVENT_CONNECT_FAILED;
                event_msg.status = param->cl_init.status;
                xQueueSend(s_event_queue, &event_msg, 0);
            }
            break;

        case ESP_BT_L2CAP_CLOSE_EVT:
            s_is_connected = false;
            event_msg.event_id = HAL_L2CAP_EVENT_DISCONNECTED;
            xQueueSend(s_event_queue, &event_msg, 0);
            break;

        case ESP_BT_L2CAP_DATA_IND_EVT:
            /* Thread Safety Fix: Allocate a standard heap buffer, pass pointer to queue, 
             * and free it safely in the hal_bt_poll() loop on the user thread. */
            if (param->data_ind.len > 0 && param->data_ind.data) {
                uint8_t* buf = malloc(param->data_ind.len);
                if (buf != NULL) {
                    memcpy(buf, param->data_ind.data, param->data_ind.len);
                    esp_rx_msg_t rx_msg = { .data = buf, .len = param->data_ind.len };
                    if (xQueueSend(s_rx_queue, &rx_msg, 0) != pdTRUE) {
                        free(buf);
                        ESP_LOGW(TAG, "RX Queue full, dropped inbound L2CAP frame");
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to allocate standard heap for RX");
                }
            }
            break;

        case ESP_BT_L2CAP_CONG_EVT:
            if (param->cong.is_congested) {
                s_tx_busy = true;
                ESP_LOGD(TAG, "L2CAP Controller Congested");
            } else {
                s_tx_busy = false;
                ESP_LOGD(TAG, "L2CAP Controller Uncongested");
                event_msg.event_id = HAL_L2CAP_EVENT_CAN_SEND_NOW;
                xQueueSend(s_event_queue, &event_msg, 0);
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

    /* Drain L2CAP data */
    esp_rx_msg_t rx_msg;
    while (xQueueReceive(s_rx_queue, &rx_msg, 0) == pdTRUE) {
        if (s_recv_cb && rx_msg.data) {
            s_recv_cb(rx_msg.data, rx_msg.len, s_recv_cb_data);
        }
        if (rx_msg.data) {
            free(rx_msg.data);
        }
    }
}

/* ============================================================================
 * HAL Implementation
 * ============================================================================ */

int hal_bt_init(void) {
    if (s_hal_initialized) return 0;

    s_event_queue = xQueueCreate(TINYPAN_ESP_EVENT_QUEUE_SIZE, sizeof(esp_event_msg_t));
    s_rx_queue = xQueueCreate(TINYPAN_ESP_RX_QUEUE_SIZE, sizeof(esp_rx_msg_t));

    esp_err_t ret = esp_bt_l2cap_register_callback(esp_l2cap_cb);
    if (ret != ESP_OK) return -1;

    ret = esp_bt_l2cap_init();
    if (ret != ESP_OK) return -1;

    s_hal_initialized = true;
    return 0;
}

void hal_bt_deinit(void) {
    if (!s_hal_initialized) return;

    if (s_is_connected) {
        esp_bt_l2cap_close(s_l2cap_handle);
    }
    
    esp_bt_l2cap_deinit();
    
    if (s_event_queue) vQueueDelete(s_event_queue);
    if (s_rx_queue) vQueueDelete(s_rx_queue);
    
    s_hal_initialized = false;
}

int hal_bt_l2cap_connect(const uint8_t* remote_addr, uint16_t local_mtu) {
    (void)local_mtu;
    esp_err_t ret = esp_bt_l2cap_connect((uint8_t*)remote_addr, ESP_BT_PSM_BNEP, 0);
    return (ret == ESP_OK) ? 0 : -1;
}

void hal_bt_l2cap_disconnect(void) {
    if (s_is_connected) {
        esp_bt_l2cap_close(s_l2cap_handle);
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

bool hal_bt_l2cap_can_send(void) {
    return s_is_connected && !s_tx_busy;
}

void hal_bt_l2cap_request_can_send_now(void) {
    if (s_is_connected) {
        esp_event_msg_t msg;
        msg.event_id = HAL_L2CAP_EVENT_CAN_SEND_NOW;
        msg.status = 0;
        xQueueSend(s_event_queue, &msg, 0);
    }
}

int hal_bt_l2cap_send(const uint8_t* data, uint16_t len) {
    if (!s_is_connected) return -1;
    if (s_tx_busy) return 1;

    /* Bounce to aligned driver buffer if needed */
    if (((uintptr_t)data & 3) != 0) {
        if (len > sizeof(s_tx_aligned_buf)) return -1;
        memcpy(s_tx_aligned_buf, data, len);
        esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, s_tx_aligned_buf, len);
        return (ret == ESP_OK) ? 0 : -1;
    }

    esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, (uint8_t*)data, len);
    return (ret == ESP_OK) ? 0 : (ret == ESP_ERR_NO_MEM ? 1 : -1);
}

int hal_bt_l2cap_send_iovec(const tinypan_iovec_t* iov, uint16_t iov_count) {
    if (!s_is_connected) return -1;
    if (s_tx_busy) return 1;

    uint32_t total_len = 0;
    for (uint16_t i = 0; i < iov_count; i++) {
        total_len += iov[i].iov_len;
    }

    if (total_len > sizeof(s_tx_aligned_buf)) {
        return -1;
    }

    /* Bounce gather mapping to contiguous aligned buffer for ESP-IDF */
    uint32_t offset = 0;
    for (uint16_t i = 0; i < iov_count; i++) {
        memcpy(s_tx_aligned_buf + offset, iov[i].iov_base, iov[i].iov_len);
        offset += iov[i].iov_len;
    }

    esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, s_tx_aligned_buf, total_len);
    return (ret == ESP_OK) ? 0 : (ret == ESP_ERR_NO_MEM ? 1 : -1);
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
