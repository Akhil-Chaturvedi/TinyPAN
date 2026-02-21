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
#include "tinypan_internal.h"

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
#include "netif/ethernet.h"

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

/* TX Queue State for handling L2CAP Busy (`ERR_WOULDBLOCK`) */
#define TINYPAN_TX_QUEUE_LEN 8
static struct pbuf* s_tx_queue[TINYPAN_TX_QUEUE_LEN] = {0};
static uint8_t s_tx_queue_head = 0;
static uint8_t s_tx_queue_tail = 0;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */


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
static int do_send_pbuf(struct pbuf* q) {
    /* Extract Ethernet header */
    uint8_t* eth_hdr = (uint8_t*)q->payload;
    uint8_t dst_addr[6], src_addr[6];
    memcpy(dst_addr, &eth_hdr[0], 6);
    memcpy(src_addr, &eth_hdr[6], 6);
    uint16_t ethertype = ((uint16_t)eth_hdr[12] << 8) | eth_hdr[13];
    
    /* IP payload starts immediately after the 14-byte Ethernet header */
    uint8_t* ip_payload = eth_hdr + 14;
    uint16_t ip_len = q->tot_len - 14;
    
    /* Send via BNEP. The BNEP layer backs up the ip_payload pointer
       into the PBUF_LINK_ENCAPSULATION_HLEN headroom, writes the BNEP
       header there, and passes a single contiguous pointer to the HAL. */
    return bnep_send_ethernet_frame(dst_addr, src_addr, ethertype, ip_payload, ip_len);
}

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
    
    if (p->tot_len < 14) {
        TINYPAN_LOG_WARN("netif: Frame too short: %u", p->tot_len);
        return ERR_ARG;
    }
    
    struct pbuf* q = NULL;
    
    /* If the pbuf is chained, flatten it into a contiguous PBUF_RAM block
       once. If it is already a single segment, just take a reference.
       Either way, the queued pointer is contiguous and ready to send
       without further copies on dequeue. */
    if (p->next != NULL) {
        q = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
        if (q == NULL) {
            TINYPAN_LOG_WARN("netif: Failed to clone pbuf");
            return ERR_MEM;
        }
    } else {
        q = p;
        pbuf_ref(q);
    }
    
    /* If the queue had items, we MUST queue to maintain order */
    bool queue_was_empty = (s_tx_queue_head == s_tx_queue_tail);
    
    if (queue_was_empty) {
        int result = do_send_pbuf(q);
        if (result == 0) {
            pbuf_free(q);
            return ERR_OK;
        } else if (result < 0) {
            pbuf_free(q);
            return ERR_IF;
        }
        /* result > 0 means ERR_WOULDBLOCK. Fall through to queueing. */
    }
    
    /* Queue the pbuf (q is already contiguous and ref'd/cloned) */
    uint8_t next_tail = (s_tx_queue_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    if (next_tail == s_tx_queue_head) {
        TINYPAN_LOG_WARN("netif: TX queue full, dropping packet");
        pbuf_free(q);
        return ERR_MEM;
    }
    
    TINYPAN_LOG_DEBUG("netif: L2CAP busy, queueing pbuf (len=%u)", q->tot_len);
    
    s_tx_queue[s_tx_queue_tail] = q;
    s_tx_queue_tail = next_tail;
    
    /* Return ERR_OK to lwIP so it doesn't drop the packet */
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
        const ip4_addr_t* ip = netif_ip4_addr(netif);
        const ip4_addr_t* gw = netif_ip4_gw(netif);
        const ip4_addr_t* mask = netif_ip4_netmask(netif);
        
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
    
    /* Initialize lwIP stack core (only once globally) */
    static bool s_lwip_initialized = false;
    if (!s_lwip_initialized) {
        lwip_init();
        s_lwip_initialized = true;
    }
    
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

void tinypan_netif_input(const uint8_t* dst_addr, const uint8_t* src_addr,
                          uint16_t ethertype, const uint8_t* payload,
                          uint16_t payload_len) {
    if (!s_initialized || dst_addr == NULL || src_addr == NULL) {
        return;
    }
    
    uint16_t total_len = 14 + payload_len;
    TINYPAN_LOG_DEBUG("netif RX: %u bytes", total_len);
    
    /* Allocate a pbuf to hold the received data directly into lwIP pool */
    struct pbuf* p = pbuf_alloc(PBUF_RAW, total_len, PBUF_POOL);
    if (p == NULL) {
        TINYPAN_LOG_WARN("netif: Failed to allocate pbuf for RX");
        return;
    }
    
    /* Copy data directly into pbuf, avoiding intermediate array copies */
    /* pbuf_take_at handles traversing chained PBUF_POOL pbufs safely */
    pbuf_take_at(p, dst_addr, BNEP_ETHER_ADDR_LEN, 0);
    pbuf_take_at(p, src_addr, BNEP_ETHER_ADDR_LEN, 6);
    
    uint8_t type_bytes[2];
    type_bytes[0] = (uint8_t)(ethertype >> 8);
    type_bytes[1] = (uint8_t)(ethertype & 0xFF);
    pbuf_take_at(p, type_bytes, 2, 12);
    
    if (payload != NULL && payload_len > 0) {
        pbuf_take_at(p, payload, payload_len, 14);
    }
    
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
    
    /* Process lwIP timers */
    sys_check_timeouts();
}

void tinypan_netif_drain_tx_queue(void) {
    if (!s_initialized) {
        return;
    }
    
    while (s_tx_queue_head != s_tx_queue_tail) {
        struct pbuf* p = s_tx_queue[s_tx_queue_head];
        
        int result = do_send_pbuf(p);
        if (result > 0) {
            /* Still blocked (busy) - stop draining and leave at head */
            break;
        }
        
        /* Successfully sent (or fatal error) - pop from queue */
        s_tx_queue_head = (s_tx_queue_head + 1) % TINYPAN_TX_QUEUE_LEN;
        pbuf_free(p); /* Release the ref we took when queueing */
    }
}

/* ============================================================================
 * lwIP System Time Provider
 * ============================================================================ */

u32_t sys_now(void) {
    return hal_get_tick_ms();
}
