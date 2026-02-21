/*
 * TinyPAN Hardware Abstraction Layer
 * 
 * This header defines the interface between TinyPAN and the underlying
 * Bluetooth stack. To port TinyPAN to a new platform, implement all
 * functions declared in this file.
 */

#ifndef TINYPAN_HAL_H
#define TINYPAN_HAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Bluetooth device address length */
#define HAL_BD_ADDR_LEN     6

/** L2CAP PSM for BNEP */
#define HAL_BNEP_PSM        0x000F

/** Minimum L2CAP MTU for BNEP */
#define HAL_BNEP_MIN_MTU    1691

/* ============================================================================
 * Event Types
 * ============================================================================ */

/**
 * @brief L2CAP event types passed to the event callback
 */
typedef enum {
    HAL_L2CAP_EVENT_CONNECTED = 1,      /**< L2CAP channel opened successfully */
    HAL_L2CAP_EVENT_DISCONNECTED,       /**< L2CAP channel closed */
    HAL_L2CAP_EVENT_CONNECT_FAILED,     /**< L2CAP connection attempt failed */
    HAL_L2CAP_EVENT_CAN_SEND_NOW        /**< Ready to send data */
} hal_l2cap_event_t;

/* ============================================================================
 * Callback Types
 * ============================================================================ */

/**
 * @brief Callback for incoming L2CAP data
 * 
 * @param data      Pointer to received data
 * @param len       Length of received data
 * @param user_data User data pointer from registration
 */
typedef void (*hal_l2cap_recv_callback_t)(const uint8_t* data, uint16_t len, void* user_data);

/**
 * @brief Callback for L2CAP connection events
 * 
 * @param event     Event type
 * @param status    Status code (0 = success, non-zero = error)
 * @param user_data User data pointer from registration
 */
typedef void (*hal_l2cap_event_callback_t)(hal_l2cap_event_t event, int status, void* user_data);

/* ============================================================================
 * Bluetooth Functions
 * ============================================================================ */

/**
 * @brief Initialize the Bluetooth stack
 * 
 * Called once during tinypan_init(). Should initialize the underlying
 * Bluetooth stack and prepare it for L2CAP connections.
 * 
 * @return 0 on success, negative error code on failure
 */
int hal_bt_init(void);

/**
 * @brief De-initialize the Bluetooth stack
 * 
 * Called during tinypan_deinit(). Should clean up all Bluetooth resources.
 */
void hal_bt_deinit(void);

/**
 * @brief Connect to a remote device's L2CAP channel
 * 
 * Initiates an L2CAP connection to the specified device and PSM.
 * This is a non-blocking call. The result will be reported via the
 * event callback (HAL_L2CAP_EVENT_CONNECTED or HAL_L2CAP_EVENT_CONNECT_FAILED).
 * 
 * @param remote_addr   6-byte Bluetooth device address
 * @param psm           Protocol/Service Multiplexer (use HAL_BNEP_PSM for BNEP)
 * @param local_mtu     Maximum Transmission Unit to negotiate (minimum 1691 for BNEP)
 * @return 0 on success (connection initiated), negative error code on failure
 */
int hal_bt_l2cap_connect(const uint8_t remote_addr[HAL_BD_ADDR_LEN], uint16_t psm, uint16_t local_mtu);

/**
 * @brief Disconnect the current L2CAP channel
 * 
 * Closes the active L2CAP connection. The event callback will receive
 * HAL_L2CAP_EVENT_DISCONNECTED when complete.
 */
void hal_bt_l2cap_disconnect(void);

/**
 * @brief Send data over the L2CAP channel
 * 
 * Sends a single contiguous buffer over the L2CAP channel.
 * The buffer contains the BNEP header followed by the IP payload.
 * 
 * @param data         Pointer to the BNEP frame (header + payload)
 * @param len          Total length of the frame
 * @return 0 on success, negative error code on failure, positive if busy (try again)
 */
int hal_bt_l2cap_send(const uint8_t* data, uint16_t len);

/**
 * @brief Check if the L2CAP channel is ready to send data
 * 
 * @return true if ready to send, false if busy
 */
bool hal_bt_l2cap_can_send(void);

/**
 * @brief Request a "can send now" event
 * 
 * If hal_bt_l2cap_can_send() returns false, call this function and wait for
 * the HAL_L2CAP_EVENT_CAN_SEND_NOW event before trying to send again.
 */
void hal_bt_l2cap_request_can_send_now(void);

/**
 * @brief Register callback for incoming L2CAP data
 * 
 * @param callback  Function to call when data is received
 * @param user_data User data pointer to pass to callback
 */
void hal_bt_l2cap_register_recv_callback(hal_l2cap_recv_callback_t callback, void* user_data);

/**
 * @brief Register callback for L2CAP events
 * 
 * @param callback  Function to call on events
 * @param user_data User data pointer to pass to callback
 */
void hal_bt_l2cap_register_event_callback(hal_l2cap_event_callback_t callback, void* user_data);

/* ============================================================================
 * System Functions
 * ============================================================================ */

/**
 * @brief Get the local Bluetooth device address
 * 
 * @param addr  Buffer to store the 6-byte address
 */
void hal_get_local_bd_addr(uint8_t addr[HAL_BD_ADDR_LEN]);

/**
 * @brief Get current time in milliseconds
 * 
 * Used for timeouts and timing. Must be monotonically increasing.
 * Wrap-around is acceptable (will be handled correctly).
 * 
 * @return Current time in milliseconds
 */
uint32_t hal_get_tick_ms(void);

/* ============================================================================
 * Non-Volatile Storage Functions (Optional)
 * ============================================================================ */

/**
 * @brief Load data from persistent storage
 * 
 * Used for storing bonding keys and configuration.
 * Implementing this is optional - return -1 if not supported.
 * 
 * @param key       String key to identify the data
 * @param buffer    Buffer to store the loaded data
 * @param max_len   Maximum number of bytes to load
 * @return Number of bytes loaded, or negative error code
 */
int hal_nv_load(const char* key, uint8_t* buffer, uint16_t max_len);

/**
 * @brief Save data to persistent storage
 * 
 * Used for storing bonding keys and configuration.
 * Implementing this is optional - return -1 if not supported.
 * 
 * @param key   String key to identify the data
 * @param data  Pointer to data to save
 * @param len   Length of data to save
 * @return 0 on success, negative error code on failure
 */
int hal_nv_save(const char* key, const uint8_t* data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* TINYPAN_HAL_H */
