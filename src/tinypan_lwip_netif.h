/*
 * TinyPAN lwIP Network Interface Adapter - Header
 * 
 * Bridges TinyPAN's transport layer (BNEP or SLIP) to lwIP's netif interface.
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
 * Sets up the lwIP netif. In BNEP mode, this configures an Ethernet-type
 * interface. In SLIP mode, it initializes a SLIP serial interface.
 * Should be called after tinypan_init() and before tinypan_start().
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
 * Called after the transport connection is established (BNEP setup complete,
 * or SLIP link ready) to obtain an IP address.
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
 * Called when the underlying transport connection state changes.
 * 
 * @param up true if link is up, false if down
 */
void tinypan_netif_set_link(bool up);

/**
 * @brief Process incoming data from the transport layer
 * 
 * In BNEP mode: reconstructs the Ethernet header from the parsed BNEP
 * addresses and passes the frame to lwIP.
 * In SLIP mode: enqueues raw bytes into the internal RX ring buffer
 * for lwIP's slipif to consume. The dst_addr, src_addr, and ethertype
 * parameters are ignored (pass NULL/0).
 * 
 * @param dst_addr   Destination MAC address (6 bytes, or NULL for SLIP mode)
 * @param src_addr   Source MAC address (6 bytes, or NULL for SLIP mode)
 * @param ethertype  EtherType (ignored in SLIP mode)
 * @param payload    Pointer to payload data (Ethernet payload or raw SLIP bytes)
 * @param payload_len Length of payload
 */
void tinypan_netif_input(const uint8_t* dst_addr, const uint8_t* src_addr,
                          uint16_t ethertype, const uint8_t* payload,
                          uint16_t payload_len);

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

/**
 * @brief Drain the transmission queue
 * 
 * Should be called when the hardware signals HAL_L2CAP_EVENT_CAN_SEND_NOW
 */
void tinypan_netif_drain_tx_queue(void);

/**
 * @brief Flush the transmission queue
 * 
 * Frees all queued packets to prevent memory leaks on disconnect.
 */
void tinypan_netif_flush_queue(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYPAN_LWIP_NETIF_H */
