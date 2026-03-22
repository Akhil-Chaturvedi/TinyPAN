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
 * a static lock-free ring buffer (`s_rx_ring_data`). Events (connect/disconnect)
 * are bridged via a small FreeRTOS queue. Memory barriers (`portMEMORY_BARRIER`)
 * guard head/tail index updates for correct multi-core visibility.
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

/* Used to align and pack unaligned or non-contiguous data for DMA safety */
static uint8_t s_tx_aligned_buf[TINYPAN_L2CAP_MTU + 32];

/* Static RX ring buffer: avoids malloc/free in the BT callback context,
 * preventing heap fragmentation on ESP32's split DRAM architecture.
 *
 * Static RAM cost: TINYPAN_ESP_RX_RING_SLOTS * TINYPAN_ESP_RX_SLOT_SIZE bytes.
 * At default values (4 slots, TINYPAN_L2CAP_MTU+16 = 1707 bytes each) this is
 * approximately 6.8 KB of BSS. Override either macro before including this file
 * to reduce footprint for applications where the network layer can tolerate
 * a smaller slot count or smaller maximum frame size:
 *
 *   #define TINYPAN_ESP_RX_RING_SLOTS  2    // 2 frames in-flight: ~3.4 KB
 *   #define TINYPAN_ESP_RX_SLOT_SIZE   512  // Smaller frames only: ~1 KB
 */
#ifndef TINYPAN_ESP_RX_RING_SLOTS
#define TINYPAN_ESP_RX_RING_SLOTS   4
#endif
#ifndef TINYPAN_ESP_RX_SLOT_SIZE
#define TINYPAN_ESP_RX_SLOT_SIZE    (TINYPAN_L2CAP_MTU + 16)
#endif

static uint8_t s_rx_ring_data[TINYPAN_ESP_RX_RING_SLOTS][TINYPAN_ESP_RX_SLOT_SIZE];
static uint16_t s_rx_ring_len[TINYPAN_ESP_RX_RING_SLOTS];
static volatile uint8_t s_rx_ring_head = 0; /* Written by BT task */
static volatile uint8_t s_rx_ring_tail = 0; /* Read by app task */

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
                    /* QA-23: Drop event but don't kill the hardware link. 
                     * The supervisor will eventually time out and recover. */
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
            /* Static Ring Buffer: Copy incoming data directly into a pre-allocated
             * slot, avoiding malloc/free in the BT controller task. */
            if (param->data_ind.len > 0 && param->data_ind.data) {
                uint8_t next_head = (s_rx_ring_head + 1) % TINYPAN_ESP_RX_RING_SLOTS;
                if (next_head != s_rx_ring_tail) {
                    uint16_t copy_len = (param->data_ind.len <= TINYPAN_ESP_RX_SLOT_SIZE)
                                        ? param->data_ind.len : TINYPAN_ESP_RX_SLOT_SIZE;
                    memcpy(s_rx_ring_data[s_rx_ring_head], param->data_ind.data, copy_len);
                    s_rx_ring_len[s_rx_ring_head] = copy_len;
                    
                    /* Atomic index update for multi-core safety */
                    portENTER_CRITICAL(&s_state_spinlock);
                    s_rx_ring_head = next_head;
                    portEXIT_CRITICAL(&s_state_spinlock);

                    if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
                } else {
                    ESP_LOGW(TAG, "RX ring full, dropped inbound L2CAP frame");
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

    /* Drain L2CAP data from static ring buffer */
    while (s_rx_ring_tail != s_rx_ring_head) {
        if (s_recv_cb) {
            s_recv_cb(s_rx_ring_data[s_rx_ring_tail],
                      s_rx_ring_len[s_rx_ring_tail],
                      s_recv_cb_data);
        }
        
        /* Atomic index update for multi-core safety */
        portENTER_CRITICAL(&s_state_spinlock);
        s_rx_ring_tail = (s_rx_ring_tail + 1) % TINYPAN_ESP_RX_RING_SLOTS;
        portEXIT_CRITICAL(&s_state_spinlock);
    }
}

/* ============================================================================
 * HAL Implementation
 * ============================================================================ */

int hal_bt_init(void) {
    if (s_hal_initialized) return 0;

    s_event_queue = xQueueCreate(TINYPAN_ESP_EVENT_QUEUE_SIZE, sizeof(esp_event_msg_t));

    /* Reset static RX ring buffer */
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
     * contiguous buffer. We bounce the BNEP iovec into a static aligned buffer 
     * here. While this adds a copy on ESP32, the transport layer provides 
     * zero-copy capability for DMA-native BLE stacks (e.g. nRF52). */
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
    
    /* QA-17: Defer completion to hal_bt_poll to prevent deep stack recursion
     * if the transport immediately tries to send the next packet. */
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
