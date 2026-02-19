/*
 * TinyPAN DHCP Simulation
 * 
 * Helper functions to simulate DHCP packet exchange for testing
 * without real network hardware.
 */

#ifndef TINYPAN_DHCP_SIM_H
#define TINYPAN_DHCP_SIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DHCP Constants
 * ============================================================================ */

#define DHCP_OP_REQUEST         1
#define DHCP_OP_REPLY           2

#define DHCP_HTYPE_ETHERNET     1

#define DHCP_FLAG_BROADCAST     0x8000

/* DHCP Message Types (Option 53) */
#define DHCP_DISCOVER           1
#define DHCP_OFFER              2
#define DHCP_REQUEST            3
#define DHCP_DECLINE            4
#define DHCP_ACK                5
#define DHCP_NAK                6
#define DHCP_RELEASE            7
#define DHCP_INFORM             8

/* DHCP Options */
#define DHCP_OPTION_MESSAGE_TYPE        53
#define DHCP_OPTION_SERVER_ID           54
#define DHCP_OPTION_LEASE_TIME          51
#define DHCP_OPTION_SUBNET_MASK         1
#define DHCP_OPTION_ROUTER              3
#define DHCP_OPTION_DNS                 6
#define DHCP_OPTION_END                 255

/* Ports */
#define DHCP_SERVER_PORT        67
#define DHCP_CLIENT_PORT        68

/* ============================================================================
 * Simulated Network Configuration
 * ============================================================================ */

/** Simulated network configuration */
typedef struct {
    uint32_t client_ip;     /**< IP to offer to client */
    uint32_t server_ip;     /**< DHCP server (NAP) IP */
    uint32_t gateway_ip;    /**< Gateway IP (usually same as server) */
    uint32_t netmask;       /**< Subnet mask */
    uint32_t dns_ip;        /**< DNS server */
    uint32_t lease_time;    /**< Lease time in seconds */
    uint8_t  server_mac[6]; /**< Server MAC address */
} dhcp_sim_config_t;

/**
 * @brief Get default simulation config
 * 
 * Default network:
 *   Client: 192.168.44.2
 *   Server/Gateway: 192.168.44.1
 *   Netmask: 255.255.255.0
 */
void dhcp_sim_get_default_config(dhcp_sim_config_t* config);

/**
 * @brief Build a DHCP Offer packet
 * 
 * @param buffer      Buffer to write packet to
 * @param buffer_size Size of buffer
 * @param config      Network configuration
 * @param xid         Transaction ID from DISCOVER
 * @param client_mac  Client's MAC address
 * @return Length of packet, or negative on error
 */
int dhcp_sim_build_offer(uint8_t* buffer, uint16_t buffer_size,
                          const dhcp_sim_config_t* config,
                          uint32_t xid,
                          const uint8_t client_mac[6]);

/**
 * @brief Build a DHCP ACK packet
 * 
 * @param buffer      Buffer to write packet to
 * @param buffer_size Size of buffer
 * @param config      Network configuration
 * @param xid         Transaction ID from REQUEST
 * @param client_mac  Client's MAC address
 * @return Length of packet, or negative on error
 */
int dhcp_sim_build_ack(uint8_t* buffer, uint16_t buffer_size,
                        const dhcp_sim_config_t* config,
                        uint32_t xid,
                        const uint8_t client_mac[6]);

/**
 * @brief Build a complete BNEP-wrapped DHCP packet
 * 
 * This wraps the DHCP packet in UDP/IP/Ethernet/BNEP layers.
 * 
 * @param buffer      Buffer to write packet to
 * @param buffer_size Size of buffer
 * @param src_mac     Source MAC (server)
 * @param dst_mac     Destination MAC (client or broadcast)
 * @param src_ip      Source IP (server)
 * @param dst_ip      Destination IP (client or broadcast)
 * @param dhcp_data   DHCP payload
 * @param dhcp_len    DHCP payload length
 * @return Length of complete packet, or negative on error
 */
int dhcp_sim_build_bnep_packet(uint8_t* buffer, uint16_t buffer_size,
                                const uint8_t src_mac[6],
                                const uint8_t dst_mac[6],
                                uint32_t src_ip,
                                uint32_t dst_ip,
                                const uint8_t* dhcp_data,
                                uint16_t dhcp_len);

/**
 * @brief Check if a received BNEP packet is a DHCP Discover
 * 
 * @param data        Received BNEP packet
 * @param len         Packet length
 * @param xid         [out] Transaction ID
 * @param client_mac  [out] Client MAC address
 * @return 1 if DHCP Discover, 0 otherwise
 */
int dhcp_sim_is_discover(const uint8_t* data, uint16_t len,
                          uint32_t* xid, uint8_t client_mac[6]);

/**
 * @brief Check if a received BNEP packet is a DHCP Request
 * 
 * @param data        Received BNEP packet
 * @param len         Packet length
 * @param xid         [out] Transaction ID
 * @return 1 if DHCP Request, 0 otherwise
 */
int dhcp_sim_is_request(const uint8_t* data, uint16_t len, uint32_t* xid);

#ifdef __cplusplus
}
#endif

#endif /* TINYPAN_DHCP_SIM_H */
