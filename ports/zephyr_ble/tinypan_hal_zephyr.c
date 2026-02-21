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

/* Message definitions for Zephyr k_msgq */
struct z_event_msg {
    int event_id;
    int status;
};

/* Because Zephyr NUS gives us a stream of bytes, we just copy them into a 
   flat ring buffer protected by a mutex, rather than allocating blocks.
   The core TinyPAN `s_rx_queue` will eventually absorb it. */
K_MSGQ_DEFINE(s_zephyr_event_q, sizeof(struct z_event_msg), 16, 4);
K_MUTEX_DEFINE(s_zephyr_rx_mutex);

static uint8_t s_intermediate_rx[1024];
static uint16_t s_int_rx_head = 0;
static uint16_t s_int_rx_tail = 0;

/* ============================================================================
 * Internal Zephyr Task (The Bridge)
 * ============================================================================ */

/**
 * @brief The user MUST call this function from their `while(1)` polling loop
 *        alongside `tinypan_process()`.
 */
void tinypan_hal_zephyr_poll(void) {
    if (!s_hal_initialized) return;

    /* 1. Drain incoming Connection/Disconnection events */
    struct z_event_msg evt_msg;
    while (k_msgq_get(&s_zephyr_event_q, &evt_msg, K_NO_WAIT) == 0) {
        if (s_event_cb) {
            s_event_cb((hal_l2cap_event_t)evt_msg.event_id, evt_msg.status, s_event_cb_data);
        }
    }

    /* 2. Drain incoming BLE UART byte stream */
    k_mutex_lock(&s_zephyr_rx_mutex, K_FOREVER);
    
    /* If there is data in the intermediate ring buffer, feed it to TinyPAN */
    if (s_int_rx_head != s_int_rx_tail && s_recv_cb) {
        /* To keep it simple, we extract into a linear block to feed the callback.
           This is slightly sub-optimal vs direct circular reading, but TinyPAN's
           input expects a linear array anyway. */
        uint8_t temp_buf[256];
        uint16_t copied = 0;
        
        while (s_int_rx_head != s_int_rx_tail && copied < sizeof(temp_buf)) {
            temp_buf[copied++] = s_intermediate_rx[s_int_rx_tail];
            s_int_rx_tail = (s_int_rx_tail + 1) % sizeof(s_intermediate_rx);
        }
        
        k_mutex_unlock(&s_zephyr_rx_mutex);
        
        /* Deliver to TinyPAN (which will feed it into its own SLIP queue) */
        s_recv_cb(temp_buf, copied, s_recv_cb_data);
    } else {
        k_mutex_unlock(&s_zephyr_rx_mutex);
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
    k_msgq_put(&s_zephyr_event_q, &msg, K_NO_WAIT);
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
    k_msgq_put(&s_zephyr_event_q, &msg, K_NO_WAIT);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data,
                          uint16_t len) {
    /* Write to intermediate ring buffer */
    k_mutex_lock(&s_zephyr_rx_mutex, K_FOREVER);
    
    for (uint16_t i = 0; i < len; i++) {
        uint16_t next_head = (s_int_rx_head + 1) % sizeof(s_intermediate_rx);
        if (next_head == s_int_rx_tail) {
            printk("Zephyr intermediate RX full!\n");
            break; /* Drop */
        }
        s_intermediate_rx[s_int_rx_head] = data[i];
        s_int_rx_head = next_head;
    }
    
    k_mutex_unlock(&s_zephyr_rx_mutex);
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

bool hal_bt_l2cap_can_send(void) {
    /* NUS tx buffer pooling in Zephyr doesn't have a reliable synchronous depth block.
       Assume true and let bt_nus_send return an error if busy. */
    return s_current_conn != NULL;
}

void hal_bt_l2cap_request_can_send_now(void) {
    if (s_current_conn) {
        struct z_event_msg msg;
        msg.event_id = HAL_L2CAP_EVENT_CAN_SEND_NOW;
        msg.status = 0;
        k_msgq_put(&s_zephyr_event_q, &msg, K_NO_WAIT);
    }
}

int hal_bt_l2cap_send(const uint8_t* data, uint16_t len) {
    if (!s_current_conn) return -1;

    /* Because BLE MTU is small (e.g. 23 to 247 bytes), the SLIP frame may exceed
       a single NUS notification. We must chunk it. */
    
    int err = bt_nus_send(s_current_conn, data, len);
    if (err == -ENOMEM) {
        return TINYPAN_ERR_BUSY;
    } else if (err) {
        printk("Failed to send data over BLE NUS: %d\n", err);
        return -1;
    }
    return 0;
}

uint32_t hal_get_tick_ms(void) {
    return (uint32_t)k_uptime_get();
}
