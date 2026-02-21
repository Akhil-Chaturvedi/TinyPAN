/*
 * TinyPAN Supervisor - Implementation
 * 
 * Connection state machine and link management.
 */

#include "tinypan_supervisor.h"
#include "tinypan_bnep.h"
#include "../include/tinypan_config.h"
#include "../include/tinypan_hal.h"
#include <string.h>

#if TINYPAN_ENABLE_LWIP
#include "tinypan_lwip_netif.h"
#endif

/* ============================================================================
 * State
 * ============================================================================ */

static tinypan_state_t s_state = TINYPAN_STATE_IDLE;
static tinypan_config_t s_config;
static bool s_initialized = false;

/* Timing */
static uint32_t s_state_enter_time = 0;
static uint32_t s_last_action_time = 0;
static uint32_t s_reconnect_delay_ms = 0;
static uint8_t s_reconnect_attempts = 0;
static uint8_t s_setup_retries = 0;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * @brief Set state and record entry time
 */
static void set_state(tinypan_state_t new_state) {
    if (s_state != new_state) {
        TINYPAN_LOG_INFO("Supervisor: %s -> %s",
                          tinypan_state_to_string(s_state),
                          tinypan_state_to_string(new_state));
        s_state = new_state;
        s_state_enter_time = hal_get_tick_ms();
    }
}

/**
 * @brief Check if timeout has elapsed since state entry
 */
static bool timeout_elapsed(uint32_t timeout_ms) {
    uint32_t now = hal_get_tick_ms();
    uint32_t elapsed = now - s_state_enter_time;
    return elapsed >= timeout_ms;
}

/**
 * @brief Start L2CAP connection
 */
static int start_l2cap_connect(void) {
    TINYPAN_LOG_INFO("Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
                      s_config.remote_addr[0], s_config.remote_addr[1],
                      s_config.remote_addr[2], s_config.remote_addr[3],
                      s_config.remote_addr[4], s_config.remote_addr[5]);
    
    return hal_bt_l2cap_connect(s_config.remote_addr, HAL_BNEP_PSM, TINYPAN_L2CAP_MTU);
}

/**
 * @brief Schedule reconnection with exponential backoff
 */
static void schedule_reconnect(void) {
    if (s_reconnect_delay_ms == 0) {
        s_reconnect_delay_ms = s_config.reconnect_interval_ms;
    } else {
        s_reconnect_delay_ms *= 2;
        if (s_reconnect_delay_ms > s_config.reconnect_max_ms) {
            s_reconnect_delay_ms = s_config.reconnect_max_ms;
        }
    }
    
    TINYPAN_LOG_INFO("Reconnect scheduled in %lu ms (next attempt %u)",
                      (unsigned long)s_reconnect_delay_ms,
                      (unsigned int)(s_reconnect_attempts + 1));
    
    s_last_action_time = hal_get_tick_ms();
}

/* ============================================================================
 * Supervisor API Implementation
 * ============================================================================ */

void supervisor_init(const tinypan_config_t* config) {
    if (config == NULL) {
        TINYPAN_LOG_ERROR("Supervisor init: NULL config");
        return;
    }
    
    memcpy(&s_config, config, sizeof(tinypan_config_t));
    s_state = TINYPAN_STATE_IDLE;
    s_state_enter_time = 0;
    s_last_action_time = 0;
    s_reconnect_delay_ms = 0;
    s_reconnect_attempts = 0;
    s_setup_retries = 0;
    s_initialized = true;
    
    /* Set addresses for BNEP layer */
    uint8_t local_addr[6];
    hal_get_local_bd_addr(local_addr);
    bnep_set_local_addr(local_addr);
    bnep_set_remote_addr(s_config.remote_addr);
    
    TINYPAN_LOG_INFO("Supervisor initialized");
}

int supervisor_start(void) {
    if (!s_initialized) {
        TINYPAN_LOG_ERROR("Supervisor not initialized");
        return -1;
    }
    
    if (s_state != TINYPAN_STATE_IDLE && s_state != TINYPAN_STATE_ERROR) {
        TINYPAN_LOG_WARN("Supervisor already started");
        return -1;
    }
    
    /* Reset reconnection state */
    s_reconnect_delay_ms = 0;
    s_reconnect_attempts = 0;
    s_setup_retries = 0;
    
    /* Begin connecting */
    set_state(TINYPAN_STATE_CONNECTING);
    
    int result = start_l2cap_connect();
    if (result < 0) {
        TINYPAN_LOG_ERROR("Failed to start L2CAP connect: %d", result);
        set_state(TINYPAN_STATE_ERROR);
        return result;
    }
    
    return 0;
}

void supervisor_stop(void) {
    TINYPAN_LOG_INFO("Supervisor stopping");
    
    if (s_state != TINYPAN_STATE_IDLE) {
        hal_bt_l2cap_disconnect();
        bnep_reset();
    }
    
    set_state(TINYPAN_STATE_IDLE);
    s_reconnect_delay_ms = 0;
    s_reconnect_attempts = 0;
}

void supervisor_process(void) {
    if (!s_initialized) {
        return;
    }
    
    switch (s_state) {
        case TINYPAN_STATE_IDLE:
            /* Nothing to do */
            break;
            
        case TINYPAN_STATE_CONNECTING:
            /* Check for timeout */
            if (timeout_elapsed(TINYPAN_L2CAP_CONNECT_TIMEOUT_MS)) {
                TINYPAN_LOG_WARN("L2CAP connect timeout");
                hal_bt_l2cap_disconnect();
                
#if TINYPAN_ENABLE_AUTO_RECONNECT
                set_state(TINYPAN_STATE_RECONNECTING);
                schedule_reconnect();
#else
                set_state(TINYPAN_STATE_ERROR);
#endif
            }
            break;
            
        case TINYPAN_STATE_BNEP_SETUP:
            /* Check for timeout */
            if (timeout_elapsed(TINYPAN_BNEP_SETUP_TIMEOUT_MS)) {
                TINYPAN_LOG_WARN("BNEP setup timeout");
                
                s_setup_retries++;
                if (s_setup_retries < TINYPAN_BNEP_SETUP_RETRIES) {
                    /* Retry setup request */
                    TINYPAN_LOG_INFO("Retrying BNEP setup (attempt %u)", s_setup_retries + 1);
                    s_state_enter_time = hal_get_tick_ms();
                    bnep_send_setup_request();
                } else {
                    /* Give up */
                    TINYPAN_LOG_ERROR("BNEP setup failed after %u retries", TINYPAN_BNEP_SETUP_RETRIES);
                    hal_bt_l2cap_disconnect();
                    
#if TINYPAN_ENABLE_AUTO_RECONNECT
                    set_state(TINYPAN_STATE_RECONNECTING);
                    schedule_reconnect();
#else
                    set_state(TINYPAN_STATE_ERROR);
#endif
                }
            }
            break;
            
        case TINYPAN_STATE_DHCP:
            /* Check for timeout */
            if (timeout_elapsed(TINYPAN_DHCP_TIMEOUT_MS)) {
                TINYPAN_LOG_WARN("DHCP timeout");
                /* Stay connected, maybe retry DHCP */
                /* For now, just wait - lwIP will keep trying */
            }
            break;
            
        case TINYPAN_STATE_ONLINE:
            /* Not implemented: heartbeat / keepalive monitoring */
            break;
            
        case TINYPAN_STATE_STALLED:
            /* Not implemented: link recovery logic */
            break;
            
        case TINYPAN_STATE_RECONNECTING:
#if TINYPAN_ENABLE_AUTO_RECONNECT
            {
                uint32_t now = hal_get_tick_ms();
                uint32_t elapsed = now - s_last_action_time;
                
                if (elapsed >= s_reconnect_delay_ms) {
                    /* Check if max attempts reached */
                    if (s_config.max_reconnect_attempts > 0 &&
                        s_reconnect_attempts >= s_config.max_reconnect_attempts) {
                        TINYPAN_LOG_ERROR("Max reconnect attempts reached");
                        set_state(TINYPAN_STATE_ERROR);
                    } else {
                        /* Try to reconnect */
                        s_reconnect_attempts++;
                        TINYPAN_LOG_INFO("Reconnecting (attempt %u)...",
                                         (unsigned int)s_reconnect_attempts);
                        set_state(TINYPAN_STATE_CONNECTING);
                        s_setup_retries = 0;

                        int result = start_l2cap_connect();
                        if (result < 0) {
                            TINYPAN_LOG_ERROR("Reconnect failed: %d", result);
                            set_state(TINYPAN_STATE_RECONNECTING);
                            schedule_reconnect();
                        }
                    }
                }
            }
#endif
            break;
            
        case TINYPAN_STATE_ERROR:
            /* Permanent error, do nothing */
            break;
            
        default:
            TINYPAN_LOG_ERROR("Unknown state: %d", s_state);
            break;
    }
}

tinypan_state_t supervisor_get_state(void) {
    return s_state;
}

bool supervisor_is_online(void) {
    return s_state == TINYPAN_STATE_ONLINE;
}

void supervisor_on_l2cap_event(int event, int status) {
    switch (event) {
        case HAL_L2CAP_EVENT_CONNECTED:
            TINYPAN_LOG_INFO("L2CAP connected");
            if (s_state == TINYPAN_STATE_CONNECTING) {
                set_state(TINYPAN_STATE_BNEP_SETUP);
                s_setup_retries = 0;
                bnep_on_l2cap_connected();
            }
            break;
            
        case HAL_L2CAP_EVENT_DISCONNECTED:
            TINYPAN_LOG_INFO("L2CAP disconnected");
            bnep_on_l2cap_disconnected();
            
            if (s_state == TINYPAN_STATE_ONLINE || 
                s_state == TINYPAN_STATE_DHCP ||
                s_state == TINYPAN_STATE_BNEP_SETUP) {
#if TINYPAN_ENABLE_AUTO_RECONNECT
                set_state(TINYPAN_STATE_RECONNECTING);
                schedule_reconnect();
#else
                set_state(TINYPAN_STATE_IDLE);
#endif
            } else if (s_state == TINYPAN_STATE_CONNECTING) {
                /* Connect failed */
#if TINYPAN_ENABLE_AUTO_RECONNECT
                set_state(TINYPAN_STATE_RECONNECTING);
                schedule_reconnect();
#else
                set_state(TINYPAN_STATE_ERROR);
#endif
            }
            break;
            
        case HAL_L2CAP_EVENT_CONNECT_FAILED:
            TINYPAN_LOG_ERROR("L2CAP connect failed: %d", status);
            
#if TINYPAN_ENABLE_AUTO_RECONNECT
            set_state(TINYPAN_STATE_RECONNECTING);
            schedule_reconnect();
#else
            set_state(TINYPAN_STATE_ERROR);
#endif
            break;
            
        case HAL_L2CAP_EVENT_CAN_SEND_NOW:
            TINYPAN_LOG_DEBUG("L2CAP can send now (flushing queues)");
#if TINYPAN_ENABLE_LWIP
            tinypan_netif_drain_tx_queue();
#endif
            break;
            
        default:
            TINYPAN_LOG_WARN("Unknown L2CAP event: %d", event);
            break;
    }
}

void supervisor_on_bnep_connected(void) {
    TINYPAN_LOG_INFO("BNEP connected");
    
    /* Reset reconnect state on successful connection */
    s_reconnect_delay_ms = 0;
    s_reconnect_attempts = 0;
}

void supervisor_on_bnep_disconnected(void) {
    TINYPAN_LOG_INFO("BNEP disconnected");
    /* Handled by L2CAP disconnect */
}

void supervisor_on_bnep_setup_response(uint16_t response_code) {
    if (response_code == BNEP_SETUP_RESPONSE_SUCCESS) {
        TINYPAN_LOG_INFO("BNEP setup successful");
        set_state(TINYPAN_STATE_DHCP);
        supervisor_on_bnep_connected();

#if TINYPAN_ENABLE_LWIP
        tinypan_netif_set_link(true);
        if (tinypan_netif_start_dhcp() < 0) {
            TINYPAN_LOG_ERROR("Failed to start DHCP");
            hal_bt_l2cap_disconnect();
            set_state(TINYPAN_STATE_RECONNECTING);
            schedule_reconnect();
        }
#else
        /* No lwIP: DHCP must be handled externally */
#endif
    } else {
        TINYPAN_LOG_ERROR("BNEP setup rejected: 0x%04X", response_code);
        hal_bt_l2cap_disconnect();
        
#if TINYPAN_ENABLE_AUTO_RECONNECT
        set_state(TINYPAN_STATE_RECONNECTING);
        schedule_reconnect();
#else
        set_state(TINYPAN_STATE_ERROR);
#endif
    }
}

void supervisor_on_ip_acquired(void) {
    TINYPAN_LOG_INFO("IP acquired - we are ONLINE!");
    set_state(TINYPAN_STATE_ONLINE);
}

void supervisor_on_ip_lost(void) {
    TINYPAN_LOG_WARN("IP lost");
    if (s_state == TINYPAN_STATE_ONLINE) {
        set_state(TINYPAN_STATE_DHCP);
#if TINYPAN_ENABLE_LWIP
        if (tinypan_netif_start_dhcp() < 0) {
            TINYPAN_LOG_WARN("Failed to restart DHCP");
        }
#else
        /* No lwIP: DHCP restart must be handled externally */
#endif
    }
}

uint32_t supervisor_get_next_timeout_ms(void) {
    if (s_state == TINYPAN_STATE_IDLE || s_state == TINYPAN_STATE_ONLINE || s_state == TINYPAN_STATE_ERROR) {
        return 0xFFFFFFFF;
    }

    uint32_t now = hal_get_tick_ms();
    uint32_t target_timeout = 0;
    uint32_t base_time = s_state_enter_time;

    switch (s_state) {
        case TINYPAN_STATE_CONNECTING:
            target_timeout = TINYPAN_L2CAP_CONNECT_TIMEOUT_MS;
            break;
        case TINYPAN_STATE_BNEP_SETUP:
            target_timeout = TINYPAN_BNEP_SETUP_TIMEOUT_MS;
            break;
        case TINYPAN_STATE_DHCP:
            target_timeout = TINYPAN_DHCP_TIMEOUT_MS;
            break;
        case TINYPAN_STATE_RECONNECTING:
            target_timeout = s_reconnect_delay_ms;
            base_time = s_last_action_time;
            break;
        default:
            return 0xFFFFFFFF;
    }

    uint32_t elapsed = now - base_time;
    if (elapsed >= target_timeout) {
        return 0; /* Already timed out, wake up immediately */
    }

    return target_timeout - elapsed;
}
