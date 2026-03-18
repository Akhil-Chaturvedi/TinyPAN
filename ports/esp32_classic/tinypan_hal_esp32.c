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
    struct pbuf* p;
} esp_rx_msg_t;

/* Static bounce buffer for DMA alignment */
static uint8_t s_tx_aligned_buf[2048] __attribute__((aligned(4)));

static QueueHandle_t s_event_queue = NULL;
static QueueHandle_t s_rx_queue = NULL;

/* ============================================================================
 * Internal FreeRTOS Task (The Bridge)
 * ============================================================================ */

/**
 * @brief Platform-specific polling implementation.
 * Drains the FreeRTOS queues (filled by the internal BT task)
 * and safely invokes the TinyPAN HAL callbacks on the user's thread.
 */
void hal_bt_poll(void) {
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
        if (s_recv_cb && rx_msg.p) {
            /* Pass the pbuf's contiguous payload directly. 
             * PBUF_POOL pbufs are usually contiguous if the stack gives them to us. */
            s_recv_cb((uint8_t*)rx_msg.p->payload, rx_msg.p->len, s_recv_cb_data);
            pbuf_free(rx_msg.p);
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
             * Instead of a 1.5KB BSS buffer, we allocate a pbuf from the lwIP pool. */
            if (param->data_ind.len > 0 && param->data_ind.data) {
                struct pbuf* p = pbuf_alloc(PBUF_RAW, param->data_ind.len, PBUF_POOL);
                if (p != NULL) {
                    pbuf_take(p, param->data_ind.data, param->data_ind.len);
                    esp_rx_msg_t rx_msg = { .p = p };
                    if (xQueueSend(s_rx_queue, &rx_msg, 0) != pdTRUE) {
                        pbuf_free(p);
                        ESP_LOGW(TAG, "RX Queue full, dropped inbound L2CAP frame");
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to allocate pbuf for RX");
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
    if (s_tx_busy) return 1;

    /* CONDITIONAL BOUNCE: 
     * If unaligned or non-contiguous (handled here by simple uint8*),
     * we use the s_tx_aligned_buf. */
    if (((uintptr_t)data & 3) != 0) {
        if (len > sizeof(s_tx_aligned_buf)) return -1;
        memcpy(s_tx_aligned_buf, data, len);
        esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, s_tx_aligned_buf, len);
        return (ret == ESP_OK) ? 0 : -1;
    }

    esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, (uint8_t*)data, len);
    return (ret == ESP_OK) ? 0 : (ret == ESP_ERR_NO_MEM ? 1 : -1);
}

int hal_bt_l2cap_send_pbuf(struct pbuf* p) {
    if (!s_is_connected) return -1;
    if (s_tx_busy) return 1;
    if (p == NULL) return -1;

    /* If the chain is contiguous and aligned, we can send directly */
    if (p->next == NULL && ((uintptr_t)p->payload & 3) == 0) {
        esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, p->payload, p->len);
        return (ret == ESP_OK) ? 0 : (ret == ESP_ERR_NO_MEM ? 1 : -1);
    }

    /* Otherwise, we MUST bounce to s_tx_aligned_buf to ensure DMA safety */
    if (p->tot_len > sizeof(s_tx_aligned_buf)) {
        ESP_LOGE(TAG, "TX pbuf too large for bounce buffer: %u", p->tot_len);
        return -1;
    }

    pbuf_copy_partial(p, s_tx_aligned_buf, p->tot_len, 0);
    esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, s_tx_aligned_buf, p->tot_len);
    return (ret == ESP_OK) ? 0 : (ret == ESP_ERR_NO_MEM ? 1 : -1);
}

uint32_t hal_get_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
