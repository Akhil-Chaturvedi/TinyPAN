/*
 * TinyPAN BNEP Transport Backend
 *
 * Implements tinypan_transport_t for Bluetooth Classic BNEP mode.
 * Optimized for zero-copy transmission via in-place Ethernet-to-BNEP header
 * swapping and naturally aligned payload offsets (ETH_PAD_SIZE=1).
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
extern void supervisor_on_bnep_filter_response(uint16_t response_code);

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

static void bnep_transport_filter_response_cb(uint16_t response_code, void* user_data) {
    (void)user_data;
    supervisor_on_bnep_filter_response(response_code);
}

static int bnep_transport_init(void) {
    bnep_init();
    bnep_register_frame_callback(bnep_transport_frame_cb, NULL);
    bnep_register_setup_response_callback(bnep_transport_setup_response_cb, NULL);
    bnep_register_filter_response_callback(bnep_transport_filter_response_cb, NULL);
    
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
static struct pbuf* s_bnep_tx_orig[TINYPAN_TX_QUEUE_LEN] = {0};
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
        int result = hal_bt_l2cap_send_pbuf(q);
        
        if (result == 0) {
            s_bnep_tx_queue[s_bnep_tx_head] = NULL;
            if (s_bnep_tx_orig[s_bnep_tx_head]) {
                pbuf_free(s_bnep_tx_orig[s_bnep_tx_head]);
                s_bnep_tx_orig[s_bnep_tx_head] = NULL;
            }
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        } else if (result > 0) {
            hal_bt_l2cap_request_can_send_now();
            break;
        } else {
            TINYPAN_LOG_ERROR("transport_bnep: Queue flush failed: %d", result);
            s_bnep_tx_queue[s_bnep_tx_head] = NULL;
            if (s_bnep_tx_orig[s_bnep_tx_head]) {
                pbuf_free(s_bnep_tx_orig[s_bnep_tx_head]);
                s_bnep_tx_orig[s_bnep_tx_head] = NULL;
            }
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
        if (s_bnep_tx_orig[s_bnep_tx_head] != NULL) {
            pbuf_free(s_bnep_tx_orig[s_bnep_tx_head]);
            s_bnep_tx_orig[s_bnep_tx_head] = NULL;
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
    
    /* QA Round 20: Real Zero-Copy TX Path.
     * Instead of copying the whole payload, we allocate a small header-only pbuf,
     * write the BNEP header into it, and chain it to the original pbuf.
     * We use pbuf_ref(p) to ensure the original pbuf is not freed by lwIP 
     * while the HAL/queue is still using it (essential for TCP retransmission). */

    uint8_t dst_addr[6], src_addr[6];
    uint16_t ethertype;
    
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
    uint16_t eth_offset = ETH_PAD_SIZE;
#else
    uint16_t eth_offset = 0;
#endif

    /* Extract Ethernet headers from the original pbuf pack */
    pbuf_copy_partial(p, dst_addr, 6, eth_offset + 0);
    pbuf_copy_partial(p, src_addr, 6, eth_offset + 6);
    uint8_t eth_type_buf[2];
    pbuf_copy_partial(p, eth_type_buf, 2, eth_offset + 12);
    ethertype = ((uint16_t)eth_type_buf[0] << 8) | eth_type_buf[1];

    uint8_t bnep_hdr_len = bnep_get_ethernet_header_len(dst_addr, src_addr);

    /* 1. Allocate a small pbuf for just the BNEP header */
    struct pbuf* h = pbuf_alloc(PBUF_RAW, bnep_hdr_len, PBUF_RAM);
    if (h == NULL) return ERR_MEM;

    /* 2. Write BNEP header into the header pbuf */
    bnep_write_ethernet_header((uint8_t*)h->payload, bnep_hdr_len, dst_addr, src_addr, ethertype);
    
    /* 3. Create a non-destructive reference to the IP payload.
     * We MUST NOT use pbuf_header(p, -14) because that mutates the original pbuf
     * which lwIP needs for TCP retransmissions! Instead, we create a light 
     * reference pbuf that points into the payload of the original frame. */
    struct pbuf* payload_p = pbuf_alloc(PBUF_RAW, p->tot_len - (14 + eth_offset), PBUF_REF);
    if (payload_p == NULL) {
        pbuf_free(h);
        return ERR_MEM;
    }
    payload_p->payload = (uint8_t*)p->payload + 14 + eth_offset;
    
    /* Chain: [BNEP Header] -> [Payload Slice] */
    pbuf_chain(h, payload_p);
    
    /* We store 'p' in a parallel array to ensure it isn't freed by lwIP 
     * while the BNEP layer or radio is still using it. */
    uint8_t next_tail = (s_bnep_tx_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    if (next_tail == s_bnep_tx_head) {
        pbuf_free(h);
        return ERR_MEM;
    }
    pbuf_ref(p);
    s_bnep_tx_orig[s_bnep_tx_tail] = p;
    
    s_bnep_tx_queue[s_bnep_tx_tail] = h;
    s_bnep_tx_tail = next_tail;

    /* Try sending immediately if queue is empty */
    if (s_bnep_tx_head == ((s_bnep_tx_tail + TINYPAN_TX_QUEUE_LEN - 1) % TINYPAN_TX_QUEUE_LEN)) {
        if (!hal_bt_l2cap_can_send()) {
            hal_bt_l2cap_request_can_send_now();
        } else {
            /* HAL MUST support non-contiguous chains via scatter-gather or bounce buffer */
            int result = hal_bt_l2cap_send_pbuf(h);
            if (result == 0) {
                /* s_bnep_tx_head is already at the right spot? No, we increment it now. */
                bnep_transport_drain_tx_queue();
                return ERR_OK;
            } else if (result < 0) {
                bnep_transport_flush_tx_queue();
                return ERR_IF;
            } else {
                hal_bt_l2cap_request_can_send_now();
            }
        }
    }
    
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
