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

/* TX Queue: stores modified pbufs with BNEP headers written in-place */
static struct pbuf* s_bnep_tx_queue[TINYPAN_TX_QUEUE_LEN] = {0};
static uint8_t s_bnep_tx_head = 0;
static uint8_t s_bnep_tx_tail = 0;

/* Tracks how many bytes were prepended via pbuf_header for each queued frame,
 * so we can restore the pointer after the send completes. */
static int16_t s_bnep_tx_hdr_delta[TINYPAN_TX_QUEUE_LEN] = {0};

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
        int result = 0;
        
        /* Convert pbuf chain to iovec array */
        tinypan_iovec_t iov[16];
        uint16_t iov_count = 0;
        struct pbuf* iter = q;
        while (iter != NULL && iov_count < 16) {
            if (iter->len > 0) {
                iov[iov_count].iov_base = (const uint8_t*)iter->payload;
                iov[iov_count].iov_len = iter->len;
                iov_count++;
            }
            iter = iter->next;
        }
        
        if (iter != NULL) {
            TINYPAN_LOG_ERROR("transport_bnep: PBUF chain too long for iovec");
            result = -1;
        } else {
            result = hal_bt_l2cap_send_iovec(iov, iov_count);
        }
        
        if (result == 0) {
            /* Restore the pbuf payload pointer before releasing */
            int16_t delta = s_bnep_tx_hdr_delta[s_bnep_tx_head];
            if (delta > 0) {
                pbuf_header(q, -delta);
            }
            s_bnep_tx_queue[s_bnep_tx_head] = NULL;
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        } else if (result > 0) {
            hal_bt_l2cap_request_can_send_now();
            break;
        } else {
            TINYPAN_LOG_ERROR("transport_bnep: Queue flush failed: %d", result);
            int16_t delta = s_bnep_tx_hdr_delta[s_bnep_tx_head];
            if (delta > 0) {
                pbuf_header(q, -delta);
            }
            s_bnep_tx_queue[s_bnep_tx_head] = NULL;
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        }
    }
}

void bnep_transport_flush_tx_queue(void) {
    while (s_bnep_tx_head != s_bnep_tx_tail) {
        if (s_bnep_tx_queue[s_bnep_tx_head] != NULL) {
            int16_t delta = s_bnep_tx_hdr_delta[s_bnep_tx_head];
            if (delta > 0) {
                pbuf_header(s_bnep_tx_queue[s_bnep_tx_head], -delta);
            }
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
    
    /* True Zero-Copy BNEP TX Path.
     * lwIP pre-allocates PBUF_LINK_ENCAPSULATION_HLEN (15) bytes of headroom
     * in every outgoing pbuf. We use pbuf_header() to expand backwards into
     * that reserved space, write the BNEP header in-place, send, and then
     * restore the pointer so lwIP can safely retry/free the pbuf. */

    uint8_t dst_addr[6], src_addr[6];
    uint16_t ethertype;
    
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
    uint16_t eth_offset = ETH_PAD_SIZE;
#else
    uint16_t eth_offset = 0;
#endif

    /* Extract Ethernet header fields from the payload */
    pbuf_copy_partial(p, dst_addr, 6, eth_offset + 0);
    pbuf_copy_partial(p, src_addr, 6, eth_offset + 6);
    uint8_t eth_type_buf[2];
    pbuf_copy_partial(p, eth_type_buf, 2, eth_offset + 12);
    ethertype = ((uint16_t)eth_type_buf[0] << 8) | eth_type_buf[1];

    uint8_t bnep_hdr_len = bnep_get_ethernet_header_len(dst_addr, src_addr);

    /* Strip the 14-byte Ethernet header + pad, then expand into headroom
     * for the BNEP header. Net delta = bnep_hdr_len - (14 + eth_offset). */
    int16_t strip = (int16_t)(14 + eth_offset);
    if (pbuf_header(p, -strip) != 0) {
        TINYPAN_LOG_ERROR("transport_bnep: Failed to strip Ethernet header");
        return ERR_BUF;
    }
    
    /* Expand backwards into the pre-allocated PBUF_LINK_ENCAPSULATION_HLEN space */
    if (pbuf_header(p, (int16_t)bnep_hdr_len) != 0) {
        /* Restore original position on failure */
        pbuf_header(p, strip);
        TINYPAN_LOG_ERROR("transport_bnep: No headroom for BNEP header");
        return ERR_BUF;
    }

    /* Write the BNEP header directly into the pbuf's headroom */
    bnep_write_ethernet_header((uint8_t*)p->payload, bnep_hdr_len, dst_addr, src_addr, ethertype);
    
    /* Track total header delta so we can restore the pointer after send */
    int16_t hdr_delta = (int16_t)bnep_hdr_len - strip;
    
    /* Enqueue the modified pbuf */
    uint8_t next_tail = (s_bnep_tx_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    if (next_tail == s_bnep_tx_head) {
        /* Queue full, restore pbuf and report backpressure */
        pbuf_header(p, -hdr_delta);
        return ERR_MEM;
    }
    pbuf_ref(p);
    s_bnep_tx_queue[s_bnep_tx_tail] = p;
    s_bnep_tx_hdr_delta[s_bnep_tx_tail] = hdr_delta;
    s_bnep_tx_tail = next_tail;

    /* Signal the HAL. The application polling thread will drain the queue
     * via bnep_transport_drain_tx_queue() on the next CAN_SEND_NOW event. */
    hal_bt_l2cap_request_can_send_now();
    
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
