/*
 * TinyPAN Mock HAL - Header
 * 
 * Additional functions for test control.
 */

#ifndef TINYPAN_HAL_MOCK_H
#define TINYPAN_HAL_MOCK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Simulate L2CAP connection success
 */
void mock_hal_simulate_connect_success(void);

/**
 * @brief Simulate L2CAP connection failure
 */
void mock_hal_simulate_connect_failure(int status);

/**
 * @brief Simulate L2CAP disconnection
 */
void mock_hal_simulate_disconnect(void);

/**
 * @brief Simulate receiving data
 */
void mock_hal_simulate_receive(const uint8_t* data, uint16_t len);

/**
 * @brief Simulate BNEP setup response (success)
 */
void mock_hal_simulate_bnep_setup_success(void);

/**
 * @brief Set whether sending is allowed (for flow control testing)
 */
void mock_hal_set_can_send(bool can_send);

/**
 * @brief Check if mock is connected
 */
bool mock_hal_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYPAN_HAL_MOCK_H */
