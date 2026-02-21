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

#if TINYPAN_USE_BLE_SLIP
#include "netif/slipif.h"
#endif

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

/* TX Queue: holds pre-encapsulated BNEP frames when the radio is busy */
static struct pbuf* s_tx_queue[TINYPAN_TX_QUEUE_LEN] = {0};
static uint8_t s_tx_queue_head = 0;
static uint8_t s_tx_queue_tail = 0;

#if TINYPAN_USE_BLE_SLIP
/* SLIP expects to read byte-by-byte from a serial port via sio_read().
   Since HAL delivers chunks via callbacks, we buffer them here. */
static uint8_t s_rx_queue[TINYPAN_RX_BUFFER_SIZE];
static uint16_t s_rx_queue_head = 0;
static uint16_t s_rx_queue_tail = 0;

/* Standard lwIP serial IO adapter for SLIP */
#include "lwip/sio.h"

sio_fd_t sio_open(u8_t devnum) {
    (void)devnum;
    return (sio_fd_t)1; /* Dummy handle */
}

u32_t sio_read(sio_fd_t fd, u8_t *data, u32_t len) {
    (void)fd;
    u32_t copied = 0;
    while (copied < len && s_rx_queue_head != s_rx_queue_tail) {
        data[copied++] = s_rx_queue[s_rx_queue_tail];
        s_rx_queue_tail = (s_rx_queue_tail + 1) % TINYPAN_RX_BUFFER_SIZE;
    }
    return copied;
}

void sio_send(u8_t c, sio_fd_t fd) {
    /* netif->linkoutput is used instead of sio_send in slipif */
    (void)c;
    (void)fd;
}
#endif

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
    
#if TINYPAN_USE_BLE_SLIP
    TINYPAN_LOG_INFO("Configuring lwIP netif for SLIP (BLE Companion Mode)");
    /* For SLIP, we just need to set the MTU and flags.
       The output functions (netif->output) are set by slipif_init automatically. */
    netif->mtu = TINYPAN_MTU;
    netif->flags = NETIF_FLAG_IGMP; /* SLIP typically doesn't need ETHARP or BROADCAST */
#else
    TINYPAN_LOG_INFO("Configuring lwIP netif for Ethernet (Native BNEP Mode)");
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
#endif

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
    (void)netif;
    
    if (p == NULL) {
        return ERR_ARG;
    }
    
#if !TINYPAN_USE_BLE_SLIP
#if !TINYPAN_USE_BLE_SLIP
    /* Check if BNEP is connected */
    if (!bnep_is_connected()) {
        TINYPAN_LOG_DEBUG("netif: Cannot send - BNEP not connected");
        return ERR_CONN;
    }
    
    if (p->tot_len < 14) {
        TINYPAN_LOG_WARN("netif: Frame too short: %u", p->tot_len);
        return ERR_ARG;
    }
#endif
#endif
    
    /* Fast path bypass: if SLIP mode is enabled, p is already just a 
       contiguous stream of escaped SLIP bytes generated by slipif_output */
#if TINYPAN_USE_BLE_SLIP
    bool can_send_now = (s_tx_queue_head == s_tx_queue_tail) && 
                        hal_bt_l2cap_can_send();

    if (can_send_now) {
        /* If chained pbuf, flatten it. SLIP might produce chained pbufs. */
        struct pbuf* send_p = p;
        if (p->next != NULL) {
            send_p = pbuf_clone(PBUF_LINK, PBUF_RAM, p);
            if (send_p == NULL) {
                return ERR_MEM;
            }
        }
        
        int hal_result = hal_bt_l2cap_send(send_p->payload, send_p->tot_len);
        
        if (send_p != p) {
            pbuf_free(send_p);
        }
        
        if (hal_result >= 0) {
            return ERR_OK;
        } else if (hal_result != TINYPAN_ERR_BUSY) {
            return ERR_IF;
        }
        /* Fallthrough to queue */
    }
#else
    /* BNEP Mode: Check if we can use the true zero-copy fast path.
       It must be ready, the queue must be empty, AND the pbuf must be contiguous! */
    bool can_send_now = (s_tx_queue_head == s_tx_queue_tail) && 
                        hal_bt_l2cap_can_send() && 
                        (p->next == NULL);
    
    if (can_send_now) {
        /* FAST PATH: ZERO ALLOCATIONS, ZERO COPIES
           We manipulate the existing pbuf in-place, send it, and then revert the pointers
           so lwIP's etharp_output can cleanly strip the 14-byte MAC header afterwards. */
        uint8_t* eth_hdr = (uint8_t*)p->payload;
        uint8_t dst_addr[6], src_addr[6];
        memcpy(dst_addr, &eth_hdr[0], 6);
        memcpy(src_addr, &eth_hdr[6], 6);
        uint16_t ethertype = ((uint16_t)eth_hdr[12] << 8) | eth_hdr[13];

        if (pbuf_remove_header(p, 14) != 0) {
            TINYPAN_LOG_ERROR("netif: Fast-path failed to remove Ethernet header");
            return ERR_ARG;
        }

        uint8_t header_len = bnep_get_ethernet_header_len(dst_addr, src_addr);

        if (pbuf_add_header(p, header_len) != 0) {
            TINYPAN_LOG_ERROR("netif: Fast-path failed to add BNEP headroom");
            pbuf_add_header(p, 14); /* Revert */
            return ERR_IF;
        }

        bnep_write_ethernet_header((uint8_t*)p->payload, header_len, dst_addr, src_addr, ethertype);

        int result = hal_bt_l2cap_send(p->payload, p->tot_len);
        if (result == 0) {
            /* Sent successfully */
        } else if (result < 0) {
            TINYPAN_LOG_ERROR("netif: Fast-path failed to send BNEP frame: %d", result);
        } else {
            /* RACE CONDITION: L2CAP became busy just after our can_send check. 
               We must queue this packet so it isn't dropped. Because `p` currently 
               contains the exact BNEP-encapsulated frame we want, cloning it via 
               PBUF_RAW captures the exact frame to enqueue perfectly. */
            TINYPAN_LOG_DEBUG("netif: Fast-path race condition (busy), cloning to queue");
            hal_bt_l2cap_request_can_send_now();
            
            struct pbuf* fallback_q = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
            if (fallback_q != NULL) {
                uint8_t next_tail = (s_tx_queue_tail + 1) % TINYPAN_TX_QUEUE_LEN;
                if (next_tail == s_tx_queue_head) {
                    TINYPAN_LOG_WARN("netif: TX queue full, dropping fast-path fallback");
                    pbuf_free(fallback_q);
                } else {
                    s_tx_queue[s_tx_queue_tail] = fallback_q;
                    s_tx_queue_tail = next_tail;
                }
            } else {
                TINYPAN_LOG_ERROR("netif: Failed to clone fast-path fallback");
            }
        }

        /* REVERT the pbuf so lwIP handles it correctly upon return */
        pbuf_remove_header(p, header_len);
        pbuf_add_header(p, 14);
        
        return ERR_OK;
    }
    
    /* SLOW PATH (Radio Busy or Chained Pbuf): We must clone and queue.
       Because etharp_output unconditionally strips the MAC header (pbuf_remove_header(p, 14))
       immediately after we return, queueing the original struct pbuf* p by reference is
       fatal. Chained pbufs (p->next != NULL) also end up here, since the HAL expects
       a single contiguous buffer. We must physically detach from lwIP's volatile sequence
       path by cloning every packet into a contiguous PBUF_RAM block before modifying it.
       
       CRITICAL: We cannot use pbuf_clone(PBUF_RAW, ...). It allocates exactly `p->tot_len` 
       and destroys the PBUF_LINK_ENCAPSULATION_HLEN headroom. We must manually allocate a
       PBUF_LINK to restore the headroom, then copy the payload. */
    struct pbuf* q = pbuf_alloc(PBUF_LINK, p->tot_len, PBUF_RAM);
    if (q == NULL) {
        TINYPAN_LOG_WARN("netif: Failed to allocate pbuf for asynchronous TX");
        return ERR_MEM;
    }
    
    if (pbuf_copy(q, p) != ERR_OK) {
        TINYPAN_LOG_WARN("netif: Failed to copy pbuf payload");
        pbuf_free(q);
        return ERR_MEM;
    }

    /* Extract MAC addresses and Ethertype BEFORE removing the header */
    uint8_t* eth_hdr = (uint8_t*)q->payload;
    uint8_t dst_addr[6], src_addr[6];
    memcpy(dst_addr, &eth_hdr[0], 6);
    memcpy(src_addr, &eth_hdr[6], 6);
    uint16_t ethertype = ((uint16_t)eth_hdr[12] << 8) | eth_hdr[13];

    /* Safely strip the 14-byte Ethernet header using the lwIP API */
    if (pbuf_remove_header(q, 14) != 0) {
        TINYPAN_LOG_ERROR("netif: Failed to remove Ethernet header");
        pbuf_free(q);
        return ERR_ARG;
    }

    /* Query how many bytes the BNEP header needs */
    uint8_t header_len = bnep_get_ethernet_header_len(dst_addr, src_addr);

    /* Safely claim the exact BNEP headroom using the lwIP API (unhides the PBUF_LINK_ENCAPSULATION_HLEN bytes) */
    if (pbuf_add_header(q, header_len) != 0) {
        TINYPAN_LOG_ERROR("netif: Failed to add BNEP headroom");
        pbuf_free(q);
        return ERR_IF;
    }

    /* Write the native BNEP header into the newly exposed headroom */
    bnep_write_ethernet_header((uint8_t*)q->payload, header_len, dst_addr, src_addr, ethertype);
#endif

    /* `q` is now a fully encapsulated BNEP frame. Attempt to send it. */
    if (s_tx_queue_head == s_tx_queue_tail) {
        if (!hal_bt_l2cap_can_send()) {
            hal_bt_l2cap_request_can_send_now();
            /* Busy, fall through to queueing */
        } else {
            int result = hal_bt_l2cap_send(q->payload, q->tot_len);
            if (result == 0) {
                pbuf_free(q);
                return ERR_OK; /* Sent successfully */
            } else if (result < 0) {
                TINYPAN_LOG_ERROR("netif: Failed to send BNEP frame: %d", result);
                pbuf_free(q);
                return ERR_IF;
            } else {
                hal_bt_l2cap_request_can_send_now();
                /* Busy (race condition), fall through to queueing */
            }
        }
    }
    
    /* Queue the heavily modified/cloned pbuf (q) */
    uint8_t next_tail = (s_tx_queue_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    if (next_tail == s_tx_queue_head) {
        TINYPAN_LOG_WARN("netif: TX queue full, dropping queued BNEP frame");
        pbuf_free(q);
        return ERR_MEM;
    }
    
    TINYPAN_LOG_DEBUG("netif: L2CAP busy, queueing BNEP frame (len=%u)", q->tot_len);
    s_tx_queue[s_tx_queue_tail] = q;
    s_tx_queue_tail = next_tail;
    
    /* Always return ERR_OK to lwIP so it tears down the original `p` immediately */
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
    if (!s_initialized) {
        return;
    }
    
#if TINYPAN_USE_BLE_SLIP
    if (payload_len == 0 || payload == NULL) return;
    
    /* Enqueue incoming bytes for SLIP to read */
    for (uint16_t i = 0; i < payload_len; i++) {
        uint16_t next_head = (s_rx_queue_head + 1) % TINYPAN_RX_BUFFER_SIZE;
        if (next_head == s_rx_queue_tail) {
            TINYPAN_LOG_WARN("netif: RX queue overflow in SLIP mode");
            break;
        }
        s_rx_queue[s_rx_queue_head] = payload[i];
        s_rx_queue_head = next_head;
    }
    
    /* Trigger SLIP processing which will call sio_read to drain our queue */
    slipif_process_rxqueue(&s_netif);
#else
    if (dst_addr == NULL || src_addr == NULL) {
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
#endif
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

void tinypan_netif_flush_queue(void) {
    while (s_tx_queue_head != s_tx_queue_tail) {
        if (s_tx_queue[s_tx_queue_head] != NULL) {
            pbuf_free(s_tx_queue[s_tx_queue_head]);
            s_tx_queue[s_tx_queue_head] = NULL;
        }
        s_tx_queue_head = (s_tx_queue_head + 1) % TINYPAN_TX_QUEUE_LEN;
    }
}

void tinypan_netif_drain_tx_queue(void) {
    if (!s_initialized) {
        return;
    }
    
    /* Always give priority to BNEP control packets */
    if (!bnep_drain_control_tx_queue()) {
        return; /* Still busy, control packet couldn't be sent */
    }

    if (s_tx_queue_head == s_tx_queue_tail) {
        return;
    }
    
    while (s_tx_queue_head != s_tx_queue_tail) {
        if (!hal_bt_l2cap_can_send()) {
            hal_bt_l2cap_request_can_send_now();
            break; /* Still busy, will try again on next callback */
        }
        
        struct pbuf* q = s_tx_queue[s_tx_queue_head];
        
        /* The queue contains purely encapsulated BNEP frames. Send directly. */
        int result = hal_bt_l2cap_send(q->payload, q->tot_len);
        
        if (result == 0) {
            /* Sent successfully */
            s_tx_queue_head = (s_tx_queue_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        } else if (result > 0) {
            /* Race condition: became busy while sending */
            hal_bt_l2cap_request_can_send_now();
            break;
        } else {
            /* Hard error (e.g. disconnected) */
            TINYPAN_LOG_ERROR("netif: Queue flush failed: %d", result);
            s_tx_queue_head = (s_tx_queue_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        }
    }
}

/* ============================================================================
 * lwIP System Time Provider
 * ============================================================================ */

u32_t sys_now(void) {
    return hal_get_tick_ms();
}
