/*
 * TinyPAN - Main Implementation
 * 
 * Entry point and public API implementation.
 */

#include "../include/tinypan.h"
#include "../include/tinypan_hal.h"
#include "tinypan_bnep.h"
#include "tinypan_supervisor.h"
#include "tinypan_internal.h"
#include <string.h>

#if TINYPAN_ENABLE_LWIP
#include "tinypan_lwip_netif.h"
#include "lwip/timeouts.h"
#endif

/* ============================================================================
 * State
 * ============================================================================ */

static bool s_initialized = false;
static tinypan_config_t s_config;
static tinypan_event_callback_t s_event_callback = NULL;
static void* s_event_callback_user_data = NULL;
static tinypan_state_t s_last_reported_state = TINYPAN_STATE_IDLE;

/* IP info (will be filled by lwIP integration) */
static tinypan_ip_info_t s_ip_info = {0};
static bool s_has_ip = false;

/* ============================================================================
 * Internal Callbacks
 * ============================================================================ */

/**
 * @brief L2CAP receive callback - passes data to BNEP layer
 */
static void l2cap_recv_callback(const uint8_t* data, uint16_t len, void* user_data) {
    (void)user_data;
    bnep_handle_incoming(data, len);
}

/**
 * @brief L2CAP event callback - passes events to supervisor
 */
static void l2cap_event_callback(hal_l2cap_event_t event, int status, void* user_data) {
    (void)user_data;
    supervisor_on_l2cap_event((int)event, status);
}

/**
 * @brief BNEP setup response callback - passes to supervisor
 */
static void bnep_setup_response_callback(uint16_t response_code, void* user_data) {
    (void)user_data;
    supervisor_on_bnep_setup_response(response_code);
}

/**
 * @brief BNEP frame receive callback - routes frames into lwIP
 */
static void bnep_frame_callback(const bnep_ethernet_frame_t* frame, void* user_data) {
    (void)user_data;
    
    TINYPAN_LOG_DEBUG("Received frame: type=0x%04X len=%u",
                       frame->ethertype, frame->payload_len);
    
#if TINYPAN_ENABLE_LWIP
    if (frame == NULL) {
        return;
    }

    /* Pass the raw pointers directly to lwIP to achieve true
       single-copy routing (copied only when instantiating the pbuf) */
    tinypan_netif_input(frame->dst_addr, frame->src_addr, frame->ethertype,
                        frame->payload, frame->payload_len);
#else
    /* No lwIP: frame is received but has nowhere to go */
#endif
}

/* ============================================================================
 * Event Dispatch
 * ============================================================================ */

static void dispatch_event(tinypan_event_t event) {
    if (s_event_callback) {
        s_event_callback(event, s_event_callback_user_data);
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

void tinypan_config_init(tinypan_config_t* config) {
    if (config == NULL) {
        return;
    }
    
    memset(config, 0, sizeof(tinypan_config_t));
    
    /* Set defaults */
    config->reconnect_interval_ms = 1000;
    config->reconnect_max_ms = 30000;
    config->heartbeat_interval_ms = 15000;
    config->heartbeat_retries = 3;
    config->max_reconnect_attempts = 0;  /* Infinite */
}

tinypan_error_t tinypan_init(const tinypan_config_t* config) {
    if (config == NULL) {
        TINYPAN_LOG_ERROR("tinypan_init: NULL config");
        return TINYPAN_ERR_INVALID_PARAM;
    }
    
    if (s_initialized) {
        TINYPAN_LOG_WARN("tinypan_init: Already initialized");
        return TINYPAN_ERR_ALREADY_STARTED;
    }
    
    TINYPAN_LOG_INFO("TinyPAN v%d.%d.%d initializing",
                      TINYPAN_VERSION_MAJOR, TINYPAN_VERSION_MINOR, TINYPAN_VERSION_PATCH);
    
    /* Copy config */
    memcpy(&s_config, config, sizeof(tinypan_config_t));
    
    /* Initialize HAL */
    int hal_result = hal_bt_init();
    if (hal_result < 0) {
        TINYPAN_LOG_ERROR("HAL init failed: %d", hal_result);
        return TINYPAN_ERR_HAL_FAILED;
    }
    
    /* Register HAL callbacks */
    hal_bt_l2cap_register_recv_callback(l2cap_recv_callback, NULL);
    hal_bt_l2cap_register_event_callback(l2cap_event_callback, NULL);
    
    /* Initialize BNEP layer */
    bnep_init();
    bnep_register_frame_callback(bnep_frame_callback, NULL);
    bnep_register_setup_response_callback(bnep_setup_response_callback, NULL);
    
    /* Initialize supervisor */
    supervisor_init(config);
    s_last_reported_state = supervisor_get_state();

#if TINYPAN_ENABLE_LWIP
    if (tinypan_netif_init() < 0) {
        TINYPAN_LOG_ERROR("lwIP netif init failed");
        hal_bt_deinit();
        return TINYPAN_ERR_HAL_FAILED;
    }
#endif
    
    s_initialized = true;
    s_has_ip = false;
    
    TINYPAN_LOG_INFO("TinyPAN initialized successfully");
    return TINYPAN_OK;
}

void tinypan_set_event_callback(tinypan_event_callback_t callback, void* user_data) {
    s_event_callback = callback;
    s_event_callback_user_data = user_data;
}

tinypan_error_t tinypan_start(void) {
    if (!s_initialized) {
        TINYPAN_LOG_ERROR("tinypan_start: Not initialized");
        return TINYPAN_ERR_NOT_INITIALIZED;
    }
    
    TINYPAN_LOG_INFO("TinyPAN starting");
    
    int result = supervisor_start();
    if (result < 0) {
        TINYPAN_LOG_ERROR("Failed to start supervisor: %d", result);
        return TINYPAN_ERR_HAL_FAILED;
    }
    
    dispatch_event(TINYPAN_EVENT_STATE_CHANGED);
    return TINYPAN_OK;
}

void tinypan_stop(void) {
    if (!s_initialized) {
        return;
    }
    
    TINYPAN_LOG_INFO("TinyPAN stopping");
    
    tinypan_state_t previous_state = supervisor_get_state();
    supervisor_stop();

#if TINYPAN_ENABLE_LWIP
    tinypan_netif_stop_dhcp();
    tinypan_netif_set_link(false);
#endif

    s_has_ip = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));

    tinypan_state_t current_state = supervisor_get_state();
    if (current_state != s_last_reported_state) {
        s_last_reported_state = current_state;
        dispatch_event(TINYPAN_EVENT_STATE_CHANGED);
    }

    if (previous_state != TINYPAN_STATE_IDLE) {
        dispatch_event(TINYPAN_EVENT_DISCONNECTED);
    }
}

void tinypan_process(void) {
    if (!s_initialized) {
        return;
    }
    
    /* Process supervisor state machine */
    supervisor_process();

    tinypan_state_t current_state = supervisor_get_state();
    if (current_state != s_last_reported_state) {
        s_last_reported_state = current_state;
        dispatch_event(TINYPAN_EVENT_STATE_CHANGED);
    }

#if TINYPAN_ENABLE_LWIP
    tinypan_netif_process();
#endif
}

uint32_t tinypan_get_next_timeout_ms(void) {
    if (!s_initialized) {
        return 0xFFFFFFFF; /* Infinite sleep, library not active */
    }
    
    uint32_t sleep_ms = 0xFFFFFFFF;
    
#if TINYPAN_ENABLE_LWIP
    /* Get lwIP's internal timer resolution (DHCP renew, TCP ACKs, etc.) */
    uint32_t lwip_sleep = sys_timeouts_sleeptime();
    if (lwip_sleep < sleep_ms) {
        sleep_ms = lwip_sleep;
    }
#endif

    /* TinyPAN has its own internal state machine timeouts.
       If we're actively connecting or running the BNEP handshake, clamp sleep
       to 50ms to ensure we don't miss Supervisor-level timeout transitions. */
    tinypan_state_t state = supervisor_get_state();
    if (state != TINYPAN_STATE_IDLE && state != TINYPAN_STATE_ONLINE && state != TINYPAN_STATE_ERROR) {
        if (sleep_ms > 50) {
            sleep_ms = 50;
        }
    }

    return sleep_ms;
}

tinypan_state_t tinypan_get_state(void) {
    return supervisor_get_state();
}

const char* tinypan_state_to_string(tinypan_state_t state) {
    switch (state) {
        case TINYPAN_STATE_IDLE:         return "IDLE";
        case TINYPAN_STATE_SCANNING:     return "SCANNING";
        case TINYPAN_STATE_CONNECTING:   return "CONNECTING";
        case TINYPAN_STATE_BNEP_SETUP:   return "BNEP_SETUP";
        case TINYPAN_STATE_DHCP:         return "DHCP";
        case TINYPAN_STATE_ONLINE:       return "ONLINE";
        case TINYPAN_STATE_STALLED:      return "STALLED";
        case TINYPAN_STATE_RECONNECTING: return "RECONNECTING";
        case TINYPAN_STATE_ERROR:        return "ERROR";
        default:                         return "UNKNOWN";
    }
}

bool tinypan_is_online(void) {
    return supervisor_is_online() && s_has_ip;
}

tinypan_error_t tinypan_get_ip_info(tinypan_ip_info_t* info) {
    if (info == NULL) {
        return TINYPAN_ERR_INVALID_PARAM;
    }
    
    if (!s_has_ip) {
        return TINYPAN_ERR_NOT_STARTED;
    }
    
    memcpy(info, &s_ip_info, sizeof(tinypan_ip_info_t));
    return TINYPAN_OK;
}

void tinypan_deinit(void) {
    if (!s_initialized) {
        return;
    }
    
    TINYPAN_LOG_INFO("TinyPAN de-initializing");
    
    tinypan_stop();

#if TINYPAN_ENABLE_LWIP
    tinypan_netif_deinit();
#endif

    hal_bt_deinit();
    
    s_initialized = false;
    s_event_callback = NULL;
    s_event_callback_user_data = NULL;
    s_last_reported_state = TINYPAN_STATE_IDLE;
    
    TINYPAN_LOG_INFO("TinyPAN de-initialized");
}

/* ============================================================================
 * Internal Functions (called by lwIP integration)
 * ============================================================================ */

void tinypan_internal_set_ip(uint32_t ip, uint32_t netmask, uint32_t gw, uint32_t dns) {
    s_ip_info.ip_addr = ip;
    s_ip_info.netmask = netmask;
    s_ip_info.gateway = gw;
    s_ip_info.dns_server = dns;
    s_has_ip = true;
    
    supervisor_on_ip_acquired();

    tinypan_state_t current_state = supervisor_get_state();
    if (current_state != s_last_reported_state) {
        s_last_reported_state = current_state;
        dispatch_event(TINYPAN_EVENT_STATE_CHANGED);
    }

    dispatch_event(TINYPAN_EVENT_IP_ACQUIRED);
}

void tinypan_internal_clear_ip(void) {
    s_has_ip = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    
    supervisor_on_ip_lost();

    tinypan_state_t current_state = supervisor_get_state();
    if (current_state != s_last_reported_state) {
        s_last_reported_state = current_state;
        dispatch_event(TINYPAN_EVENT_STATE_CHANGED);
    }

    dispatch_event(TINYPAN_EVENT_IP_LOST);
}
