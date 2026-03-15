/*
 * TinyPAN BNEP Transport Backend
 *
 * Implements tinypan_transport_t for Bluetooth Classic BNEP mode.
 * Only compiled when TINYPAN_USE_BLE_SLIP is 0.
 */

#include "tinypan_transport.h"
#include "tinypan_bnep.h"
#include "tinypan_internal.h"
#include "../include/tinypan.h"
#include "../include/tinypan_config.h"
#include "../include/tinypan_hal.h"

#if !TINYPAN_USE_BLE_SLIP

#if TINYPAN_ENABLE_LWIP
#include "tinypan_lwip_netif.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "netif/ethernet.h"
#include "lwip/etharp.h"
#endif

#include <string.h>

/* Forward declare supervisor callbacks we need to emit */
extern void supervisor_on_bnep_setup_response(uint16_t response_code);

static void bnep_transport_frame_cb(const bnep_ethernet_frame_t* frame, void* user_data) {
    (void)user_data;
    TINYPAN_LOG_DEBUG("transport_bnep: Received frame type=0x%04X len=%u",
                       frame->ethertype, frame->payload_len);
    
#if TINYPAN_ENABLE_LWIP
    if (frame != NULL) {
        tinypan_netif_input(frame->dst_addr, frame->src_addr, frame->ethertype,
                            frame->payload, frame->payload_len);
    }
#endif
}

static void bnep_transport_setup_response_cb(uint16_t response_code, void* user_data) {
    (void)user_data;
    supervisor_on_bnep_setup_response(response_code);
}

static int bnep_transport_init(void) {
    bnep_init();
    bnep_register_frame_callback(bnep_transport_frame_cb, NULL);
    bnep_register_setup_response_callback(bnep_transport_setup_response_cb, NULL);
    
    const tinypan_config_t* config = tinypan_internal_get_config();
    if (config) {
        uint8_t local_addr[6];
        hal_get_local_bd_addr(local_addr);
        bnep_set_local_addr(local_addr);
        bnep_set_remote_addr(config->remote_addr);
    }
    
    return 0;
}

static void bnep_transport_on_connected(void) {
    bnep_on_l2cap_connected();
}

static void bnep_transport_on_disconnected(void) {
    bnep_on_l2cap_disconnected();
}

static void bnep_transport_handle_incoming(const uint8_t* data, uint16_t len) {
    bnep_handle_incoming(data, len);
}

static void bnep_transport_retry_setup(void) {
    bnep_send_setup_request();
}

static void bnep_transport_on_can_send_now(void) {
    /* If we were stalled sending the setup request, try again now */
    if (bnep_get_state() == BNEP_STATE_WAIT_FOR_CONNECTION_RESPONSE) {
        bnep_send_setup_request();
    }
#if TINYPAN_ENABLE_LWIP
    void bnep_transport_drain_tx_queue(void);
    bnep_transport_drain_tx_queue();
#endif
}

#if TINYPAN_ENABLE_LWIP

/* TX Queue moved from netif */
static struct pbuf* s_bnep_tx_queue[TINYPAN_TX_QUEUE_LEN] = {0};
static uint8_t s_bnep_tx_head = 0;
static uint8_t s_bnep_tx_tail = 0;

/* Must be exposed to drain the BNEP tx queue */
void bnep_transport_drain_tx_queue(void) {
    if (!bnep_drain_control_tx_queue()) {
        return;
    }
    
    if (s_bnep_tx_head == s_bnep_tx_tail) return;
    
    while (s_bnep_tx_head != s_bnep_tx_tail) {
        if (!hal_bt_l2cap_can_send()) {
            hal_bt_l2cap_request_can_send_now();
            break;
        }
        
        struct pbuf* q = s_bnep_tx_queue[s_bnep_tx_head];
        int result = hal_bt_l2cap_send(q->payload, q->tot_len);
        
        if (result == 0) {
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        } else if (result > 0) {
            hal_bt_l2cap_request_can_send_now();
            break;
        } else {
            TINYPAN_LOG_ERROR("transport_bnep: Queue flush failed: %d", result);
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        }
    }
}

void bnep_transport_flush_tx_queue(void) {
    while (s_bnep_tx_head != s_bnep_tx_tail) {
        if (s_bnep_tx_queue[s_bnep_tx_head] != NULL) {
            pbuf_free(s_bnep_tx_queue[s_bnep_tx_head]);
            s_bnep_tx_queue[s_bnep_tx_head] = NULL;
        }
        s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
    }
}

static int bnep_transport_output(struct netif* netif, struct pbuf* p) {
    (void)netif;
    if (p == NULL) return ERR_ARG;
    
    if (!bnep_is_connected()) {
        return ERR_CONN;
    }
    if (p->tot_len < 14) {
        return ERR_ARG;
    }
    
    bool can_send_now = (s_bnep_tx_head == s_bnep_tx_tail) && 
                        hal_bt_l2cap_can_send() && 
                        (p->next == NULL);
                        
    if (can_send_now) {
        
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
        if (pbuf_remove_header(p, ETH_PAD_SIZE) != 0) return ERR_ARG;
#endif

        uint8_t* eth_hdr = (uint8_t*)p->payload;
        uint8_t dst_addr[6], src_addr[6];
        memcpy(dst_addr, &eth_hdr[0], 6);
        memcpy(src_addr, &eth_hdr[6], 6);
        uint16_t ethertype = ((uint16_t)eth_hdr[12] << 8) | eth_hdr[13];
        
        if (pbuf_remove_header(p, 14) != 0) return ERR_ARG;
        
        uint8_t header_len = bnep_get_ethernet_header_len(dst_addr, src_addr);
        if (pbuf_add_header(p, header_len) != 0) {
            pbuf_add_header(p, 14);
            return ERR_IF;
        }
        
        bnep_write_ethernet_header((uint8_t*)p->payload, header_len, dst_addr, src_addr, ethertype);
        
        int result = hal_bt_l2cap_send(p->payload, p->tot_len);
        if (result == 0) {
            /* Sent */
        } else if (result < 0) {
            TINYPAN_LOG_ERROR("transport_bnep: Fast-path BNEP send failed: %d", result);
        } else {
            hal_bt_l2cap_request_can_send_now();
            struct pbuf* fallback_q = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
            if (fallback_q != NULL) {
                uint8_t next_tail = (s_bnep_tx_tail + 1) % TINYPAN_TX_QUEUE_LEN;
                if (next_tail != s_bnep_tx_head) {
                    s_bnep_tx_queue[s_bnep_tx_tail] = fallback_q;
                    s_bnep_tx_tail = next_tail;
                } else {
                    pbuf_free(fallback_q);
                }
            }
        }
        pbuf_remove_header(p, header_len);
        pbuf_add_header(p, 14);
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
        pbuf_add_header(p, ETH_PAD_SIZE);
#endif
        return ERR_OK;
    }
    
    struct pbuf* q = pbuf_alloc(PBUF_LINK, p->tot_len, PBUF_RAM);
    if (q == NULL) return ERR_MEM;
    if (pbuf_copy(q, p) != ERR_OK) { pbuf_free(q); return ERR_MEM; }
    
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
    if (pbuf_remove_header(q, ETH_PAD_SIZE) != 0) { pbuf_free(q); return ERR_ARG; }
#endif
    
    uint8_t* eth_hdr = (uint8_t*)q->payload;
    uint8_t dst_addr[6], src_addr[6];
    memcpy(dst_addr, &eth_hdr[0], 6);
    memcpy(src_addr, &eth_hdr[6], 6);
    uint16_t ethertype = ((uint16_t)eth_hdr[12] << 8) | eth_hdr[13];
    
    if (pbuf_remove_header(q, 14) != 0) { pbuf_free(q); return ERR_ARG; }
    uint8_t header_len = bnep_get_ethernet_header_len(dst_addr, src_addr);
    if (pbuf_add_header(q, header_len) != 0) { pbuf_free(q); return ERR_IF; }
    
    bnep_write_ethernet_header((uint8_t*)q->payload, header_len, dst_addr, src_addr, ethertype);
    
    if (s_bnep_tx_head == s_bnep_tx_tail) {
        if (!hal_bt_l2cap_can_send()) {
            hal_bt_l2cap_request_can_send_now();
        } else {
            int result = hal_bt_l2cap_send(q->payload, q->tot_len);
            if (result == 0) { pbuf_free(q); return ERR_OK; }
            else if (result < 0) { pbuf_free(q); return ERR_IF; }
            else { hal_bt_l2cap_request_can_send_now(); }
        }
    }
    
    uint8_t next_tail = (s_bnep_tx_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    if (next_tail == s_bnep_tx_head) {
        pbuf_free(q);
        return ERR_MEM;
    }
    
    s_bnep_tx_queue[s_bnep_tx_tail] = q;
    s_bnep_tx_tail = next_tail;
    return ERR_OK;
}
#endif

const tinypan_transport_t transport_bnep = {
    .name = "BNEP",
    .requires_setup = true,
    .init = bnep_transport_init,
    .on_connected = bnep_transport_on_connected,
    .on_disconnected = bnep_transport_on_disconnected,
    .handle_incoming = bnep_transport_handle_incoming,
    .retry_setup = bnep_transport_retry_setup,
    .on_can_send_now = bnep_transport_on_can_send_now,
    .flush_queues = bnep_transport_flush_tx_queue,
#if TINYPAN_ENABLE_LWIP
    .output = bnep_transport_output
#endif
};

#else
/* Stub for when BNEP is disabled */
const tinypan_transport_t transport_bnep = {
    .name = "BNEP (Disabled)"
};
#endif
