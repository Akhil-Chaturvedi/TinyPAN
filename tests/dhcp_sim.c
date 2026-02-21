/*
 * TinyPAN DHCP Simulation - Implementation
 * 
 * Builds realistic DHCP packets for testing.
 */

#include "dhcp_sim.h"
#include <string.h>

/* ============================================================================
 * Helper macros
 * ============================================================================ */

#define WRITE_BE16(buf, val) do { (buf)[0] = ((val) >> 8) & 0xFF; (buf)[1] = (val) & 0xFF; } while(0)
#define WRITE_BE32(buf, val) do { (buf)[0] = ((val) >> 24) & 0xFF; (buf)[1] = ((val) >> 16) & 0xFF; \
                                   (buf)[2] = ((val) >> 8) & 0xFF; (buf)[3] = (val) & 0xFF; } while(0)

#define READ_BE32(buf) (((uint32_t)(buf)[0] << 24) | ((uint32_t)(buf)[1] << 16) | \
                        ((uint32_t)(buf)[2] << 8) | (buf)[3])

/* DHCP Magic Cookie */
static const uint8_t DHCP_MAGIC[] = {0x63, 0x82, 0x53, 0x63};

/* ============================================================================
 * Implementation
 * ============================================================================ */

void dhcp_sim_get_default_config(dhcp_sim_config_t* config) {
    if (config == NULL) return;
    
    /* 192.168.44.x network */
    config->client_ip  = 0xC0A82C02;  /* 192.168.44.2 */
    config->server_ip  = 0xC0A82C01;  /* 192.168.44.1 */
    config->gateway_ip = 0xC0A82C01;  /* 192.168.44.1 */
    config->netmask    = 0xFFFFFF00;  /* 255.255.255.0 */
    config->dns_ip     = 0x08080808;  /* 8.8.8.8 */
    config->lease_time = 86400;       /* 24 hours */
    
    /* NAP MAC */
    config->server_mac[0] = 0xAA;
    config->server_mac[1] = 0xBB;
    config->server_mac[2] = 0xCC;
    config->server_mac[3] = 0xDD;
    config->server_mac[4] = 0xEE;
    config->server_mac[5] = 0xFF;
}

/**
 * @brief Build DHCP message structure (common for OFFER and ACK)
 */
static int build_dhcp_message(uint8_t* buffer, uint16_t buffer_size,
                               const dhcp_sim_config_t* config,
                               uint32_t xid,
                               const uint8_t client_mac[6],
                               uint8_t msg_type) {
    if (buffer_size < 300) {
        return -1;  /* Need at least this much */
    }
    
    memset(buffer, 0, buffer_size);
    
    uint16_t pos = 0;
    
    /* DHCP/BOOTP Header (236 bytes fixed) */
    buffer[pos++] = DHCP_OP_REPLY;              /* op: reply */
    buffer[pos++] = DHCP_HTYPE_ETHERNET;        /* htype: Ethernet */
    buffer[pos++] = 6;                          /* hlen: 6 bytes MAC */
    buffer[pos++] = 0;                          /* hops */
    
    /* Transaction ID */
    WRITE_BE32(&buffer[pos], xid);
    pos += 4;
    
    /* secs, flags */
    buffer[pos++] = 0; buffer[pos++] = 0;       /* secs */
    buffer[pos++] = 0; buffer[pos++] = 0;       /* flags */
    
    /* ciaddr - client IP (0 for OFFER) */
    pos += 4;
    
    /* yiaddr - your (client) IP address */
    WRITE_BE32(&buffer[pos], config->client_ip);
    pos += 4;
    
    /* siaddr - server IP */
    WRITE_BE32(&buffer[pos], config->server_ip);
    pos += 4;
    
    /* giaddr - gateway IP (relay) */
    pos += 4;
    
    /* chaddr - client hardware address (16 bytes) */
    memcpy(&buffer[pos], client_mac, 6);
    pos += 16;
    
    /* sname - server hostname (64 bytes) */
    pos += 64;
    
    /* file - boot filename (128 bytes) */
    pos += 128;
    
    /* Magic cookie */
    memcpy(&buffer[pos], DHCP_MAGIC, 4);
    pos += 4;
    
    /* Options */
    
    /* Option 53: DHCP Message Type */
    buffer[pos++] = DHCP_OPTION_MESSAGE_TYPE;
    buffer[pos++] = 1;  /* length */
    buffer[pos++] = msg_type;
    
    /* Option 54: Server Identifier */
    buffer[pos++] = DHCP_OPTION_SERVER_ID;
    buffer[pos++] = 4;
    WRITE_BE32(&buffer[pos], config->server_ip);
    pos += 4;
    
    /* Option 51: Lease Time */
    buffer[pos++] = DHCP_OPTION_LEASE_TIME;
    buffer[pos++] = 4;
    WRITE_BE32(&buffer[pos], config->lease_time);
    pos += 4;
    
    /* Option 1: Subnet Mask */
    buffer[pos++] = DHCP_OPTION_SUBNET_MASK;
    buffer[pos++] = 4;
    WRITE_BE32(&buffer[pos], config->netmask);
    pos += 4;
    
    /* Option 3: Router */
    buffer[pos++] = DHCP_OPTION_ROUTER;
    buffer[pos++] = 4;
    WRITE_BE32(&buffer[pos], config->gateway_ip);
    pos += 4;
    
    /* Option 6: DNS */
    buffer[pos++] = DHCP_OPTION_DNS;
    buffer[pos++] = 4;
    WRITE_BE32(&buffer[pos], config->dns_ip);
    pos += 4;
    
    /* End option */
    buffer[pos++] = DHCP_OPTION_END;
    
    return (int)pos;
}

int dhcp_sim_build_offer(uint8_t* buffer, uint16_t buffer_size,
                          const dhcp_sim_config_t* config,
                          uint32_t xid,
                          const uint8_t client_mac[6]) {
    return build_dhcp_message(buffer, buffer_size, config, xid, client_mac, DHCP_OFFER);
}

int dhcp_sim_build_ack(uint8_t* buffer, uint16_t buffer_size,
                        const dhcp_sim_config_t* config,
                        uint32_t xid,
                        const uint8_t client_mac[6]) {
    return build_dhcp_message(buffer, buffer_size, config, xid, client_mac, DHCP_ACK);
}

static uint16_t calc_checksum(const uint8_t* ptr, int len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint16_t)ptr[0] << 8) | ptr[1];
        ptr += 2;
        len -= 2;
    }
    if (len > 0) {
        sum += ((uint16_t)ptr[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

int dhcp_sim_build_bnep_packet(uint8_t* buffer, uint16_t buffer_size,
                                const uint8_t src_mac[6],
                                const uint8_t dst_mac[6],
                                uint32_t src_ip,
                                uint32_t dst_ip,
                                const uint8_t* dhcp_data,
                                uint16_t dhcp_len) {
    /*
     * Build a complete BNEP packet with:
     * - BNEP header (1 byte for compressed, or 15 for general)
     * - Ethernet addresses (if general)
     * - IP header (20 bytes)
     * - UDP header (8 bytes)
     * - DHCP payload
     */
    
    /* Use general Ethernet format for clarity */
    uint16_t bnep_header = 15;  /* type + dst + src + ethertype */
    uint16_t ip_header = 20;
    uint16_t udp_header = 8;
    uint16_t total = bnep_header + ip_header + udp_header + dhcp_len;
    
    if (buffer_size < total) {
        return -1;
    }
    
    memset(buffer, 0, total);
    
    uint16_t pos = 0;
    
    /* BNEP Header */
    buffer[pos++] = 0x00;  /* General Ethernet, no extension */
    memcpy(&buffer[pos], dst_mac, 6);
    pos += 6;
    memcpy(&buffer[pos], src_mac, 6);
    pos += 6;
    buffer[pos++] = 0x08;  /* EtherType: IPv4 */
    buffer[pos++] = 0x00;
    
    /* IP Header */
    buffer[pos++] = 0x45;  /* Version 4, IHL 5 */
    buffer[pos++] = 0x00;  /* DSCP/ECN */
    uint16_t ip_total_len = ip_header + udp_header + dhcp_len;
    WRITE_BE16(&buffer[pos], ip_total_len);
    pos += 2;
    buffer[pos++] = 0x00; buffer[pos++] = 0x00;  /* Identification */
    buffer[pos++] = 0x00; buffer[pos++] = 0x00;  /* Flags/Fragment */
    buffer[pos++] = 64;   /* TTL */
    buffer[pos++] = 17;   /* Protocol: UDP */
    
    uint16_t checksum_pos = pos;
    buffer[pos++] = 0x00; buffer[pos++] = 0x00;  /* Header checksum placeholder */
    WRITE_BE32(&buffer[pos], src_ip);
    pos += 4;
    WRITE_BE32(&buffer[pos], dst_ip);
    pos += 4;
    
    /* Calculate IPv4 header checksum */
    uint16_t ip_chksum = calc_checksum(&buffer[15], 20); /* 15 is bnep offset */
    WRITE_BE16(&buffer[checksum_pos], ip_chksum);
    
    /* UDP Header */
    WRITE_BE16(&buffer[pos], DHCP_SERVER_PORT);  /* Source port */
    pos += 2;
    WRITE_BE16(&buffer[pos], DHCP_CLIENT_PORT);  /* Dest port */
    pos += 2;
    uint16_t udp_len = udp_header + dhcp_len;
    WRITE_BE16(&buffer[pos], udp_len);
    pos += 2;
    buffer[pos++] = 0x00; buffer[pos++] = 0x00;  /* UDP checksum (optional for IPv4) */
    
    /* DHCP Payload */
    memcpy(&buffer[pos], dhcp_data, dhcp_len);
    pos += dhcp_len;
    
    return (int)pos;
}

int dhcp_sim_is_discover(const uint8_t* data, uint16_t len,
                          uint32_t* xid, uint8_t client_mac[6]) {
    /*
     * Check for DHCP Discover in a BNEP packet:
     * - BNEP header (1 or 15 bytes)
     * - IP header (20+ bytes) 
     * - UDP header (8 bytes)
     * - DHCP message
     */
    
    if (data == NULL || len < 50) {
        return 0;
    }
    
    /* Check BNEP type */
    uint8_t bnep_type = data[0] & 0x7F;
    uint16_t eth_offset;
    
    if (bnep_type == 0x00) {
        eth_offset = 15;  /* General Ethernet */
    } else if (bnep_type == 0x02) {
        eth_offset = 3;   /* Compressed */
    } else {
        return 0;
    }
    
    if (len < eth_offset + 28) {  /* Need at least IP + UDP headers */
        return 0;
    }
    
    /* Check EtherType is IPv4 */
    uint16_t ethertype_offset = (bnep_type == 0x00) ? 13 : 1;
    if (data[ethertype_offset] != 0x08 || data[ethertype_offset + 1] != 0x00) {
        return 0;
    }
    
    /* Check IP protocol is UDP */
    uint16_t ip_offset = eth_offset;
    if (data[ip_offset + 9] != 17) {  /* Protocol field */
        return 0;
    }
    
    /* Check UDP dest port is 67 (DHCP server) */
    uint16_t udp_offset = ip_offset + 20;  /* Assuming no IP options */
    if (len < udp_offset + 8) {
        return 0;
    }
    
    uint16_t dest_port = ((uint16_t)data[udp_offset + 2] << 8) | data[udp_offset + 3];
    if (dest_port != DHCP_SERVER_PORT) {
        return 0;
    }
    
    /* Check DHCP message type */
    uint16_t dhcp_offset = udp_offset + 8;
    if (len < dhcp_offset + 240) {  /* Minimum DHCP message */
        return 0;
    }
    
    /* Check magic cookie */
    if (memcmp(&data[dhcp_offset + 236], DHCP_MAGIC, 4) != 0) {
        return 0;
    }
    
    /* Extract XID */
    if (xid != NULL) {
        *xid = READ_BE32(&data[dhcp_offset + 4]);
    }
    
    /* Extract client MAC */
    if (client_mac != NULL) {
        memcpy(client_mac, &data[dhcp_offset + 28], 6);
    }
    
    /* Check option 53 for DISCOVER */
    uint16_t opt_offset = dhcp_offset + 240;
    while (opt_offset < len - 2) {
        uint8_t opt_type = data[opt_offset];
        if (opt_type == DHCP_OPTION_END) break;
        if (opt_type == 0) { opt_offset++; continue; }  /* Padding */
        
        uint8_t opt_len = data[opt_offset + 1];
        
        if (opt_type == DHCP_OPTION_MESSAGE_TYPE && opt_len >= 1) {
            return (data[opt_offset + 2] == DHCP_DISCOVER) ? 1 : 0;
        }
        
        opt_offset += 2 + opt_len;
    }
    
    return 0;
}

int dhcp_sim_is_request(const uint8_t* data, uint16_t len, uint32_t* xid) {
    uint8_t client_mac[6];
    
    /* Reuse discover function logic but check for REQUEST */
    if (dhcp_sim_is_discover(data, len, xid, client_mac)) {
        /* It was a DISCOVER, not REQUEST */
        return 0;
    }
    
    /* Same parsing, different message type check */
    if (data == NULL || len < 50) {
        return 0;
    }
    
    uint8_t bnep_type = data[0] & 0x7F;
    uint16_t eth_offset = (bnep_type == 0x00) ? 15 : 3;
    
    if (len < eth_offset + 28) {
        return 0;
    }
    
    uint16_t ip_offset = eth_offset;
    uint16_t udp_offset = ip_offset + 20;
    uint16_t dhcp_offset = udp_offset + 8;
    
    if (len < dhcp_offset + 240) {
        return 0;
    }
    
    if (xid != NULL) {
        *xid = READ_BE32(&data[dhcp_offset + 4]);
    }
    
    /* Check option 53 for REQUEST */
    uint16_t opt_offset = dhcp_offset + 240;
    while (opt_offset < len - 2) {
        uint8_t opt_type = data[opt_offset];
        if (opt_type == DHCP_OPTION_END) break;
        if (opt_type == 0) { opt_offset++; continue; }
        
        uint8_t opt_len = data[opt_offset + 1];
        
        if (opt_type == DHCP_OPTION_MESSAGE_TYPE && opt_len >= 1) {
            return (data[opt_offset + 2] == DHCP_REQUEST) ? 1 : 0;
        }
        
        opt_offset += 2 + opt_len;
    }
    
    return 0;
}
