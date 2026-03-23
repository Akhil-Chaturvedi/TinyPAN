/*
 * TinyPAN BNEP Transport Backend
 *
 * Implements tinypan_transport_t for Bluetooth Classic BNEP mode.
 * Optimized for zero-copy transmission via scatter-gather iovec mapping,
 * separating the synthesized BNEP header from the original pbuf payload.
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

/* TX Queue: stores unmodified pbufs alongside their synthesized BNEP headers */
typedef struct {
    struct pbuf* p;
    uint8_t hdr[15];
    uint8_t hdr_len;
    tinypan_iovec_t iov[6]; /* Reduced from 16 to 6 to save RAM; 16-chain pbufs are invalid. */
    uint16_t iov_count;
    uint32_t sent_at_ms;
    bool in_flight;
} bnep_tx_job_t;

static bnep_tx_job_t s_bnep_tx_queue[TINYPAN_TX_QUEUE_LEN];
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
        
        bnep_tx_job_t* job = &s_bnep_tx_queue[s_bnep_tx_head];
        
        if (job->in_flight) {
            /* Check for hardware/link timeout. If a packet is in flight but the 
             * TX_COMPLETE interrupt was lost (e.g. stack glitch or link drop), 
             * we must forcibly time out to reclaim memory. */
            uint32_t now = hal_get_tick_ms();
            if (now - job->sent_at_ms > TINYPAN_BNEP_TX_TIMEOUT_MS) {
                TINYPAN_LOG_ERROR("transport_bnep: TX timeout (in_flight=%d, job=%p, p=%p)", 
                                   job->in_flight, (void*)job, (void*)job->p);
                struct pbuf* q = job->p;
                job->p = NULL;
                job->in_flight = false;
                s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
                if (q) pbuf_free(q);
                continue; /* Try sending the next packet in the queue */
            }
            break; /* Packet still legitimately in flight */
        }
        
        struct pbuf* q = job->p;
        int result = 0;
        
        /* Convert to iovec array: 
         * iov[0] = Synthesized BNEP header
         * iov[1..N] = Original pbuf, skipping the 14-byte Ethernet header */
        job->iov[0].iov_base = job->hdr;
        job->iov[0].iov_len = job->hdr_len;
        job->iov_count = 1;

        /* Determine offset to skip the Ethernet header (14 bytes + pad) */
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
        uint16_t skip_bytes = 14 + ETH_PAD_SIZE;
#else
        uint16_t skip_bytes = 14;
#endif

        struct pbuf* iter = q;
        uint16_t max_iov = sizeof(job->iov) / sizeof(job->iov[0]);
        while (iter != NULL && job->iov_count < max_iov) {
            if (iter->len > 0) {
                if (skip_bytes >= iter->len) {
                    /* Entire pbuf is skipped */
                    skip_bytes -= iter->len;
                } else {
                    job->iov[job->iov_count].iov_base = (const uint8_t*)iter->payload + skip_bytes;
                    job->iov[job->iov_count].iov_len = iter->len - skip_bytes;
                    job->iov_count++;
                    skip_bytes = 0; /* Only skip in the first block(s) */
                }
            }
            iter = iter->next;
        }
        
        if (job->iov_count <= 1) {
            /* Empty frame (Ethernet header entirely skipped, no payload).
             * Drop cleanly as there is nothing to send besides the BNEP header. */
            TINYPAN_LOG_WARN("transport_bnep: Dropping empty payload frame");
            result = -1;
        } else if (iter != NULL) {
            TINYPAN_LOG_ERROR("transport_bnep: PBUF chain too long for iovec");
            result = -1;
        } else {
            result = hal_bt_l2cap_send_iovec(job->iov, job->iov_count);
        }
        
        if (result == 0) {
            /* Success - HW DMA transfer initiated. Wait for complete event. */
            job->in_flight = true;
            job->sent_at_ms = hal_get_tick_ms();
            break; /* Standard L2CAP serialization requires one packet at a time */
        } else if (result > 0) {
            hal_bt_l2cap_request_can_send_now();
            break;
        } else {
            /* Drop packet on hard failure */
            TINYPAN_LOG_ERROR("transport_bnep: Queue flush failed: %d", result);
            job->p = NULL;
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        }
    }
}

void bnep_transport_flush_tx_queue(void) {
    while (s_bnep_tx_head != s_bnep_tx_tail) {
        bnep_tx_job_t* job = &s_bnep_tx_queue[s_bnep_tx_head];
        
        /* Hardening: We MUST NOT free an in-flight packet during a standard flush
         * because the hardware DMA controller might still be physically reading it
         * over the memory bus. If the link is truly dead, the BNEP TX timeout logic
         * will independently reclaim this memory after waiting for DMA shutdown. */
        if (job->in_flight) {
            break;
        }

        if (job->p != NULL) {
            pbuf_free(job->p);
            job->p = NULL;
        }
        job->in_flight = false;
        s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
    }
}

static void bnep_transport_on_tx_complete(void) {
    if (s_bnep_tx_head != s_bnep_tx_tail) {
        bnep_tx_job_t* job = &s_bnep_tx_queue[s_bnep_tx_head];
        if (job->in_flight) {
            struct pbuf* q = job->p;
            job->p = NULL;
            job->in_flight = false;
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            if (q) pbuf_free(q);
            
            /* Process next packet */
            bnep_transport_drain_tx_queue();
        }
    }
}

static int bnep_transport_output(struct netif* netif, struct pbuf* p) {
    (void)netif;
    if (p == NULL) return ERR_ARG;
    
    if (!bnep_is_connected()) {
        return ERR_CONN;
    }
    
    /* True Zero-Copy BNEP TX Path via `iovec`.
     * Instead of mutating the shared pbuf using pbuf_header(), we synthesize
     * the BNEP header locally in the queue struct, and pass it directly into
     * iov[0] while the remaining pbuf segments map to iov[1..N]. */

    /* Read Ethernet header via safe byte indexing to avoid unaligned access faults
     * on architectures that do not support non-word-aligned pointers (e.g. ARM Cortex-M0).
     * We must also respect ETH_PAD_SIZE to correctly locate the start of the header. */
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
    uint8_t* eth_ptr = (uint8_t*)p->payload + ETH_PAD_SIZE;
#else
    uint8_t* eth_ptr = (uint8_t*)p->payload;
#endif
    uint8_t* dst_addr = &eth_ptr[0];
    uint8_t* src_addr = &eth_ptr[6];
    uint16_t ethertype = (eth_ptr[12] << 8) | eth_ptr[13];

    uint8_t bnep_hdr_len = bnep_get_ethernet_header_len(dst_addr, src_addr);

    /* Enqueue the job */
    uint8_t next_tail = (s_bnep_tx_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    if (next_tail == s_bnep_tx_head) {
        /* Queue full, report backpressure */
        return ERR_MEM;
    }
    
    bnep_tx_job_t* job = &s_bnep_tx_queue[s_bnep_tx_tail];
    pbuf_ref(p);
    job->p = p;
    job->hdr_len = bnep_hdr_len;
    job->in_flight = false;
    job->sent_at_ms = 0;
    bnep_write_ethernet_header(job->hdr, bnep_hdr_len, dst_addr, src_addr, ethertype);
    
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
    .on_tx_complete = bnep_transport_on_tx_complete,
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
