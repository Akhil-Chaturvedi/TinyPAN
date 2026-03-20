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
 * to the thread calling `tinypan_process()` using `k_msgq` (Message Queues).
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

/* NUS delivers a raw byte stream. We copy incoming bytes into a lock-free
   ring buffer so the BLE callback returns immediately without blocking. */
K_MSGQ_DEFINE(s_zephyr_event_q, sizeof(struct z_event_msg), 16, 4);

/* Concurrency: Lock-free Zephyr Ring Buffer replaces K_MUTEX_DEFINE to prevent stalls.
 * Size is increased to 4KB to handle TCP window bursts and prevent overflows
 * during high-throughput transfers if the app thread is busy. */
RING_BUF_DECLARE(s_rx_ringbuf, 4096);

/* SLIP TX Chunker */
/* TinyPAN passes ~1500 byte SLIP MTU frames. NUS must chunk them to BLE MTU */
static bool s_tx_notify_pending = false;
static uint32_t s_tx_retry_time = 0;

/* ============================================================================
 * Internal Zephyr Task (The Bridge)
 * ============================================================================ */

/**
 * @brief Platform-specific polling implementation for Zephyr.
 */
void hal_bt_poll(void) {
    if (!s_hal_initialized) return;

    /* 1. Drain incoming Connection/Disconnection events */
    struct z_event_msg evt_msg;
    while (k_msgq_get(&s_zephyr_event_q, &evt_msg, K_NO_WAIT) == 0) {
        if (s_event_cb) {
            s_event_cb((hal_l2cap_event_t)evt_msg.event_id, evt_msg.status, s_event_cb_data);
        }
    }

    /* 2. Drain incoming BLE UART byte stream completely.
     * ring_buf_get returns up to sizeof(temp_buf) bytes per call. Loop until
     * the ring buffer is empty so a burst larger than 256 bytes is not held
     * over to the next poll cycle. */
    uint8_t temp_buf[256];
    uint32_t read_len;
    while ((read_len = ring_buf_get(&s_rx_ringbuf, temp_buf, sizeof(temp_buf))) > 0) {
        if (s_recv_cb) {
            s_recv_cb(temp_buf, read_len, s_recv_cb_data);
        }
    }

    /* 3. Drain pending CAN_SEND_NOW events */
    if (s_current_conn && s_tx_notify_pending) {
        if (k_uptime_get() >= s_tx_retry_time) {
            s_tx_notify_pending = false;
            /* QA-18: Fire directly to avoid polling-lag jitter */
            if (s_event_cb) {
                s_event_cb(HAL_L2CAP_EVENT_CAN_SEND_NOW, 0, s_event_cb_data);
            }
        }
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

    printk("Connected\n");
    s_current_conn = bt_conn_ref(conn);
    
    struct z_event_msg msg;
    msg.event_id = HAL_L2CAP_EVENT_CONNECTED;
    msg.status = 0;
    if (k_msgq_put(&s_zephyr_event_q, &msg, K_NO_WAIT) != 0) {
        /* Queue full: supervisor cannot learn of this connection.
         * Force-disconnect to restore a clean state. */
        printk("Event queue full on CONNECTED; force-disconnecting\n");
        bt_conn_disconnect(s_current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(s_current_conn);
        s_current_conn = NULL;
    }

    if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    printk("Disconnected (reason 0x%02x)\n", reason);

    if (s_current_conn) {
        bt_conn_unref(s_current_conn);
        s_current_conn = NULL;
    }
    
    struct z_event_msg msg;
    msg.event_id = HAL_L2CAP_EVENT_DISCONNECTED;
    msg.status = reason;
    if (k_msgq_put(&s_zephyr_event_q, &msg, K_NO_WAIT) != 0) {
        /* Queue full: supervisor cannot learn of this disconnect.
         * The supervisor's state timeout will eventually recover. */
        printk("Event queue full on DISCONNECTED; supervisor will timeout\n");
    }

    if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data,
                          uint16_t len) {
    /* Concurrency: Lock-free put directly from BLE RX thread */
    uint32_t wrote = ring_buf_put(&s_rx_ringbuf, data, len);
    if (wrote < len) {
        printk("Zephyr RX ringbuf full, dropped %lu bytes\n", (unsigned long)(len - wrote));
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
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return -1;
    }

    err = bt_nus_init(&nus_cb);
    if (err) {
        printk("Failed to initialize UART service (err: %d)\n", err);
        return -1;
    }

    /* Note: Advertising setup is typically handled by the application, 
       but for completeness in initialization: */
    /* bt_le_adv_start(...) */

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
    /* In BLE SLIP mode, the MCU is usually the Peripheral server advertising NUS.
       The connection is initiated by the Phone. Hence, this connect call is a no-op 
       because hal_bt_init() starts advertising and we just wait for `connected` 
       callback. */
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
    if (s_current_conn) {
        s_tx_notify_pending = true;
        s_tx_retry_time = k_uptime_get() + 5; /* 5ms backoff */
    }
}

int hal_bt_l2cap_send(const uint8_t* data, uint16_t len) {
    if (!s_current_conn) return -1;

    int err = bt_nus_send(s_current_conn, data, len);
    if (err == -ENOMEM) {
        return 1;
    } else if (err < 0) {
        printk("bt_nus_send failed: %d\n", err);
        return -1;
    }

    return 0;
}

int hal_bt_l2cap_send_iovec(const tinypan_iovec_t* iov, uint16_t iov_count) {
    (void)iov;
    (void)iov_count;
    /* SLIP over BLE NUS does not natively support BNEP scatter-gather. */
    return -1;
}

void hal_get_local_bd_addr(uint8_t addr[HAL_BD_ADDR_LEN]) {
    /* Zephyr: Retrieve identity address */
    bt_addr_le_t target_addr;
    size_t count = 1;
    bt_id_get(&target_addr, &count);
    if (count > 0) {
        /* Zephyr addresses are little-endian, reverse them for TinyPAN (big-endian) */
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
        return 0; /* Poll immediately */
    }
    return 0xFFFFFFFF;
}

uint16_t hal_bt_l2cap_get_mtu(void) {
    if (!s_current_conn) return 23; /* GATT Base MTU */
    return bt_gatt_get_mtu(s_current_conn) - 3; /* GATT Overhead */
}
