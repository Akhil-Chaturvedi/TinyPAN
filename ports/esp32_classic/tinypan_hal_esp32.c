/**
 * @file tinypan_hal_esp32.c
 * @brief Hardware Abstraction Layer for ESP-IDF (Bluetooth Classic / BNEP)
 *
 * This file implements the TinyPAN `tinypan_hal.h` interface for an ESP32
 * running the official ESP-IDF framework (v5.5+) with the Bluedroid host stack.
 *
 * Target: PAN Client (BNEP) over Classic Bluetooth (BR/EDR).
 *
 * ## Architecture: VFS-to-Callback Bridge
 *
 * ESP-IDF's L2CAP API uses a VFS (Virtual File System) model where data I/O
 * is performed via POSIX `read()`/`write()`/`close()` on a file descriptor
 * obtained from the `ESP_BT_L2CAP_OPEN_EVT` callback. This HAL bridges that
 * model to TinyPAN's callback-based interface:
 *
 *   - **RX Path**: A dedicated FreeRTOS task blocks on `read(fd, ...)`.
 *     Received data is pushed into a `MessageBuffer`, which is drained by
 *     `hal_bt_poll()` in the application task context.
 *   - **TX Path**: `hal_bt_l2cap_send()` / `hal_bt_l2cap_send_iovec()` use
 *     POSIX `write(fd, ...)` directly. Congestion is detected synchronously
 *     from the `write()` return value (0 = ring buffer full).
 *   - **Events**: L2CAP callbacks dispatch events to a FreeRTOS queue,
 *     drained by `hal_bt_poll()`.
 *
 * @note Thread Safety
 * The ESP-IDF Bluetooth stack executes all callbacks on a dedicated internal
 * FreeRTOS task (`btu_task`). TinyPAN is strictly single-threaded. Incoming
 * data and events are bridged to the application thread via FreeRTOS
 * synchronization primitives (MessageBuffer, Queue). Cross-task state is
 * protected by `s_state_spinlock` (portENTER_CRITICAL_SAFE).
 */

#include "tinypan_hal.h"
#include "tinypan_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/message_buffer.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_l2cap_bt_api.h>
#include <esp_timer.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void esp_l2cap_cb(esp_bt_l2cap_cb_event_t event,
                          esp_bt_l2cap_cb_param_t *param);
static void can_send_timer_cb(TimerHandle_t timer);
static bool start_rx_reader_task(int fd);
static void signal_rx_reader_stop(void);
static void stop_rx_reader_task(void);
static void rx_reader_task_fn(void* arg);

/* ============================================================================
 * Constants
 * ============================================================================ */

static const char* TAG = "TinyPAN_HAL";

/** EventGroup bit for RX reader task exit signaling. */
#define RX_EXIT_BIT (1 << 0)

/** RX reader task stack size (bytes). */
#ifndef TINYPAN_ESP_RX_TASK_STACK
#define TINYPAN_ESP_RX_TASK_STACK   4096
#endif

/** RX reader task priority. */
#ifndef TINYPAN_ESP_RX_TASK_PRIO
#define TINYPAN_ESP_RX_TASK_PRIO    (configMAX_PRIORITIES - 3)
#endif

/** Delay before CAN_SEND_NOW timer fires (milliseconds). */
#ifndef TINYPAN_CAN_SEND_DELAY_MS
#define TINYPAN_CAN_SEND_DELAY_MS   10
#endif

/* ============================================================================
 * State & Sync Mechanisms
 * ============================================================================ */

/* --- Callbacks (set from app task, read from BTU/task contexts) --- */
static hal_l2cap_recv_callback_t s_recv_cb = NULL;
static void* s_recv_cb_data = NULL;

static hal_l2cap_event_callback_t s_event_cb = NULL;
static void* s_event_cb_data = NULL;

static void (*s_wakeup_cb)(void*) = NULL;
static void* s_wakeup_cb_data = NULL;

/* --- Initialization flag --- */
static bool s_hal_initialized = false;

/* --- Cross-task state (protected by s_state_spinlock) --- */
static portMUX_TYPE s_state_spinlock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_is_connected = false;
static volatile int s_l2cap_fd = -1;           /* VFS file descriptor (-1 = invalid) */
static bool s_tx_complete_pending = false;

/* --- Negotiated MTU from OPEN_EVT --- */
static uint16_t s_negotiated_mtu = TINYPAN_L2CAP_MTU;

/* --- TX aligned bounce buffer for scatter-gather concatenation --- */
static uint32_t s_tx_aligned_buf_raw[(TINYPAN_L2CAP_MTU + 3) / 4];
#define s_tx_aligned_buf ((uint8_t*)s_tx_aligned_buf_raw)

/* --- RX bridge: Reader task → MessageBuffer → app task --- */
#ifndef TINYPAN_ESP_RX_MSG_BUF_SIZE
#define TINYPAN_ESP_RX_MSG_BUF_SIZE   8192
#endif

static MessageBufferHandle_t s_rx_msg_buf = NULL;
static uint8_t s_rx_poll_temp_buf[TINYPAN_L2CAP_MTU];

/* --- RX reader task --- */
static TaskHandle_t s_rx_reader_task = NULL;
static volatile bool s_rx_reader_running = false;
static uint8_t s_rx_reader_buf[TINYPAN_L2CAP_MTU];  /* Static read buffer */

/* --- EventGroup for RX reader task exit synchronization --- */
static EventGroupHandle_t s_rx_exit_event = NULL;

/* --- One-shot timer for deferred CAN_SEND_NOW events --- */
static TimerHandle_t s_can_send_timer = NULL;

/* --- Event queue (BTU callback → app task) --- */
typedef struct {
    int event_id;
    int status;
} esp_event_msg_t;

static QueueHandle_t s_event_queue = NULL;

/* ============================================================================
 * CAN_SEND Timer Callback
 * ============================================================================ */

/**
 * @brief One-shot timer callback for deferred CAN_SEND_NOW events.
 *
 * Runs in the FreeRTOS timer daemon context. Queues a CAN_SEND_NOW event
 * for the app task to pick up in hal_bt_poll(). Does NOT call s_wakeup_cb
 * because the timer daemon has limited stack and the wakeup callback's
 * thread-safety contract is undefined.
 */
static void can_send_timer_cb(TimerHandle_t timer) {
    (void)timer;
    esp_event_msg_t msg = { .event_id = HAL_L2CAP_EVENT_CAN_SEND_NOW, .status = 0 };
    xQueueSend(s_event_queue, &msg, 0);
}

/* ============================================================================
 * ESP-IDF L2CAP Callback
 * ============================================================================ */

/**
 * @brief L2CAP event callback — runs in BTU task context.
 *
 * Dispatches events to the app task via s_event_queue. Key events:
 *   - INIT_EVT: Triggers VFS registration.
 *   - OPEN_EVT: Connection established; stores fd, starts RX reader task.
 *   - CLOSE_EVT: Connection closed; stops RX reader, resets state.
 *   - CL_INIT_EVT: Connection initiated (NOT yet established).
 */
static void esp_l2cap_cb(esp_bt_l2cap_cb_event_t event,
                          esp_bt_l2cap_cb_param_t *param) {
    esp_event_msg_t event_msg = {0};

    switch (event) {
    case ESP_BT_L2CAP_INIT_EVT:
        ESP_LOGI(TAG, "L2CAP initialized, status: %d", param->init.status);
        if (param->init.status == ESP_BT_L2CAP_SUCCESS) {
            /* Register VFS immediately after successful init */
            esp_err_t ret = esp_bt_l2cap_vfs_register();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "VFS register failed: %s", esp_err_to_name(ret));
            }
        }
        break;

    case ESP_BT_L2CAP_VFS_REGISTER_EVT:
        ESP_LOGI(TAG, "L2CAP VFS registered, status: %d",
                 param->vfs_register.status);
        break;

    case ESP_BT_L2CAP_CL_INIT_EVT:
        /* Connection INITIATED (not yet established) — just log */
        ESP_LOGI(TAG, "L2CAP connection initiated, status: %d",
                 param->cl_init.status);
        if (param->cl_init.status != ESP_BT_L2CAP_SUCCESS) {
            /* Initiation failed — report to app */
            event_msg.event_id = HAL_L2CAP_EVENT_CONNECT_FAILED;
            event_msg.status = param->cl_init.status;
            xQueueSend(s_event_queue, &event_msg, 0);
            if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
        }
        break;

    case ESP_BT_L2CAP_OPEN_EVT:
        /* Connection ACTUALLY ESTABLISHED */
        if (param->open.status == ESP_BT_L2CAP_SUCCESS) {
            portENTER_CRITICAL_SAFE(&s_state_spinlock);
            s_l2cap_fd = param->open.fd;    /* Store the VFS file descriptor */
            s_is_connected = true;
            s_negotiated_mtu = (uint16_t)param->open.tx_mtu;  /* Store actual MTU */
            portEXIT_CRITICAL_SAFE(&s_state_spinlock);

            ESP_LOGI(TAG, "L2CAP connected, fd=%d, tx_mtu=%d",
                     param->open.fd, (int)s_negotiated_mtu);

            /* Start the RX reader task (abort connection if task creation fails) */
            if (!start_rx_reader_task(param->open.fd)) {
                ESP_LOGE(TAG, "RX reader task creation failed, aborting connection");
                /* Reset state inside critical section to prevent app task from
                 * seeing s_is_connected=true for a connection we're aborting.
                 * Close fd AFTER state reset so hal_bt_l2cap_send() can't write
                 * to a closed fd during the race window. */
                portENTER_CRITICAL_SAFE(&s_state_spinlock);
                s_is_connected = false;
                s_l2cap_fd = -1;
                s_negotiated_mtu = TINYPAN_L2CAP_MTU;
                portEXIT_CRITICAL_SAFE(&s_state_spinlock);
                close(param->open.fd);
                event_msg.event_id = HAL_L2CAP_EVENT_CONNECT_FAILED;
                event_msg.status = -1;
                xQueueSend(s_event_queue, &event_msg, 0);
                if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
                break;
            }

            event_msg.event_id = HAL_L2CAP_EVENT_CONNECTED;
            xQueueSend(s_event_queue, &event_msg, 0);
            if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
        } else {
            ESP_LOGE(TAG, "L2CAP open failed, status: %d",
                     param->open.status);
            event_msg.event_id = HAL_L2CAP_EVENT_CONNECT_FAILED;
            event_msg.status = param->open.status;
            xQueueSend(s_event_queue, &event_msg, 0);
            if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
        }
        break;

    case ESP_BT_L2CAP_CLOSE_EVT:
        /* Signal RX reader to stop (non-blocking — we're in BTU callback context) */
        signal_rx_reader_stop();

        portENTER_CRITICAL_SAFE(&s_state_spinlock);
        s_is_connected = false;
        s_l2cap_fd = -1;
        s_negotiated_mtu = TINYPAN_L2CAP_MTU;  /* Reset for next connection */
        portEXIT_CRITICAL_SAFE(&s_state_spinlock);

        event_msg.event_id = HAL_L2CAP_EVENT_DISCONNECTED;
        xQueueSend(s_event_queue, &event_msg, 0);
        if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
        break;

    default:
        ESP_LOGD(TAG, "Unhandled L2CAP event: %d", event);
        break;
    }
}

/* ============================================================================
 * RX Reader Task
 * ============================================================================ */

/**
 * @brief RX reader task — blocks on read(fd) and pushes data to MessageBuffer.
 *
 * ESP-IDF's VFS model requires actively calling read() to receive data.
 * This task runs in a dedicated FreeRTOS task and blocks on read(). When
 * data arrives, it pushes bytes into a MessageBuffer for the app task to
 * drain in hal_bt_poll().
 *
 * @param arg  File descriptor cast to void* (via intptr_t)
 */
static void rx_reader_task_fn(void* arg) {
    int fd = (int)(intptr_t)arg;

    ESP_LOGI(TAG, "RX reader started, fd=%d", fd);
    while (s_rx_reader_running) {
        /* Backpressure: don't read from fd if MessageBuffer can't hold a full frame.
         * This lets the ESP-IDF L2CAP ring buffer fill up, which triggers L2CAP
         * flow control to the remote device — preventing silent data drops. */
        size_t available = xMessageBufferSpacesAvailable(s_rx_msg_buf);
        if (available < TINYPAN_L2CAP_MTU + sizeof(size_t)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        ssize_t len = read(fd, s_rx_reader_buf, TINYPAN_L2CAP_MTU);
        if (len < 0) {
            /* Error — connection likely closed or fd invalid */
            ESP_LOGE(TAG, "RX reader: read error, errno=%d", errno);
            break;
        }
        if (len == 0) {
            /* ESP-IDF specific: read() returns 0 when NO DATA is available,
             * NOT when connection is closed (unlike standard POSIX). */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Push to MessageBuffer for app task to pick up in hal_bt_poll() */
        size_t sent = xMessageBufferSend(s_rx_msg_buf, s_rx_reader_buf, (size_t)len, pdMS_TO_TICKS(100));
        if (sent == (size_t)len) {
            if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
        } else {
            ESP_LOGW(TAG, "RX MessageBuffer full, dropped %d bytes", (int)len);
        }
    }

    /* Signal exit to stop_rx_reader_task() via EventGroup before self-deleting. */
    if (s_rx_exit_event) xEventGroupSetBits(s_rx_exit_event, RX_EXIT_BIT);
    s_rx_reader_running = false;
    ESP_LOGI(TAG, "RX reader exited");
    vTaskDelete(NULL);
}

/**
 * @brief Start the RX reader task for the given file descriptor.
 * @return true if task was created successfully, false on failure.
 *
 * Caller MUST handle failure (e.g., abort the connection).
 */
static bool start_rx_reader_task(int fd) {
    s_rx_reader_running = true;
    BaseType_t ret = xTaskCreate(rx_reader_task_fn, "tinypan_rx",
                TINYPAN_ESP_RX_TASK_STACK, (void*)(intptr_t)fd,
                TINYPAN_ESP_RX_TASK_PRIO, &s_rx_reader_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX reader task (heap exhausted?)");
        s_rx_reader_running = false;
        s_rx_reader_task = NULL;
        return false;
    }
    return true;
}

/**
 * @brief Non-blocking stop — safe to call from BTU callback context.
 *
 * Just signals the task to stop. The task exits on its own when:
 *   1. It sees s_rx_reader_running == false, OR
 *   2. read() returns < 0 because the fd was closed
 */
static void signal_rx_reader_stop(void) {
    s_rx_reader_running = false;
    /* Do NOT wait or block — the BTU task must not be delayed.
     * The task will clean up and call vTaskDelete(NULL) on its own. */
}

/**
 * @brief Blocking stop — call ONLY from app task context (e.g. hal_bt_deinit).
 *
 * Waits for the RX reader task to signal exit via EventGroup, with a timeout.
 */
static void stop_rx_reader_task(void) {
    s_rx_reader_running = false;
    if (s_rx_reader_task) {
        /* Wait for the task to signal exit via EventGroup.
         * The task sets RX_EXIT_BIT just before calling vTaskDelete(NULL). */
        if (s_rx_exit_event) {
            EventBits_t bits = xEventGroupWaitBits(s_rx_exit_event, RX_EXIT_BIT,
                                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(200));
            if (!(bits & RX_EXIT_BIT)) {
                ESP_LOGW(TAG, "RX reader didn't exit cleanly, force-deleting");
                vTaskDelete(s_rx_reader_task);
            }
        }
        s_rx_reader_task = NULL;
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

    /* Fire deferred completion events (Breaks recursion loop from app thread) */
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
    }
}

/* ============================================================================
 * HAL Implementation
 * ============================================================================ */

int hal_bt_init(void) {
    if (s_hal_initialized) return 0;

    s_event_queue = xQueueCreate(TINYPAN_ESP_EVENT_QUEUE_SIZE, sizeof(esp_event_msg_t));
    s_rx_msg_buf = xMessageBufferCreate(TINYPAN_ESP_RX_MSG_BUF_SIZE);
    s_l2cap_fd = -1;
    s_negotiated_mtu = TINYPAN_L2CAP_MTU;

    /* EventGroup for RX reader task exit signaling */
    s_rx_exit_event = xEventGroupCreate();
    if (!s_rx_exit_event) {
        ESP_LOGE(TAG, "Failed to create RX exit event group");
        goto cleanup;
    }

    /* Create one-shot timer for deferred CAN_SEND_NOW events */
    s_can_send_timer = xTimerCreate("tp_can", pdMS_TO_TICKS(TINYPAN_CAN_SEND_DELAY_MS),
                                     pdFALSE, NULL, can_send_timer_cb);
    if (!s_can_send_timer) {
        ESP_LOGE(TAG, "Failed to create CAN_SEND timer");
        goto cleanup;
    }

    esp_err_t ret = esp_bt_l2cap_register_callback(esp_l2cap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "L2CAP callback register failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = esp_bt_l2cap_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "L2CAP init failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* Note: VFS registration happens in the INIT_EVT callback, not here.
     * The INIT_EVT callback calls esp_bt_l2cap_vfs_register(). */

    s_hal_initialized = true;
    return 0;

cleanup:
    /* Clean up all allocated resources on any failure to prevent leaks.
     * hal_bt_init() can be safely retried after a failure. */
    if (s_can_send_timer) { xTimerDelete(s_can_send_timer, portMAX_DELAY); s_can_send_timer = NULL; }
    if (s_rx_exit_event) { vEventGroupDelete(s_rx_exit_event); s_rx_exit_event = NULL; }
    if (s_event_queue) { vQueueDelete(s_event_queue); s_event_queue = NULL; }
    if (s_rx_msg_buf) { vMessageBufferDelete(s_rx_msg_buf); s_rx_msg_buf = NULL; }
    return -1;
}

void hal_bt_deinit(void) {
    if (!s_hal_initialized) return;

    /* 1. Signal RX reader to stop (non-blocking) */
    s_rx_reader_running = false;

    /* 2. Close fd FIRST — this unblocks read() in the RX reader task,
     *    causing it to return -1 and exit cleanly. */
    portENTER_CRITICAL_SAFE(&s_state_spinlock);
    int fd = s_l2cap_fd;
    bool connected = s_is_connected;
    s_is_connected = false;
    s_l2cap_fd = -1;
    portEXIT_CRITICAL_SAFE(&s_state_spinlock);

    if (connected && fd >= 0) {
        close(fd);  /* POSIX close, NOT esp_bt_l2cap_close */
    }

    /* 3. NOW wait for RX reader task to exit (read() already returned error) */
    stop_rx_reader_task();

    /* 4. Clean up CAN_SEND timer */
    if (s_can_send_timer) {
        xTimerDelete(s_can_send_timer, portMAX_DELAY);
        s_can_send_timer = NULL;
    }

    /* Note: hal_bt_deinit() does not synchronize with ESP_BT_L2CAP_UNINIT_EVT.
     * This is acceptable because TinyPAN has no deinit/reinit lifecycle path. */
    esp_bt_l2cap_vfs_unregister();
    esp_bt_l2cap_deinit();

    if (s_event_queue) { vQueueDelete(s_event_queue); s_event_queue = NULL; }
    if (s_rx_msg_buf) { vMessageBufferDelete(s_rx_msg_buf); s_rx_msg_buf = NULL; }
    if (s_rx_exit_event) { vEventGroupDelete(s_rx_exit_event); s_rx_exit_event = NULL; }

    s_hal_initialized = false;
}

int hal_bt_l2cap_connect(const uint8_t remote_addr[HAL_BD_ADDR_LEN],
                          uint16_t psm, uint16_t local_mtu) {
    (void)local_mtu;  /* MTU handled by ESP-IDF L2CAP internally (no param in API) */

    if (!s_hal_initialized) return -1;

    /* Guard against double-connect (supervisor state machine bug or race) */
    portENTER_CRITICAL_SAFE(&s_state_spinlock);
    bool already = s_is_connected;
    portEXIT_CRITICAL_SAFE(&s_state_spinlock);
    if (already) {
        ESP_LOGW(TAG, "hal_bt_l2cap_connect: already connected, ignoring");
        return -1;
    }

    esp_err_t ret = esp_bt_l2cap_connect(
        ESP_BT_L2CAP_SEC_AUTHENTICATE | ESP_BT_L2CAP_SEC_ENCRYPT,  /* Android 14 requires both */
        psm,                              /* Passed from supervisor (typically HAL_BNEP_PSM 0x000F) */
        (uint8_t*)remote_addr             /* Cast away const for esp_bd_addr_t param */
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "L2CAP connect failed: %s", esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

void hal_bt_l2cap_disconnect(void) {
    portENTER_CRITICAL_SAFE(&s_state_spinlock);
    int fd = s_l2cap_fd;
    bool connected = s_is_connected;
    portEXIT_CRITICAL_SAFE(&s_state_spinlock);

    if (connected && fd >= 0) {
        close(fd);  /* POSIX close triggers CLOSE_EVT in callback */
        /* NOTE: The RX reader task is NOT explicitly stopped here. The CLOSE_EVT
         * handler calls signal_rx_reader_stop(), and the reader task exits
         * asynchronously when read() returns -1 (fd closed). This is safe because:
         *   1. The reader task exits quickly after read() error
         *   2. start_rx_reader_task() overwrites s_rx_reader_task on next connect
         *   3. The old task's vTaskDelete(NULL) frees its own TCB
         * If reconnection fails due to stale task state, add stop_rx_reader_task()
         * here (note: blocking — must NOT be called from BTU context). */
    }
}

int hal_bt_l2cap_send(const uint8_t* data, uint16_t len) {
    portENTER_CRITICAL_SAFE(&s_state_spinlock);
    int fd = s_l2cap_fd;
    bool connected = s_is_connected;
    portEXIT_CRITICAL_SAFE(&s_state_spinlock);

    if (!connected || fd < 0) return -1;

    ssize_t written = write(fd, data, len);
    if (written < 0) {
        return -1;  /* Error (connection lost) */
    }
    if (written == 0) {
        return 1;   /* Ring buffer full — busy/congested (ESP-IDF specific) */
    }
    /* Partial write handling: ESP-IDF VFS write() typically writes the
     * full buffer to the ring buffer atomically, but defensively check. */
    if ((uint16_t)written != len) {
        ESP_LOGW(TAG, "Partial write: %d/%d bytes", (int)written, (int)len);
        return -1;  /* Treat partial write as error for BNEP frame integrity */
    }
    return 0;
}

int hal_bt_l2cap_send_iovec(const tinypan_iovec_t* iov, uint16_t iov_count) {
    portENTER_CRITICAL_SAFE(&s_state_spinlock);
    int fd = s_l2cap_fd;
    bool connected = s_is_connected;
    portEXIT_CRITICAL_SAFE(&s_state_spinlock);

    if (!connected || fd < 0) return -1;

    /* Concatenate iovec into aligned bounce buffer */
    uint32_t total_len = 0;
    if (iov_count == 1) {
        if (iov[0].iov_len > TINYPAN_L2CAP_MTU) return -1;
        memcpy(s_tx_aligned_buf, iov[0].iov_base, iov[0].iov_len);
        total_len = iov[0].iov_len;
    } else {
        for (int i = 0; i < iov_count; i++) {
            if (total_len + iov[i].iov_len > TINYPAN_L2CAP_MTU) return -1;
            memcpy(s_tx_aligned_buf + total_len, iov[i].iov_base, iov[i].iov_len);
            total_len += iov[i].iov_len;
        }
    }

    ssize_t written = write(fd, s_tx_aligned_buf, total_len);
    if (written < 0) {
        return -1;  /* Error */
    }
    if (written == 0) {
        return 1;   /* Ring buffer full — busy/congested */
    }
    if ((uint32_t)written != total_len) {
        ESP_LOGW(TAG, "Partial iovec write: %d/%lu bytes", (int)written, (unsigned long)total_len);
        return -1;
    }

    /* Defer TX_COMPLETE to hal_bt_poll */
    s_tx_complete_pending = true;
    return 0;
}

void hal_bt_l2cap_request_can_send_now(void) {
    portENTER_CRITICAL_SAFE(&s_state_spinlock);
    bool connected = s_is_connected;
    portEXIT_CRITICAL_SAFE(&s_state_spinlock);

    if (!connected || !s_can_send_timer) return;

    /* Start/restart the one-shot timer. When it fires (after 10ms),
     * can_send_timer_cb() queues HAL_L2CAP_EVENT_CAN_SEND_NOW.
     * xTimerReset is non-blocking when called with ticks_to_wait=0. */
    BaseType_t timer_ret = xTimerReset(s_can_send_timer, 0);
    if (timer_ret != pdPASS) {
        /* Timer command queue full — fallback: queue CAN_SEND_NOW directly.
         * Safe because we're in the app task context (same as hal_bt_poll). */
        ESP_LOGW(TAG, "CAN_SEND timer queue full, direct-queuing");
        esp_event_msg_t msg = { .event_id = HAL_L2CAP_EVENT_CAN_SEND_NOW, .status = 0 };
        xQueueSend(s_event_queue, &msg, 0);
        if (s_wakeup_cb) s_wakeup_cb(s_wakeup_cb_data);
    }
}

bool hal_bt_l2cap_can_send(void) {
    portENTER_CRITICAL_SAFE(&s_state_spinlock);
    bool can_send = s_is_connected && (s_l2cap_fd >= 0);
    portEXIT_CRITICAL_SAFE(&s_state_spinlock);
    return can_send;
}

bool hal_bt_l2cap_is_connected(void) {
    bool connected;
    portENTER_CRITICAL_SAFE(&s_state_spinlock);
    connected = s_is_connected;
    portEXIT_CRITICAL_SAFE(&s_state_spinlock);
    return connected;
}

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

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

/* ============================================================================
 * System & Utility Functions
 * ============================================================================ */

void hal_get_local_bd_addr(uint8_t addr[HAL_BD_ADDR_LEN]) {
    const uint8_t* bd_addr = esp_bt_dev_get_address();
    if (bd_addr) {
        memcpy(addr, bd_addr, HAL_BD_ADDR_LEN);
    } else {
        memset(addr, 0, HAL_BD_ADDR_LEN);
    }
}

uint32_t hal_get_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

uint32_t hal_bt_get_next_timeout_ms(void) {
    return 1000;  /* Default supervisor tick */
}

uint16_t hal_bt_l2cap_get_mtu(void) {
    /* Returns the actual negotiated MTU from the last OPEN_EVT.
     * Falls back to TINYPAN_L2CAP_MTU when not connected. */
    return s_negotiated_mtu;
}

/* ============================================================================
 * Mutex Implementation (FreeRTOS Semaphores)
 * ============================================================================ */

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
