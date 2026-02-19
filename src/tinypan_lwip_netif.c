/*
 * TinyPAN lwIP Network Interface Adapter - Implementation
 * 
 * Bridges the BNEP layer to lwIP's netif interface.
 * This makes TinyPAN appear as an Ethernet interface to lwIP.
 */

#include "tinypan_lwip_netif.h"
#include "tinypan_bnep.h"
#include "../include/tinypan_config.h"
#include "../include/tinypan_hal.h"

/* lwIP includes */
#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include <string.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Interface name (appears as "tp0" in debug output) */
#define IFNAME0 't'
#define IFNAME1 'p'

/** MTU - standard Ethernet */
#define TINYPAN_MTU 1500

/* ============================================================================
 * Static State
 * ============================================================================ */

/** The lwIP network interface */
static struct netif s_netif;

/** Flag: is the netif initialized? */
static bool s_initialized = false;

/** Local MAC address (derived from Bluetooth address) */
static uint8_t s_mac_addr[6] = {0};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static err_t tinypan_netif_output(struct netif* netif, struct pbuf* p);
static err_t tinypan_netif_linkoutput(struct netif* netif, struct pbuf* p);
static void tinypan_netif_status_callback(struct netif* netif);

/* ============================================================================
 * lwIP Netif Initialization Callback
 * ============================================================================ */

/**
 * @brief lwIP netif initialization callback
 * 
 * Called by lwIP when adding the netif. Sets up the interface parameters.
 */
static err_t tinypan_netif_init_callback(struct netif* netif) {
    LWIP_ASSERT("netif != NULL", (netif != NULL));
    
    /* Set interface name */
    netif->name[0] = IFNAME0;
    netif->name[1] = IFNAME1;
    
    /* Set output functions */
    netif->output = etharp_output;              /* For IP packets */
    netif->linkoutput = tinypan_netif_linkoutput; /* For raw Ethernet frames */
    
    /* Set hardware address (MAC) */
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, s_mac_addr, 6);
    
    /* Set MTU */
    netif->mtu = TINYPAN_MTU;
    
    /* Set interface flags */
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    
    /* Enable link callback */
#if LWIP_NETIF_STATUS_CALLBACK
    netif_set_status_callback(netif, tinypan_netif_status_callback);
#endif
    
    TINYPAN_LOG_INFO("lwIP netif initialized: %c%c",
                      netif->name[0], netif->name[1]);
    
    return ERR_OK;
}

/* ============================================================================
 * Output Functions (Sending)
 * ============================================================================ */

/**
 * @brief Low-level output function - sends Ethernet frame over BNEP
 * 
 * Called by lwIP when it needs to send an Ethernet frame.
 * 
 * @param netif The network interface
 * @param p     Pbuf chain containing the Ethernet frame
 * @return ERR_OK on success, error code otherwise
 */
static err_t tinypan_netif_linkoutput(struct netif* netif, struct pbuf* p) {
    (void)netif;
    
    if (p == NULL) {
        return ERR_ARG;
    }
    
    /* Check if BNEP is connected */
    if (!bnep_is_connected()) {
        TINYPAN_LOG_DEBUG("netif: Cannot send - BNEP not connected");
        return ERR_CONN;
    }
    
    /* The pbuf contains a full Ethernet frame:
     * - Destination MAC (6 bytes)
     * - Source MAC (6 bytes)
     * - EtherType (2 bytes)
     * - Payload (variable)
     */
    
    if (p->tot_len < 14) {
        TINYPAN_LOG_WARN("netif: Frame too short: %u", p->tot_len);
        return ERR_ARG;
    }
    
    /* Extract Ethernet header */
    uint8_t* data = (uint8_t*)p->payload;
    uint8_t* dst_addr = &data[0];
    uint8_t* src_addr = &data[6];
    uint16_t ethertype = ((uint16_t)data[12] << 8) | data[13];
    
    /* Payload starts after 14-byte Ethernet header */
    uint8_t* payload = &data[14];
    uint16_t payload_len = (uint16_t)(p->tot_len - 14);
    
    TINYPAN_LOG_DEBUG("netif TX: dst=%02X:%02X:%02X:%02X:%02X:%02X type=0x%04X len=%u",
                       dst_addr[0], dst_addr[1], dst_addr[2],
                       dst_addr[3], dst_addr[4], dst_addr[5],
                       ethertype, payload_len);
    
    /* Handle chained pbufs by copying to contiguous buffer if needed */
    if (p->next != NULL) {
        /* Chained pbuf - need to flatten */
        static uint8_t tx_buf[TINYPAN_TX_BUFFER_SIZE];
        
        if (p->tot_len > sizeof(tx_buf) - 14) {
            TINYPAN_LOG_WARN("netif: Frame too large for buffer");
            return ERR_MEM;
        }
        
        /* Copy all pbuf segments */
        uint16_t offset = 0;
        for (struct pbuf* q = p; q != NULL; q = q->next) {
            memcpy(&tx_buf[offset], q->payload, q->len);
            offset += q->len;
        }
        
        /* Now send from contiguous buffer */
        payload = &tx_buf[14];
    }
    
    /* Send via BNEP */
    int result = bnep_send_ethernet_frame(dst_addr, src_addr, ethertype,
                                           payload, payload_len);
    
    if (result < 0) {
        TINYPAN_LOG_WARN("netif: BNEP send failed: %d", result);
        return ERR_IF;
    }
    
    if (result > 0) {
        /* Busy - try again later */
        return ERR_WOULDBLOCK;
    }
    
    return ERR_OK;
}

/* ============================================================================
 * Status Callback
 * ============================================================================ */

/**
 * @brief Called when netif status changes (IP assigned, etc.)
 */
static void tinypan_netif_status_callback(struct netif* netif) {
    if (netif_is_up(netif)) {
        ip4_addr_t* ip = netif_ip4_addr(netif);
        ip4_addr_t* gw = netif_ip4_gw(netif);
        ip4_addr_t* mask = netif_ip4_netmask(netif);
        
        if (ip->addr != 0) {
            TINYPAN_LOG_INFO("netif: IP acquired!");
            TINYPAN_LOG_INFO("  IP:      %d.%d.%d.%d",
                             ip4_addr1(ip), ip4_addr2(ip),
                             ip4_addr3(ip), ip4_addr4(ip));
            TINYPAN_LOG_INFO("  Gateway: %d.%d.%d.%d",
                             ip4_addr1(gw), ip4_addr2(gw),
                             ip4_addr3(gw), ip4_addr4(gw));
            TINYPAN_LOG_INFO("  Netmask: %d.%d.%d.%d",
                             ip4_addr1(mask), ip4_addr2(mask),
                             ip4_addr3(mask), ip4_addr4(mask));
            
            /* Notify the main TinyPAN module */
            extern void tinypan_internal_set_ip(uint32_t ip, uint32_t netmask, 
                                                 uint32_t gw, uint32_t dns);
            tinypan_internal_set_ip(ip->addr, mask->addr, gw->addr, 0);
        }
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

int tinypan_netif_init(void) {
    if (s_initialized) {
        TINYPAN_LOG_WARN("netif: Already initialized");
        return 0;
    }
    
    /* Get MAC address from Bluetooth address */
    hal_get_local_bd_addr(s_mac_addr);
    
    /* Set the locally administered bit to make it a valid unicast MAC */
    /* Bit 1 of first byte = locally administered */
    s_mac_addr[0] |= 0x02;
    /* Clear multicast bit */
    s_mac_addr[0] &= ~0x01;
    
    TINYPAN_LOG_INFO("netif: MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
                      s_mac_addr[0], s_mac_addr[1], s_mac_addr[2],
                      s_mac_addr[3], s_mac_addr[4], s_mac_addr[5]);
    
    /* Initialize lwIP */
    lwip_init();
    
    /* Add our network interface */
    ip4_addr_t ipaddr, netmask, gateway;
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gateway, 0, 0, 0, 0);
    
    if (netif_add(&s_netif, &ipaddr, &netmask, &gateway, NULL,
                  tinypan_netif_init_callback, ethernet_input) == NULL) {
        TINYPAN_LOG_ERROR("netif: Failed to add interface");
        return -1;
    }
    
    /* Set as default interface */
    netif_set_default(&s_netif);
    
    s_initialized = true;
    
    TINYPAN_LOG_INFO("netif: Initialized successfully");
    return 0;
}

void tinypan_netif_deinit(void) {
    if (!s_initialized) {
        return;
    }
    
    /* Stop DHCP if running */
    dhcp_stop(&s_netif);
    
    /* Remove the interface */
    netif_remove(&s_netif);
    
    s_initialized = false;
    TINYPAN_LOG_INFO("netif: De-initialized");
}

int tinypan_netif_start_dhcp(void) {
    if (!s_initialized) {
        TINYPAN_LOG_ERROR("netif: Not initialized");
        return -1;
    }
    
    TINYPAN_LOG_INFO("netif: Starting DHCP...");
    
    err_t err = dhcp_start(&s_netif);
    if (err != ERR_OK) {
        TINYPAN_LOG_ERROR("netif: DHCP start failed: %d", err);
        return -1;
    }
    
    return 0;
}

void tinypan_netif_stop_dhcp(void) {
    if (!s_initialized) {
        return;
    }
    
    dhcp_stop(&s_netif);
    TINYPAN_LOG_INFO("netif: DHCP stopped");
}

void tinypan_netif_set_link(bool up) {
    if (!s_initialized) {
        return;
    }
    
    if (up) {
        netif_set_link_up(&s_netif);
        netif_set_up(&s_netif);
        TINYPAN_LOG_INFO("netif: Link UP");
    } else {
        netif_set_link_down(&s_netif);
        netif_set_down(&s_netif);
        TINYPAN_LOG_INFO("netif: Link DOWN");
    }
}

void tinypan_netif_input(const uint8_t* data, uint16_t len) {
    if (!s_initialized || data == NULL || len == 0) {
        return;
    }
    
    TINYPAN_LOG_DEBUG("netif RX: %u bytes", len);
    
    /* Allocate a pbuf to hold the received data */
    struct pbuf* p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p == NULL) {
        TINYPAN_LOG_WARN("netif: Failed to allocate pbuf for RX");
        return;
    }
    
    /* Copy data into pbuf */
    pbuf_take(p, data, len);
    
    /* Pass to lwIP's Ethernet input */
    if (s_netif.input(p, &s_netif) != ERR_OK) {
        TINYPAN_LOG_WARN("netif: Input processing failed");
        pbuf_free(p);
    }
}

struct netif* tinypan_netif_get(void) {
    return s_initialized ? &s_netif : NULL;
}

bool tinypan_netif_has_ip(void) {
    if (!s_initialized) {
        return false;
    }
    return netif_ip4_addr(&s_netif)->addr != 0;
}

uint32_t tinypan_netif_get_ip(void) {
    if (!s_initialized) {
        return 0;
    }
    return netif_ip4_addr(&s_netif)->addr;
}

uint32_t tinypan_netif_get_gateway(void) {
    if (!s_initialized) {
        return 0;
    }
    return netif_ip4_gw(&s_netif)->addr;
}

uint32_t tinypan_netif_get_netmask(void) {
    if (!s_initialized) {
        return 0;
    }
    return netif_ip4_netmask(&s_netif)->addr;
}

/* ============================================================================
 * lwIP Timeout Processing
 * ============================================================================ */

/**
 * @brief Process lwIP timers
 * 
 * Must be called periodically from the main loop.
 * This handles DHCP retries, ARP timeouts, TCP timers, etc.
 */
void tinypan_netif_process(void) {
    if (!s_initialized) {
        return;
    }
    
    /* Process lwIP timeouts */
    sys_check_timeouts();
}
