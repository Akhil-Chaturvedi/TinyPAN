/*
 * TinyPAN Transport Layer Interface
 * 
 * Defines the contract for different transport backends (BNEP, SLIP, etc.).
 */

#ifndef TINYPAN_TRANSPORT_H
#define TINYPAN_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>

#include "../include/tinypan_config.h"

#if TINYPAN_ENABLE_LWIP
struct pbuf;
struct netif;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Transport interface definition
 */
typedef struct {
    /** Transport name for debugging */
    const char* name;
    
    /** True if this transport requires a setup phase (like BNEP) before DHCP */
    bool requires_setup;
    
    /**
     * @brief Initialize the transport layer
     * @return 0 on success, negative on error
     */
    int (*init)(void);
    
    /**
     * @brief Called when the underlying L2CAP/Link connects
     */
    void (*on_connected)(void);
    
    /**
     * @brief Called when the underlying L2CAP/Link disconnects
     */
    void (*on_disconnected)(void);
    
    /**
     * @brief Handle raw incoming data from the HAL
     * @param data Raw data buffer
     * @param len Length of the data
     */
    void (*handle_incoming)(const uint8_t* data, uint16_t len);
    
    /**
     * @brief Called during setup timeouts to retry the connection handshake
     */
    void (*retry_setup)(void);
    
    /**
     * @brief Called when the HAL signals CAN_SEND_NOW
     */
    void (*on_can_send_now)(void);
    
    /**
     * @brief Clear and free any pending TX queues (e.g. on disconnect)
     */
    void (*flush_queues)(void);
    
#if TINYPAN_ENABLE_LWIP
    /**
     * @brief Output an IP packet or Ethernet frame from lwIP
     * @param netif The lwIP network interface
     * @param p The pbuf to send
     * @return lwIP err_t
     */
    int (*output)(struct netif* netif, struct pbuf* p);
#endif
} tinypan_transport_t;

/**
 * @brief Get the active transport interface
 * @return Pointer to the active transport interface
 */
const tinypan_transport_t* tinypan_transport_get(void);

extern const tinypan_transport_t transport_bnep;
extern const tinypan_transport_t transport_slip;

#ifdef __cplusplus
}
#endif

#endif /* TINYPAN_TRANSPORT_H */
