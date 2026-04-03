/**
 * @file tinypan_hal_zephyr.c
 * @brief Reference Hardware Abstraction Layer for Zephyr RTOS (BLE SLIP Mode)
 * 
 * This file implements the TinyPAN `tinypan_hal.h` interface for a Zephyr
 * RTOS device (e.g. nRF52) running the Nordic UART Service (NUS).
 * 
 * Target: Custom Companion App via BLE SLIP (`TINYPAN_USE_BLE_SLIP=1`)
 * 
 * @note Thread Safety
 * Zephyr's Bluetooth RX callbacks occur in the system workqueue or BT RX
 * thread context. Like the ESP-IDF port, we must safely bounce these events 
 * to the thread calling `tinypan_process()` using `k_msgq` (Message Queues)
 * and protect shared transport state using the hal_mutex_x API.
 */

#include "tinypan_hal.h"
#include "tinypan_config.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>

#if !TINYPAN_USE_BLE_SLIP
#error "This HAL requires TINYPAN_USE_BLE_SLIP=1 in tinypan_config.h"
#endif

/* ============================================================================
 * State & Sync Mechanisms
 * ============================================================================ */

static hal_l2cap_recv_callback_t s_recv_cb = NULL;
static void* s_recv_cb_data = NULL;

static hal_l2cap_event_callback_t s_event_cb = NULL;
static void* s_event_cb_data = NULL;

static bool s_hal_initialized = false;
static struct bt_conn *s_current_conn = NULL;

static void (*s_wakeup_cb)(void*) = NULL;
static void* s_wakeup_cb_data = NULL;

/* Message definitions for Zephyr k_msgq */
struct z_event_msg {
    int event_id;
    int status;
};

K_MSGQ_DEFINE(s_zephyr_event_q, sizeof(struct z_event_msg), 16, 4);

/* Concurrency: Lock-free Zephyr Ring Buffer for RX byte stream.
 * Size set to 2KB to handle full 1500-byte IP bursts. */
RING_BUF_DECLARE(s_rx_ringbuf, 2048);

/* SLIP TX Chunker */
static struct k_spinlock s_tx_lock;
static bool s_tx_notify_pending = false;
static uint32_t s_tx_retry_time = 0;

/* ============================================================================
 * Internal Zephyr Task (The Bridge)
 * ============================================================================ */

void hal_bt_poll(void) {
    if (!s_hal_initialized) return;

    /* 1. Drain incoming Connection/Disconnection events */
    struct z_event_msg evt_msg;
    while (k_msgq_get(&s_zephyr_event_q, &evt_msg, K_NO_WAIT) == 0) {
        if (s_event_cb) {
            s_event_cb((hal_l2cap_event_t)evt_msg.event_id, evt_msg.status, s_event_cb_data);
        }
    }

    /* 2. Drain incoming BLE UART byte stream */
    uint8_t temp_buf[256];
    uint32_t read_len;
    while ((read_len = ring_buf_get(&s_rx_ringbuf, temp_buf, sizeof(temp_buf))) > 0) {
        if (s_recv_cb) {
            s_recv_cb(temp_buf, read_len, s_recv_cb_data);
        }
    }

    /* 3. Drain pending CAN_SEND_NOW events */
    k_spinlock_key_t key = k_spin_lock(&s_tx_lock);
    if (s_current_conn && s_tx_notify_pending) {
        if (k_uptime_get() >= s_tx_retry_time) {
            s_tx_notify_pending = false;
            k_spin_unlock(&s_tx_lock, key);
            if (s_event_cb) {
                s_event_cb(HAL_L2CAP_EVENT_CAN_SEND_NOW, 0, s_event_cb_data);
            }
        } else {
            k_spin_unlock(&s_tx_lock, key);
        }
    } else {
        k_spin_unlock(&s_tx_lock, key);
    }
}

/* ============================================================================
 * Zephyr BT Callbacks
 * ============================================================================ */

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        printk("Connection failed (err 0x%02x)\n", err);
        return;
    }
    
    if (s_current_conn) return;

    s_current_conn = bt_conn_ref(conn);
    
    struct z_event_msg msg;
    msg.event_id = HAL_L2CAP_EVENT_CONNECTED;
    msg.status = 0;
    if (k_msgq_put(&s_zephyr_event_q, &msg, K_NO_WAIT) != 0) {
        bt_conn_disconnect(s_current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(s_current_conn);
        s_current_conn = NULL;
    }

    if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    if (s_current_conn) {
        bt_conn_unref(s_current_conn);
        s_current_conn = NULL;
    }
    
    struct z_event_msg msg;
    msg.event_id = HAL_L2CAP_EVENT_DISCONNECTED;
    msg.status = reason;
    k_msgq_put(&s_zephyr_event_q, &msg, K_NO_WAIT);

    if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data,
                          uint16_t len) {
    uint32_t wrote = ring_buf_put(&s_rx_ringbuf, data, len);
    if (wrote < len) {
        printk("Zephyr RX ringbuf full, dropped bytes\n");
    }

    if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
}

static struct bt_nus_cb nus_cb = {
    .received = bt_receive_cb,
};

/* ============================================================================
 * HAL Implementation
 * ============================================================================ */

int hal_bt_init(void) {
    if (s_hal_initialized) return 0;

    int err = bt_enable(NULL);
    if (err) return -1;

    err = bt_nus_init(&nus_cb);
    if (err) return -1;

    s_hal_initialized = true;
    return 0;
}

void hal_bt_deinit(void) {
    if (!s_hal_initialized) return;
    if (s_current_conn) {
        bt_conn_disconnect(s_current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    s_hal_initialized = false;
}

int hal_bt_l2cap_connect(const uint8_t* remote_addr, uint16_t local_mtu) {
    (void)remote_addr;
    (void)local_mtu;
    return 0; 
}

void hal_bt_l2cap_disconnect(void) {
    if (s_current_conn) {
        bt_conn_disconnect(s_current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
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

bool hal_bt_l2cap_can_send(void) {
    return (s_current_conn != NULL);
}

void hal_bt_l2cap_request_can_send_now(void) {
    k_spinlock_key_t key = k_spin_lock(&s_tx_lock);
    if (s_current_conn) {
        s_tx_notify_pending = true;
        s_tx_retry_time = k_uptime_get() + 5; /* 5ms backoff */
    }
    k_spin_unlock(&s_tx_lock, key);
}

int hal_bt_l2cap_send(const uint8_t* data, uint16_t len) {
    if (!s_current_conn) return -1;
    int err = bt_nus_send(s_current_conn, data, len);
    if (err == -ENOMEM) return 1;
    else if (err < 0) return -1;
    return 0;
}

int hal_bt_l2cap_send_iovec(const tinypan_iovec_t* iov, uint16_t iov_count) {
    if (!s_current_conn) return -1;
    
    /* Zephyr NUS doesn't have a native scatter-gather API.
     * We must bounce into a contiguous buffer. This is NOT zero-copy,
     * but it implements the HAL interface required for BNEP mode. */
    static uint8_t s_tx_bounce_buf[TINYPAN_L2CAP_MTU + 32];
    uint16_t total_len = 0;
    
    for (uint16_t i = 0; i < iov_count; i++) {
        if (total_len + iov[i].iov_len > sizeof(s_tx_bounce_buf)) return -1;
        memcpy(&s_tx_bounce_buf[total_len], iov[i].iov_base, iov[i].iov_len);
        total_len += iov[i].iov_len;
    }
    
    int err = bt_nus_send(s_current_conn, s_tx_bounce_buf, total_len);
    if (err == -ENOMEM) return 1;
    else if (err < 0) return -1;
    
    return 0;
}

void hal_get_local_bd_addr(uint8_t addr[HAL_BD_ADDR_LEN]) {
    bt_addr_le_t target_addr;
    size_t count = 1;
    bt_id_get(&target_addr, &count);
    if (count > 0) {
        for (int i = 0; i < 6; i++) {
            addr[i] = target_addr.a.val[5 - i];
        }
    } else {
        memset(addr, 0, HAL_BD_ADDR_LEN);
    }
}

uint32_t hal_get_tick_ms(void) {
    return (uint32_t)k_uptime_get();
}

uint32_t hal_bt_get_next_timeout_ms(void) {
    if (s_tx_notify_pending) {
        uint32_t now = k_uptime_get();
        if (now < s_tx_retry_time) {
            return (s_tx_retry_time - now);
        }
        return 0;
    }
    return 0xFFFFFFFF;
}

uint16_t hal_bt_l2cap_get_mtu(void) {
    if (s_current_conn) {
        /* Return the negotiated GATT MTU minus the 3-byte ATT header.
         * This prevents 'EMSGSIZE' errors when calling bt_nus_send() 
         * on connections with legacy 23-byte MTUs. */
        return bt_gatt_get_mtu(s_current_conn) - 3;
    }
    return 20; /* BLE default */
}

hal_mutex_t hal_mutex_create(void) {
    struct k_mutex* mutex = malloc(sizeof(struct k_mutex));
    if (mutex) k_mutex_init(mutex);
    return (hal_mutex_t)mutex;
}

void hal_mutex_lock(hal_mutex_t mutex) {
    if (mutex) k_mutex_lock((struct k_mutex*)mutex, K_FOREVER);
}

void hal_mutex_unlock(hal_mutex_t mutex) {
    if (mutex) k_mutex_unlock((struct k_mutex*)mutex);
}

void hal_mutex_destroy(hal_mutex_t mutex) {
    if (mutex) free(mutex);
}
