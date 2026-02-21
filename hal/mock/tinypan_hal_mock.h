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
 * @brief Enable/disable deterministic mock time source for tests
 */
void mock_hal_use_mock_time(bool enabled);

/**
 * @brief Set current mock tick (milliseconds)
 */
void mock_hal_set_tick_ms(uint32_t tick_ms);

/**
 * @brief Advance current mock tick by delta milliseconds
 */
void mock_hal_advance_tick_ms(uint32_t delta_ms);

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

/**
 * @brief Get pointer to the last transmitted frame (for test assertions)
 */
const uint8_t* mock_hal_get_last_tx_data(void);

/**
 * @brief Get length of the last transmitted frame (for test assertions)
 */
uint16_t mock_hal_get_last_tx_len(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYPAN_HAL_MOCK_H */
