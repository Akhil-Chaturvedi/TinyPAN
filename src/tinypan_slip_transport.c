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
static struct pbuf* s_slip_rx_curr_pbuf = NULL;
static uint16_t s_slip_rx_curr_offset = 0;
static uint16_t s_slip_rx_total_offset = 0;
static bool s_slip_rx_escape = false;

/* TX State Machine */
static struct pbuf* s_slip_tx_queue[TINYPAN_TX_QUEUE_LEN] = {0};
static uint8_t s_slip_tx_head = 0;
static uint8_t s_slip_tx_tail = 0;
static struct pbuf* s_slip_tx_current = NULL; /* Tracks current segment in the chain */
static uint16_t s_slip_tx_offset = 0;
static uint8_t s_slip_tx_state = 0; /* 0 = START, 1 = PAYLOAD, 2 = END */

static uint8_t s_slip_chunk_buf[128];
static uint16_t s_slip_chunk_len = 0;

#endif /* TINYPAN_ENABLE_LWIP */

static void slip_transport_handle_incoming(const uint8_t* data, uint16_t len) {
#if TINYPAN_ENABLE_LWIP
    if (len == 0 || data == NULL) return;

    const uint8_t* p = data;
    const uint8_t* end = data + len;

    while (p < end) {
        if (s_slip_rx_pbuf == NULL) {
            /* Memory Management: Use incremental pbuf allocation during SLIP 
             * streaming to reduce peak RAM pressure. */
            s_slip_rx_pbuf = pbuf_alloc(PBUF_RAW, 0, PBUF_POOL); 
            if (s_slip_rx_pbuf == NULL) return; /* OOM */
            s_slip_rx_curr_pbuf = s_slip_rx_pbuf;
            s_slip_rx_curr_offset = 0;
            s_slip_rx_total_offset = 0;
            s_slip_rx_escape = false;
        }

        /* Fast path: search for next delimiter in the current block */
        const uint8_t* next_delim = NULL;
        if (!s_slip_rx_escape) {
            const uint8_t* next_end = memchr(p, SLIP_END, end - p);
            const uint8_t* next_esc = memchr(p, SLIP_ESC, end - p);
            
            if (next_end && next_esc) next_delim = (next_end < next_esc) ? next_end : next_esc;
            else next_delim = next_end ? next_end : next_esc;
        }

        uint16_t chunk_len;
        if (next_delim) {
            chunk_len = (uint16_t)(next_delim - p);
        } else {
            chunk_len = (uint16_t)(end - p);
        }

        /* Copy non-escaped chunk directly into pbuf chain */
        if (chunk_len > 0) {
            uint16_t written = 0;
            while (written < chunk_len && s_slip_rx_total_offset < TINYPAN_MAX_FRAME_SIZE) {
                uint16_t space = s_slip_rx_curr_pbuf->len - s_slip_rx_curr_offset;
                if (space == 0) {
                    /* Current segment is full, allocate next one if not and if we are within limits */
                    if (s_slip_rx_total_offset >= TINYPAN_MAX_FRAME_SIZE) break;
                    struct pbuf* next = pbuf_alloc(PBUF_RAW, 0, PBUF_POOL);
                    if (next == NULL) { /* OOM */
                        pbuf_free(s_slip_rx_pbuf);
                        s_slip_rx_pbuf = NULL;
                        return;
                    }
                    pbuf_cat(s_slip_rx_pbuf, next);
                    s_slip_rx_curr_pbuf = next;
                    s_slip_rx_curr_offset = 0;
                    space = s_slip_rx_curr_pbuf->len;
                }
                
                uint16_t to_write = (chunk_len - written < space) ? (chunk_len - written) : space;
                memcpy((uint8_t*)s_slip_rx_curr_pbuf->payload + s_slip_rx_curr_offset, p + written, to_write);
                s_slip_rx_curr_offset += to_write;
                written += to_write;
                s_slip_rx_total_offset += to_write;
            }
            p += written;
            
            if (written < chunk_len && s_slip_rx_total_offset >= TINYPAN_MAX_FRAME_SIZE) {
                /* Frame too large, drop it */
                pbuf_free(s_slip_rx_pbuf);
                s_slip_rx_pbuf = NULL;
                s_slip_rx_escape = false; /* Reset state on overflow */
                return;
            }
        }

        /* Process the delimiter or escape byte */
        if (p < end) {
            uint8_t c = *p++;
            if (s_slip_rx_escape) {
                s_slip_rx_escape = false;
                if (c == SLIP_ESC_END) c = SLIP_END;
                else if (c == SLIP_ESC_ESC) c = SLIP_ESC;
                else continue; /* Protocol violation */
                
                /* Write the escaped byte */
                if (s_slip_rx_total_offset < TINYPAN_MAX_FRAME_SIZE) {
                    while (s_slip_rx_curr_pbuf != NULL && s_slip_rx_curr_offset >= s_slip_rx_curr_pbuf->len) {
                        s_slip_rx_curr_pbuf = s_slip_rx_curr_pbuf->next;
                        s_slip_rx_curr_offset = 0;
                    }
                    if (s_slip_rx_curr_pbuf) {
                        ((uint8_t*)s_slip_rx_curr_pbuf->payload)[s_slip_rx_curr_offset++] = c;
                        s_slip_rx_total_offset++;
                    }
                }
            } else if (c == SLIP_ESC) {
                s_slip_rx_escape = true;
            } else if (c == SLIP_END) {
                if (s_slip_rx_total_offset > 0) {
                    pbuf_realloc(s_slip_rx_pbuf, s_slip_rx_total_offset);
                    struct netif* netif = tinypan_netif_get();
                    if (netif) {
                        if (netif->input(s_slip_rx_pbuf, netif) != ERR_OK) pbuf_free(s_slip_rx_pbuf);
                    } else {
                        pbuf_free(s_slip_rx_pbuf);
                    }
                    s_slip_rx_pbuf = NULL;
                }
            }
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

        /* If we have a pending chunk from a previous busy state, try sending it first */
        if (s_slip_chunk_len > 0) {
            int result = hal_bt_l2cap_send(s_slip_chunk_buf, s_slip_chunk_len);
            if (result > 0) {
                hal_bt_l2cap_request_can_send_now();
                break;
            } else if (result < 0) {
                /* Hard error, drop packet */
                s_slip_chunk_len = 0;
                goto drop_packet;
            }
            s_slip_chunk_len = 0; /* Successfully sent */
            continue; /* Immediately check if we can send more */
        }

        /* Fill the next chunk */
        struct pbuf* root_pbuf = s_slip_tx_queue[s_slip_tx_head];
        if (s_slip_tx_current == NULL) {
            s_slip_tx_current = root_pbuf;
            s_slip_tx_offset = 0;
            s_slip_tx_state = 0;
        }

        uint16_t chunk_idx = 0;

        if (s_slip_tx_state == 0) {
            s_slip_chunk_buf[chunk_idx++] = SLIP_END;
            s_slip_tx_state = 1;
        }

        if (s_slip_tx_state == 1) {
            while (s_slip_tx_current != NULL && chunk_idx < sizeof(s_slip_chunk_buf) - 2) {
                uint8_t* payload = (uint8_t*)s_slip_tx_current->payload;
                while (s_slip_tx_offset < s_slip_tx_current->len && chunk_idx < sizeof(s_slip_chunk_buf) - 2) {
                    uint8_t c = payload[s_slip_tx_offset++];
                    if (c == SLIP_END) {
                        s_slip_chunk_buf[chunk_idx++] = SLIP_ESC;
                        s_slip_chunk_buf[chunk_idx++] = SLIP_ESC_END;
                    } else if (c == SLIP_ESC) {
                        s_slip_chunk_buf[chunk_idx++] = SLIP_ESC;
                        s_slip_chunk_buf[chunk_idx++] = SLIP_ESC_ESC;
                    } else {
                        s_slip_chunk_buf[chunk_idx++] = c;
                    }
                }
                if (s_slip_tx_offset >= s_slip_tx_current->len) {
                    s_slip_tx_current = s_slip_tx_current->next;
                    s_slip_tx_offset = 0;
                }
            }
            if (s_slip_tx_current == NULL) {
                s_slip_tx_state = 2;
            }
        }

        bool frame_done = false;
        if (s_slip_tx_state == 2 && chunk_idx < sizeof(s_slip_chunk_buf)) {
            s_slip_chunk_buf[chunk_idx++] = SLIP_END;
            frame_done = true;
        }

        if (chunk_idx > 0) {
            s_slip_chunk_len = chunk_idx;
            int result = hal_bt_l2cap_send(s_slip_chunk_buf, s_slip_chunk_len);
            if (result > 0) {
                hal_bt_l2cap_request_can_send_now();
                break;
            } else if (result < 0) {
                s_slip_chunk_len = 0;
                goto drop_packet;
            }
            s_slip_chunk_len = 0; /* Sent successfully */
        }

        if (frame_done) {
drop_packet:
            pbuf_free(root_pbuf);
            s_slip_tx_queue[s_slip_tx_head] = NULL;
            s_slip_tx_head = (s_slip_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            s_slip_tx_current = NULL;
            s_slip_tx_offset = 0;
            s_slip_tx_state = 0;
            s_slip_chunk_len = 0;
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
    s_slip_tx_current = NULL;
    s_slip_tx_offset = 0;
    s_slip_tx_state = 0;
    s_slip_chunk_len = 0;
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
