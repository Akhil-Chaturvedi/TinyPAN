/*
 * TinyPAN Mock HAL
 * 
 * A mock implementation of the HAL for unit testing on Windows/Linux
 * without real Bluetooth hardware.
 */

#include "../../include/tinypan_hal.h"
#include "../../include/tinypan_config.h"
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

/* ============================================================================
 * Mock State
 * ============================================================================ */

static bool s_initialized = false;
static bool s_connected = false;
static bool s_can_send = true;

static uint8_t s_local_addr[HAL_BD_ADDR_LEN] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

static hal_l2cap_recv_callback_t s_recv_callback = NULL;
static void* s_recv_callback_user_data = NULL;

static hal_l2cap_event_callback_t s_event_callback = NULL;
static void* s_event_callback_user_data = NULL;

static bool s_use_mock_time = false;
static uint32_t s_mock_tick_ms = 0;

/* Last TX buffer capture for test inspection */
static uint8_t s_last_tx_data[1500] = {0};
static uint16_t s_last_tx_len = 0;

/* ============================================================================
 * Mock Control API (for testing)
 * ============================================================================ */

void mock_hal_use_mock_time(bool enabled) {
    s_use_mock_time = enabled;
}

void mock_hal_set_tick_ms(uint32_t tick_ms) {
    s_mock_tick_ms = tick_ms;
}

void mock_hal_advance_tick_ms(uint32_t delta_ms) {
    s_mock_tick_ms += delta_ms;
}

/**
 * @brief Simulate L2CAP connection success
 * 
 * Call this from test code to simulate the phone accepting the connection.
 */
void mock_hal_simulate_connect_success(void) {
    if (!s_initialized) return;
    
    s_connected = true;
    TINYPAN_LOG_DEBUG("[MOCK] Simulating L2CAP connect success");
    
    if (s_event_callback) {
        s_event_callback(HAL_L2CAP_EVENT_CONNECTED, 0, s_event_callback_user_data);
    }
}

/**
 * @brief Simulate L2CAP connection failure
 */
void mock_hal_simulate_connect_failure(int status) {
    if (!s_initialized) return;
    
    s_connected = false;
    TINYPAN_LOG_DEBUG("[MOCK] Simulating L2CAP connect failure: %d", status);
    
    if (s_event_callback) {
        s_event_callback(HAL_L2CAP_EVENT_CONNECT_FAILED, status, s_event_callback_user_data);
    }
}

/**
 * @brief Simulate L2CAP disconnection
 */
void mock_hal_simulate_disconnect(void) {
    if (!s_initialized) return;
    
    s_connected = false;
    TINYPAN_LOG_DEBUG("[MOCK] Simulating L2CAP disconnect");
    
    if (s_event_callback) {
        s_event_callback(HAL_L2CAP_EVENT_DISCONNECTED, 0, s_event_callback_user_data);
    }
}

/**
 * @brief Simulate receiving data
 * 
 * @param data Data to "receive"
 * @param len Length of data
 */
void mock_hal_simulate_receive(const uint8_t* data, uint16_t len) {
    if (!s_initialized || !s_connected) return;
    if (data == NULL || len == 0) return;
    
    TINYPAN_LOG_DEBUG("[MOCK] Simulating receive: %u bytes", len);
    
    if (s_recv_callback) {
        s_recv_callback(data, len, s_recv_callback_user_data);
    }
}

/**
 * @brief Simulate BNEP setup response (success)
 */
void mock_hal_simulate_bnep_setup_success(void) {
    /* BNEP Setup Response: Type=0x01 (Control), ControlType=0x02 (Response), Code=0x0000 (Success) */
    uint8_t response[] = {0x01, 0x02, 0x00, 0x00};
    mock_hal_simulate_receive(response, sizeof(response));
}

/**
 * @brief Set whether sending is allowed
 */
void mock_hal_set_can_send(bool can_send) {
    s_can_send = can_send;
    
    if (can_send && s_event_callback) {
        s_event_callback(HAL_L2CAP_EVENT_CAN_SEND_NOW, 0, s_event_callback_user_data);
    }
}

/**
 * @brief Check if mock is connected
 */
bool mock_hal_is_connected(void) {
    return s_connected;
}

/* ============================================================================
 * HAL Implementation
 * ============================================================================ */

int hal_bt_init(void) {
    TINYPAN_LOG_INFO("[MOCK] HAL initializing");
    s_initialized = true;
    s_connected = false;
    s_can_send = true;
    s_mock_tick_ms = 0;
    return 0;
}

void hal_bt_deinit(void) {
    TINYPAN_LOG_INFO("[MOCK] HAL de-initializing");
    s_initialized = false;
    s_connected = false;
    s_recv_callback = NULL;
    s_event_callback = NULL;
}

int hal_bt_l2cap_connect(const uint8_t remote_addr[HAL_BD_ADDR_LEN], uint16_t psm) {
    if (!s_initialized) {
        return -1;
    }
    
    TINYPAN_LOG_INFO("[MOCK] L2CAP connect to %02X:%02X:%02X:%02X:%02X:%02X PSM=0x%04X",
                      remote_addr[0], remote_addr[1], remote_addr[2],
                      remote_addr[3], remote_addr[4], remote_addr[5], psm);
    
    /* In mock mode, connection is not automatic.
       Test code must call mock_hal_simulate_connect_success() */
    
    return 0;  /* Initiated successfully */
}

void hal_bt_l2cap_disconnect(void) {
    if (!s_initialized) return;
    
    TINYPAN_LOG_INFO("[MOCK] L2CAP disconnect");
    s_connected = false;
}

int hal_bt_l2cap_send_sg(const uint8_t* header, uint16_t header_len, 
                         const uint8_t* payload, uint16_t payload_len) {
    if (!s_initialized) {
        return -1;
    }
    
    if (!s_connected) {
        TINYPAN_LOG_WARN("[MOCK] Cannot send: not connected");
        return -1;
    }
    
    if (!s_can_send) {
        TINYPAN_LOG_DEBUG("[MOCK] Cannot send now");
        return 1;  /* Busy */
    }
    
    uint16_t total_len = header_len + payload_len;
    TINYPAN_LOG_DEBUG("[MOCK] Sending %u bytes (sg):", total_len);
    
    if (total_len <= sizeof(s_last_tx_data)) {
        if (header != NULL && header_len > 0) {
            memcpy(s_last_tx_data, header, header_len);
        }
        if (payload != NULL && payload_len > 0) {
            memcpy(s_last_tx_data + header_len, payload, payload_len);
        }
        s_last_tx_len = total_len;
    }
    
    /* Print hex dump for debugging */
    #if TINYPAN_ENABLE_DEBUG
    char hex_buf[256];
    int pos = 0;
    for (uint16_t i = 0; i < total_len && pos < 240; i++) {
        pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", s_last_tx_data[i]);
    }
    TINYPAN_LOG_DEBUG("[MOCK] TX: %s", hex_buf);
    #endif
    
    return 0;
}

bool hal_bt_l2cap_can_send(void) {
    return s_initialized && s_connected && s_can_send;
}

void hal_bt_l2cap_request_can_send_now(void) {
    /* In mock, immediately fire event if can send */
    if (s_can_send && s_event_callback) {
        s_event_callback(HAL_L2CAP_EVENT_CAN_SEND_NOW, 0, s_event_callback_user_data);
    }
}

void hal_bt_l2cap_register_recv_callback(hal_l2cap_recv_callback_t callback, void* user_data) {
    s_recv_callback = callback;
    s_recv_callback_user_data = user_data;
}

void hal_bt_l2cap_register_event_callback(hal_l2cap_event_callback_t callback, void* user_data) {
    s_event_callback = callback;
    s_event_callback_user_data = user_data;
}

void hal_get_local_bd_addr(uint8_t addr[HAL_BD_ADDR_LEN]) {
    memcpy(addr, s_local_addr, HAL_BD_ADDR_LEN);
}

uint32_t hal_get_tick_ms(void) {
    if (s_use_mock_time) {
        return s_mock_tick_ms;
    }
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
#endif
}

int hal_nv_load(const char* key, uint8_t* buffer, uint16_t max_len) {
    (void)key;
    (void)buffer;
    (void)max_len;
    /* Not implemented in mock */
    return -1;
}

int hal_nv_save(const char* key, const uint8_t* data, uint16_t len) {
    (void)key;
    (void)data;
    (void)len;
    /* Not implemented in mock */
    return -1;
}

const uint8_t* mock_hal_get_last_tx_data(void) {
    return s_last_tx_data;
}

uint16_t mock_hal_get_last_tx_len(void) {
    return s_last_tx_len;
}
