/*
 * TinyPAN lwIP Network Interface Adapter - Implementation
 * 
 * Registers the lwIP netif (tp0) and delegates packet output/input
 * to the active transport backend via tinypan_transport_get().
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

#if TINYPAN_USE_BLE_SLIP
#include "lwip/ip.h"
#endif

#include "tinypan_transport.h"

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

/* The active transport layer handles TX/RX queues and sio interfaces */

/*
 * Private forward declaration for link status
 */
static void tinypan_netif_status_callback(struct netif* netif);
static err_t tinypan_netif_linkoutput(struct netif* netif, struct pbuf* p);

#if TINYPAN_USE_BLE_SLIP
static err_t tinypan_slip_output_wrapper(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr) {
    (void)ipaddr;
    return tinypan_netif_linkoutput(netif, p);
}
#endif

/* ============================================================================
 * Internal lwIP API mapping
 * ============================================================================ */

/**
 * @brief lwIP network interface initialization callback
 */
static err_t tinypan_netif_init_callback(struct netif* netif) {
    LWIP_ASSERT("netif != NULL", (netif != NULL));
    
    /* Set interface name */
    netif->name[0] = 't';
    netif->name[1] = 'p';
    
    const tinypan_transport_t* transport = tinypan_transport_get();
    
    if (transport == &transport_slip) {
        TINYPAN_LOG_INFO("Configuring lwIP netif for SLIP (BLE Companion Mode)");
        netif->mtu = TINYPAN_MTU;
        netif->flags = NETIF_FLAG_IGMP;
#if TINYPAN_USE_BLE_SLIP
        netif->output = tinypan_slip_output_wrapper;
#endif
    } else {
        TINYPAN_LOG_INFO("Configuring lwIP netif for Ethernet (Native Mode)");
        netif->output = etharp_output;
        netif->linkoutput = tinypan_netif_linkoutput;
        netif->hwaddr_len = 6;
        memcpy(netif->hwaddr, s_mac_addr, 6);
        netif->mtu = TINYPAN_MTU;
        netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    }

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

static err_t tinypan_netif_linkoutput(struct netif* netif, struct pbuf* p) {
    const tinypan_transport_t* transport = tinypan_transport_get();
    if (transport && transport->output) {
        return transport->output(netif, p);
    }
    return ERR_IF;
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
    
    /* Initialize lwIP stack core if requested and not already done */
    const tinypan_config_t* config = tinypan_internal_get_config();
    static bool s_lwip_initialized_by_us = false;
    if (config && config->auto_init_lwip && !s_lwip_initialized_by_us) {
        /* WARNING: In shared stack environments (ESP32), the OS usually calls this. */
        lwip_init();
        s_lwip_initialized_by_us = true;
    }
    
    /* Add our network interface */
    ip4_addr_t ipaddr, netmask, gateway;
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gateway, 0, 0, 0, 0);
    
#if TINYPAN_USE_BLE_SLIP
    if (netif_add(&s_netif, &ipaddr, &netmask, &gateway, NULL,
                  tinypan_netif_init_callback, ip_input) == NULL) {
#else
    if (netif_add(&s_netif, &ipaddr, &netmask, &gateway, NULL,
                  tinypan_netif_init_callback, ethernet_input) == NULL) {
#endif
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
    
    static bool s_dhcp_started = false;
    if (s_dhcp_started) {
        return 0; /* Auto-renewed by link_up */
    }
    
    TINYPAN_LOG_INFO("netif: Starting DHCP...");
    
    err_t err = dhcp_start(&s_netif);
    if (err != ERR_OK) {
        TINYPAN_LOG_ERROR("netif: DHCP start failed: %d", err);
        return -1;
    }
    
    s_dhcp_started = true;
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
    if (!s_initialized) {
        return;
    }
    
    /* In refactored transport model, the SLIP bytes are fed automatically into slipif
       via the transport layer `handle_incoming` -> slipif_process_rxqueue.
       This function is now ONLY meant to inject Ethernet frames into lwIP (from BNEP). */
    
    if (dst_addr == NULL || src_addr == NULL) {
        return;
    }
    
    uint16_t total_len = 14 + payload_len;
    TINYPAN_LOG_DEBUG("netif RX: %u bytes", total_len);
    
    /* Allocate a pbuf to hold the received data directly into lwIP pool */
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
    struct pbuf* p = pbuf_alloc(PBUF_RAW, total_len + ETH_PAD_SIZE, PBUF_POOL);
#else
    struct pbuf* p = pbuf_alloc(PBUF_RAW, total_len, PBUF_POOL);
#endif
    if (p == NULL) {
        TINYPAN_LOG_WARN("netif: Failed to allocate pbuf for RX");
        return;
    }

#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
    pbuf_remove_header(p, ETH_PAD_SIZE); /* Drop padding space temporarily for copying */
#endif

    /* Copy data directly into pbuf, avoiding intermediate array copies */
    /* pbuf_take_at handles traversing chained PBUF_POOL pbufs safely */
    pbuf_take_at(p, dst_addr, 6 /* BNEP_ETHER_ADDR_LEN */, 0);
    pbuf_take_at(p, src_addr, 6 /* BNEP_ETHER_ADDR_LEN */, 6);
    
    uint8_t type_bytes[2];
    type_bytes[0] = (uint8_t)(ethertype >> 8);
    type_bytes[1] = (uint8_t)(ethertype & 0xFF);
    pbuf_take_at(p, type_bytes, 2, 12);
    
    if (payload != NULL && payload_len > 0) {
        pbuf_take_at(p, payload, payload_len, 14);
    }
    
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
    pbuf_add_header(p, ETH_PAD_SIZE); /* Restore padding space before handing to lwIP */
#endif
    
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
    
    /* Process lwIP timers only if running in bare-metal (NO_SYS=1) mode.
     * In OS-mode (ESP32/Zephyr), lwIP's tcpip_thread handles its own timeouts. */
#if NO_SYS
    sys_check_timeouts();
#endif
}

void tinypan_netif_flush_queue(void) {
    const tinypan_transport_t* transport = tinypan_transport_get();
    if (transport && transport->flush_queues) {
        transport->flush_queues();
    }
}

void tinypan_netif_drain_tx_queue(void) {
    /* Called when radio is ready - now dispatched by transport's on_can_send_now
       but kept for public API compatibility. */
    const tinypan_transport_t* transport = tinypan_transport_get();
    if (transport && transport->on_can_send_now) {
        transport->on_can_send_now();
    }
}

/* ============================================================================
 * lwIP System Time Provider
 * ============================================================================ */

u32_t sys_now(void) {
    return hal_get_tick_ms();
}
