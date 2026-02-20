/*
 * TinyPAN lwIP Network Interface Adapter - Header
 * 
 * Bridges the BNEP layer to lwIP's netif interface.
 */

#ifndef TINYPAN_LWIP_NETIF_H
#define TINYPAN_LWIP_NETIF_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration for lwIP netif */
struct netif;

/**
 * @brief Initialize the TinyPAN network interface
 * 
 * This function initializes the lwIP netif that uses BNEP as the underlying
 * transport. It should be called after tinypan_init() and before tinypan_start().
 * 
 * @return 0 on success, negative on error
 */
int tinypan_netif_init(void);

/**
 * @brief De-initialize the TinyPAN network interface
 */
void tinypan_netif_deinit(void);

/**
 * @brief Start DHCP on the TinyPAN interface
 * 
 * Called after BNEP connection is established to obtain an IP address.
 * 
 * @return 0 on success, negative on error
 */
int tinypan_netif_start_dhcp(void);

/**
 * @brief Stop DHCP on the TinyPAN interface
 */
void tinypan_netif_stop_dhcp(void);

/**
 * @brief Set the interface link state
 * 
 * Called when BNEP connection state changes.
 * 
 * @param up true if link is up, false if down
 */
void tinypan_netif_set_link(bool up);

/**
 * @brief Process incoming Ethernet frame from BNEP
 * 
 * Called by the BNEP layer when an Ethernet frame is received.
 * Passes the frame to lwIP for processing.
 * 
 * @param data   Pointer to Ethernet frame data
 * @param len    Length of frame data
 */
void tinypan_netif_input(const uint8_t* data, uint16_t len);

/**
 * @brief Get the lwIP netif structure
 * 
 * For advanced users who need direct access to lwIP.
 * 
 * @return Pointer to the netif, or NULL if not initialized
 */
struct netif* tinypan_netif_get(void);

/**
 * @brief Check if the interface has an IP address
 * 
 * @return true if IP is assigned
 */
bool tinypan_netif_has_ip(void);

/**
 * @brief Get the assigned IP address
 * 
 * @return IP address in network byte order, or 0 if none
 */
uint32_t tinypan_netif_get_ip(void);

/**
 * @brief Get the gateway address
 * 
 * @return Gateway address in network byte order, or 0 if none
 */
uint32_t tinypan_netif_get_gateway(void);

/**
 * @brief Get the netmask
 * 
 * @return Netmask in network byte order
 */
uint32_t tinypan_netif_get_netmask(void);

/**
 * @brief Process lwIP timeout/timer callbacks
 *
 * Must be called periodically while the interface is active.
 */
void tinypan_netif_process(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYPAN_LWIP_NETIF_H */
