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

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ESP-IDF Framework Headers */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_l2cap_bt_api.h"
#include "esp_timer.h"

/* The BNEP PSM standard port */
#define ESP_BT_PSM_BNEP                     0x000F

/* FreeRTOS Queue sizing */
#define TINYPAN_ESP_EVENT_QUEUE_SIZE        16
#define TINYPAN_ESP_RX_QUEUE_SIZE           8

static const char* TAG = "TinyPAN_HAL";

/* ============================================================================
 * State & Sync Mechanisms
 * ============================================================================ */

static hal_l2cap_recv_callback_t s_recv_cb = NULL;
static void* s_recv_cb_data = NULL;

static hal_l2cap_event_callback_t s_event_cb = NULL;
static void* s_event_cb_data = NULL;

static bool s_hal_initialized = false;
static uint32_t s_l2cap_handle = 0;
static bool s_is_connected = false;
static bool s_tx_busy = false;

typedef struct {
    int event_id;
    int status;
} esp_event_msg_t;

typedef struct {
    uint8_t* payload;
    uint16_t length;
} esp_rx_msg_t;

static QueueHandle_t s_event_queue = NULL;
static QueueHandle_t s_rx_queue = NULL;

/* ============================================================================
 * Internal FreeRTOS Task (The Bridge)
 * ============================================================================ */

/**
 * @brief The user MUST call this function from their `while(1)` polling loop
 *        alongside `tinypan_process()`.
 * 
 * This function drains the FreeRTOS queues (filled by the internal BT task)
 * and safely invokes the TinyPAN HAL callbacks on the user's thread.
 */
void tinypan_hal_esp32_poll(void) {
    if (!s_hal_initialized) return;

    /* 1. Drain incoming L2CAP connection/disconnection events */
    esp_event_msg_t evt_msg;
    while (xQueueReceive(s_event_queue, &evt_msg, 0) == pdTRUE) {
        if (s_event_cb) {
            s_event_cb((hal_l2cap_event_t)evt_msg.event_id, evt_msg.status, s_event_cb_data);
        }
    }

    /* 2. Drain incoming L2CAP data frames */
    esp_rx_msg_t rx_msg;
    while (xQueueReceive(s_rx_queue, &rx_msg, 0) == pdTRUE) {
        if (s_recv_cb && rx_msg.payload) {
            s_recv_cb(rx_msg.payload, rx_msg.length, s_recv_cb_data);
        }
        if (rx_msg.payload) {
            free(rx_msg.payload); /* Free the heap-copy made by the BT task */
        }
    }
}

/* ============================================================================
 * ESP-IDF L2CAP Callbacks (Executes in ESP BT Task context)
 * ============================================================================ */

static void esp_l2cap_cb(esp_bt_l2cap_cb_event_t event, esp_bt_l2cap_cb_param_t *param) {
    esp_event_msg_t msg;

    switch (event) {
        case ESP_BT_L2CAP_INIT_EVT:
            ESP_LOGI(TAG, "L2CAP initialized");
            break;

        case ESP_BT_L2CAP_CL_INIT_EVT:
            if (param->cl_init.status == ESP_BT_L2CAP_SUCCESS) {
                s_l2cap_handle = param->cl_init.handle;
                s_is_connected = true;
                s_tx_busy = false;
                ESP_LOGI(TAG, "L2CAP Connected. Handle: 0x%04lx", s_l2cap_handle);
                
                msg.event_id = HAL_L2CAP_EVENT_CONNECTED;
                msg.status = 0;
                xQueueSend(s_event_queue, &msg, 0);
            } else {
                ESP_LOGE(TAG, "L2CAP Connect Failed. Status: %d", param->cl_init.status);
                msg.event_id = HAL_L2CAP_EVENT_DISCONNECTED;
                msg.status = param->cl_init.status;
                xQueueSend(s_event_queue, &msg, 0);
            }
            break;

        case ESP_BT_L2CAP_CLOSE_EVT:
            ESP_LOGI(TAG, "L2CAP Disconnected");
            s_is_connected = false;
            s_l2cap_handle = 0;
            
            msg.event_id = HAL_L2CAP_EVENT_DISCONNECTED;
            msg.status = 0;
            xQueueSend(s_event_queue, &msg, 0);
            break;

        case ESP_BT_L2CAP_DATA_IND_EVT:
            /* Data arrived from the remote PAN device.
               We must copy it to the heap and queue it, as the p_data pointer
               belongs to the volatile BT task. */
            if (param->data_ind.len > 0 && param->data_ind.data) {
                esp_rx_msg_t rx_msg;
                rx_msg.length = param->data_ind.len;
                rx_msg.payload = malloc(rx_msg.length);
                if (rx_msg.payload) {
                    memcpy(rx_msg.payload, param->data_ind.data, rx_msg.length);
                    if (xQueueSend(s_rx_queue, &rx_msg, 0) != pdTRUE) {
                        free(rx_msg.payload); /* Queue full, drop */
                        ESP_LOGW(TAG, "RX Queue full, dropped inbound L2CAP frame");
                    }
                }
            }
            break;

        default:
            break;
    }
}

/* ============================================================================
 * HAL Implementation
 * ============================================================================ */

int hal_bt_init(void) {
    if (s_hal_initialized) return 0;

    s_event_queue = xQueueCreate(TINYPAN_ESP_EVENT_QUEUE_SIZE, sizeof(esp_event_msg_t));
    s_rx_queue = xQueueCreate(TINYPAN_ESP_RX_QUEUE_SIZE, sizeof(esp_rx_msg_t));

    /* Initialize the Classic BT L2CAP client */
    esp_err_t ret = esp_bt_l2cap_register_callback(esp_l2cap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register L2CAP callback: %s", esp_err_to_name(ret));
        return -1;
    }

    ret = esp_bt_l2cap_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init L2CAP: %s", esp_err_to_name(ret));
        return -1;
    }

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
    (void)local_mtu; /* ESP32 handles MTU internally during init config */
    
    ESP_LOGI(TAG, "Connecting to BDA: %02x:%02x:%02x:%02x:%02x:%02x",
             remote_addr[0], remote_addr[1], remote_addr[2],
             remote_addr[3], remote_addr[4], remote_addr[5]);

    /* Connect to the standard BNEP PSM */
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
    /* Currently the ESP-IDF API does not expose a synchronous "can send"
       queue depth monitor for the underlying controller. We assume true. */
    return s_is_connected && !s_tx_busy;
}

void hal_bt_l2cap_request_can_send_now(void) {
    /* Since we pretend it's always ready, just schedule an artificial event */
    if (s_is_connected) {
        esp_event_msg_t msg;
        msg.event_id = HAL_L2CAP_EVENT_CAN_SEND_NOW;
        msg.status = 0;
        xQueueSend(s_event_queue, &msg, 0);
    }
}

int hal_bt_l2cap_send(const uint8_t* data, uint16_t len) {
    if (!s_is_connected) return -1;
    if (s_tx_busy) return TINYPAN_ERR_BUSY;

    /* WARNING: Zero-Copy Pointer Alignment!
       TinyPAN's fast-path prepends the BNEP header in-place, meaning the 
       `data` pointer here is highly likely to be unaligned (e.g. at an odd byte offset).
       On the ESP32 (Xtensa or RISC-V), the CPU supports unaligned access, but 
       underlying DMA hardware might reject it.
       If the ESP-IDF Bluedroid send function crashes, you must bounce this buffer.
       For safety in this reference port, we will conditionally bounce it. */
       
    if (((uintptr_t)data & 3) != 0) {
        /* Pointer is unaligned (not on a 4-byte boundary).
           Bounce it through an aligned heap allocation to prevent DMA HardFaults. */
        uint8_t* aligned_buf = malloc(len);
        if (!aligned_buf) return -1;
        memcpy(aligned_buf, data, len);
        
        /* The ESP-IDF l2cap API takes ownership and will eventually free this array?
           Actually, esp_bt_l2cap_vfs_send takes a copy. We must structure it correctly. */
        /* NOTE: the actual API is esp_bt_l2cap_vfs_send or esp_bt_l2cap_connect? 
           We use the standard ESP Bluedroid TX. */
        esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, aligned_buf, len);
        free(aligned_buf);
        return (ret == ESP_OK) ? 0 : -1;
    }

    /* Aligned pointer, send directly */
    esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, (uint8_t*)data, len);
    return (ret == ESP_OK) ? 0 : (ret == ESP_ERR_NO_MEM ? TINYPAN_ERR_BUSY : -1);
}

uint32_t hal_get_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
