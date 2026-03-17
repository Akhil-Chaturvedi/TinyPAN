/*
 * TinyPAN SLIP Transport Backend
 *
 * Implements tinypan_transport_t for BLE SLIP companion mode.
 * Features a streaming FSM for zero-buffer RX (writing directly to pbuf chains)
 * and persistent TX queue drainage to handle radio backpressure.
 */

#include "tinypan_transport.h"
#include "../include/tinypan.h"
#include "../include/tinypan_config.h"
#include "../include/tinypan_hal.h"

#if TINYPAN_USE_BLE_SLIP

#if TINYPAN_ENABLE_LWIP
#include "tinypan_lwip_netif.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#endif

#include <string.h>

static int slip_transport_init(void) {
    return 0; /* SLIP is stateless at this level */
}

static void slip_transport_on_connected(void) {
    /* No setup phase for SLIP */
}

static void slip_transport_on_disconnected(void) {
    /* Nothing to do */
}

static void slip_transport_retry_setup(void) {
    /* SLIP has no setup phase, this is a no-op */
}

static void slip_transport_on_can_send_now(void) {
#if TINYPAN_ENABLE_LWIP
    void slip_transport_drain_tx_queue(void);
    slip_transport_drain_tx_queue();
#endif
}

/* SLIP Escape characters */
#define SLIP_END            0xC0
#define SLIP_ESC            0xDB
#define SLIP_ESC_END        0xDC
#define SLIP_ESC_ESC        0xDD

#if TINYPAN_ENABLE_LWIP
/* RX State Machine */
static struct pbuf* s_slip_rx_pbuf = NULL;
static uint16_t s_slip_rx_offset = 0;
static bool s_slip_rx_escape = false;

/* TX State Machine */
static struct pbuf* s_slip_tx_queue[TINYPAN_TX_QUEUE_LEN] = {0};
static uint8_t s_slip_tx_head = 0;
static uint8_t s_slip_tx_tail = 0;
static uint16_t s_slip_tx_offset = 0;
static bool s_slip_tx_sending_end = true; /* Need to send leading END */

#endif /* TINYPAN_ENABLE_LWIP */

static void slip_transport_handle_incoming(const uint8_t* data, uint16_t len) {
#if TINYPAN_ENABLE_LWIP
    if (len == 0 || data == NULL) return;

    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        
        if (s_slip_rx_pbuf == NULL) {
            s_slip_rx_pbuf = pbuf_alloc(PBUF_LINK, TINYPAN_MAX_FRAME_SIZE, PBUF_POOL);
            if (s_slip_rx_pbuf == NULL) {
                /* OOM, drop bytes until we hit next frame */
                continue;
            }
            s_slip_rx_offset = 0;
            s_slip_rx_escape = false;
        }

        if (s_slip_rx_escape) {
            s_slip_rx_escape = false;
            if (c == SLIP_ESC_END) c = SLIP_END;
            else if (c == SLIP_ESC_ESC) c = SLIP_ESC;
            else continue; /* Protocol violation */
        } else {
            if (c == SLIP_ESC) {
                s_slip_rx_escape = true;
                continue;
            } else if (c == SLIP_END) {
                if (s_slip_rx_offset > 0) {
                    pbuf_realloc(s_slip_rx_pbuf, s_slip_rx_offset);
                    
                    struct netif* netif = tinypan_netif_get();
                    if (netif && netif->input) {
                        if (netif->input(s_slip_rx_pbuf, netif) != ERR_OK) {
                            pbuf_free(s_slip_rx_pbuf);
                        }
                    } else {
                        pbuf_free(s_slip_rx_pbuf);
                    }
                    s_slip_rx_pbuf = NULL;
                }
                continue;
            }
        }

        if (s_slip_rx_offset < TINYPAN_MAX_FRAME_SIZE) {
            /* Use pbuf_take_at to safely write into chained PBUF_POOL pbufs.
             * PBUF_POOL may return a linked list of smaller blocks; direct
             * payload[offset] would overrun the first block's bounds. */
            pbuf_take_at(s_slip_rx_pbuf, &c, 1, s_slip_rx_offset);
            s_slip_rx_offset++;
        } else {
            /* Overflow */
            pbuf_free(s_slip_rx_pbuf);
            s_slip_rx_pbuf = NULL;
        }
    }
#else
    (void)data;
    (void)len;
#endif
}

#if TINYPAN_ENABLE_LWIP

void slip_transport_drain_tx_queue(void) {
    if (s_slip_tx_head == s_slip_tx_tail) return;

    while (s_slip_tx_head != s_slip_tx_tail) {
        if (!hal_bt_l2cap_can_send()) {
            hal_bt_l2cap_request_can_send_now();
            break;
        }
        
        struct pbuf* q = s_slip_tx_queue[s_slip_tx_head];
        
        /* Encode exactly one chunk of escaped SLIP bytes to avoid stack explosion.
         * Save the serialization offset before the loop so we can rewind if the
         * radio returns BUSY, preventing silent data loss. */
        uint8_t chunk_buf[256];
        uint16_t chunk_idx = 0;
        uint16_t saved_offset = s_slip_tx_offset;
        bool saved_sending_end = s_slip_tx_sending_end;
        
        if (s_slip_tx_sending_end && s_slip_tx_offset == 0) {
            chunk_buf[chunk_idx++] = SLIP_END;
            s_slip_tx_sending_end = false;
        }
        
        /* Stream bytes from the pbuf out, escaping as we go */
        bool finishing = false;
        while (s_slip_tx_offset < q->tot_len && chunk_idx < sizeof(chunk_buf) - 2) {
            uint8_t c = pbuf_get_at(q, s_slip_tx_offset++);
            if (c == SLIP_END) {
                chunk_buf[chunk_idx++] = SLIP_ESC;
                chunk_buf[chunk_idx++] = SLIP_ESC_END;
            } else if (c == SLIP_ESC) {
                chunk_buf[chunk_idx++] = SLIP_ESC;
                chunk_buf[chunk_idx++] = SLIP_ESC_ESC;
            } else {
                chunk_buf[chunk_idx++] = c;
            }
        }
        
        if (s_slip_tx_offset >= q->tot_len) {
            chunk_buf[chunk_idx++] = SLIP_END;
            finishing = true;
        }
        
        if (chunk_idx > 0) {
            int result = hal_bt_l2cap_send(chunk_buf, chunk_idx);
            if (result > 0) {
                /* Radio busy: rewind the serialization state to the exact
                 * position before this chunk was built.  The chunk_buf is
                 * discarded but no bytes are lost because s_slip_tx_offset
                 * is restored to its pre-chunk value. */
                s_slip_tx_offset = saved_offset;
                s_slip_tx_sending_end = saved_sending_end;
                hal_bt_l2cap_request_can_send_now();
                break;
            } else if (result < 0) {
                /* Hard error, drop packet to clear queue */
                finishing = true;
            }
        }
        
        if (finishing) {
            s_slip_tx_head = (s_slip_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
            s_slip_tx_offset = 0;
            s_slip_tx_sending_end = true;
        }
    }
}

void slip_transport_flush_tx_queue(void) {
    while (s_slip_tx_head != s_slip_tx_tail) {
        if (s_slip_tx_queue[s_slip_tx_head] != NULL) {
            pbuf_free(s_slip_tx_queue[s_slip_tx_head]);
            s_slip_tx_queue[s_slip_tx_head] = NULL;
        }
        s_slip_tx_head = (s_slip_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
    }
    s_slip_tx_offset = 0;
    s_slip_tx_sending_end = true;
}

static int slip_transport_output(struct netif* netif, struct pbuf* p) {
    (void)netif;
    if (p == NULL) return ERR_ARG;
    
    struct pbuf* q = pbuf_clone(PBUF_LINK, PBUF_RAM, p);
    if (!q) return ERR_MEM;

    uint8_t next_tail = (s_slip_tx_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    if (next_tail == s_slip_tx_head) {
        pbuf_free(q);
        return ERR_MEM;
    }

    s_slip_tx_queue[s_slip_tx_tail] = q;
    s_slip_tx_tail = next_tail;
    
    /* Kick TX engine */
    slip_transport_drain_tx_queue();
    
    return ERR_OK;
}

#endif /* TINYPAN_ENABLE_LWIP */

const tinypan_transport_t transport_slip = {
    .name = "SLIP",
    .requires_setup = false,
    .init = slip_transport_init,
    .on_connected = slip_transport_on_connected,
    .on_disconnected = slip_transport_on_disconnected,
    .handle_incoming = slip_transport_handle_incoming,
    .retry_setup = slip_transport_retry_setup,
    .on_can_send_now = slip_transport_on_can_send_now,
    .flush_queues = slip_transport_flush_tx_queue,
#if TINYPAN_ENABLE_LWIP
    .output = slip_transport_output
#endif
};

#else
/* Stub for when SLIP is disabled */
const tinypan_transport_t transport_slip = {
    .name = "SLIP (Disabled)"
};
#endif /* TINYPAN_USE_BLE_SLIP */
