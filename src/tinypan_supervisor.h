/*
 * TinyPAN Supervisor - Internal Header
 * 
 * Connection state machine and link management.
 */

#ifndef TINYPAN_SUPERVISOR_H
#define TINYPAN_SUPERVISOR_H

#include <stdint.h>
#include <stdbool.h>
#include "../include/tinypan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Supervisor API
 * ============================================================================ */

/**
 * @brief Initialize supervisor
 * 
 * @param config Pointer to configuration
 */
void supervisor_init(const tinypan_config_t* config);

/**
 * @brief Start supervisor (begin connection process)
 * 
 * @return 0 on success, negative on error
 */
int supervisor_start(void);

/**
 * @brief Stop supervisor and disconnect
 */
void supervisor_stop(void);

/**
 * @brief Process supervisor state machine
 * 
 * Must be called periodically.
 */
void supervisor_process(void);

/**
 * @brief Get current state
 */
tinypan_state_t supervisor_get_state(void);

/**
 * @brief Check if online
 */
bool supervisor_is_online(void);

/**
 * @brief Called when BNEP connection is established
 */
void supervisor_on_bnep_connected(void);

/**
 * @brief Called when BNEP connection is closed
 */
void supervisor_on_bnep_disconnected(void);

/**
 * @brief Called when BNEP setup response is received
 * 
 * @param response_code Setup response code
 */
void supervisor_on_bnep_setup_response(uint16_t response_code);

/**
 * @brief Called when IP address is acquired via DHCP
 */
void supervisor_on_ip_acquired(void);

/**
 * @brief Called when IP address is lost
 */
void supervisor_on_ip_lost(void);

/**
 * @brief Called when an L2CAP event occurs
 * 
 * @param event Event type
 * @param status Status code
 */
void supervisor_on_l2cap_event(int event, int status);

#ifdef __cplusplus
}
#endif

#endif /* TINYPAN_SUPERVISOR_H */
