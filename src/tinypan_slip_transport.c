/*
 * TinyPAN SLIP Transport Backend
 *
 * Implements tinypan_transport_t for BLE SLIP companion mode.
 *
 * TX: Encodes outgoing pbuf chains in-place using a memchr-based bulk copy loop
 * into a 128-byte staging buffer, flushed via hal_bt_l2cap_send(). The original
 * pbuf is held by reference (pbuf_ref) and freed when transmission completes.
 *
 * RX: Accumulates incoming bytes from the HAL into PBUF_POOL segments via a
 * streaming FSM. A single frame is capped at 2 pool segments to prevent pool
 * exhaustion from malformed or incomplete SLIP streams.
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
#if TINYPAN_ENABLE_LWIP
    /* Free any partially-assembled inbound frame. If a connection drops mid-frame
     * (e.g., BLE signal loss after one or two bytes have been received), the
     * allocated pbuf pool segment would be leaked otherwise. With PBUF_POOL_SIZE=4
     * this could exhaust the pool after 4 such events. */
    if (s_slip_rx_pbuf != NULL) {
        pbuf_free(s_slip_rx_pbuf);
        s_slip_rx_pbuf = NULL;
    }
    s_slip_rx_curr_pbuf   = NULL;
    s_slip_rx_curr_offset  = 0;
    s_slip_rx_total_offset = 0;
    s_slip_rx_seg_count    = 0;
    s_slip_rx_escape       = false;
#endif
}

#if TINYPAN_ENABLE_LWIP
/* Hardened configuration guard: prevent integer underflow bombs in chunking loop */
_Static_assert(TINYPAN_SLIP_CHUNK_SIZE >= 4, "TINYPAN_SLIP_CHUNK_SIZE must be at least 4 bytes");
#endif

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
static uint8_t s_slip_rx_seg_count = 0;
static bool s_slip_rx_escape = false;
static bool s_slip_rx_seeking_end = false;

/* SLIP Interface variables */
static struct pbuf* s_slip_tx_queue[TINYPAN_TX_QUEUE_LEN] = {0};
static uint8_t s_slip_tx_head = 0;
static uint8_t s_slip_tx_tail = 0;

/* Fits a standard 247-byte BLE 4.2+ Data Length Extension MTU without
 * artificially fragmenting it and causing extra RTOS context switches */
static uint8_t s_slip_chunk_buf[TINYPAN_SLIP_CHUNK_SIZE];
static uint16_t s_slip_chunk_len = 0;
static struct pbuf* s_slip_tx_current = NULL; /* Tracks current segment in the chain */
static uint16_t s_slip_tx_offset = 0;
static uint8_t s_slip_tx_state = 0; /* 0 = START, 1 = PAYLOAD, 2 = END */

#endif /* TINYPAN_ENABLE_LWIP */

static void slip_transport_handle_incoming(const uint8_t* data, uint16_t len) {
#if TINYPAN_ENABLE_LWIP
    if (len == 0 || data == NULL) return;

    const uint8_t* p = data;
    const uint8_t* end = data + len;

    while (p < end) {
        /* If we are recovering from a dropped frame, scan forward until SLIP_END
         * without allocating any unused pbufs from the pool. */
        if (s_slip_rx_seeking_end) {
            while (p < end && *p != SLIP_END) p++;
            if (p < end && *p == SLIP_END) {
                s_slip_rx_seeking_end = false;
                p++; /* Consume the delimiter */
                continue; /* Process next frame properly */
            } else {
                break; /* End of buffer, still seeking */
            }
        }

        if (s_slip_rx_pbuf == NULL) {
            /* Memory Management: Allocate a full pool segment. lwIP sets len/tot_len
             * to 0 if we pass 0 to pbuf_alloc, which would cause an infinite loop. */
            s_slip_rx_pbuf = pbuf_alloc(PBUF_RAW, PBUF_POOL_BUFSIZE, PBUF_POOL); 
            if (s_slip_rx_pbuf == NULL) return; /* OOM */
            s_slip_rx_curr_pbuf = s_slip_rx_pbuf;
            s_slip_rx_curr_offset = 0;
            s_slip_rx_total_offset = 0;
            s_slip_rx_seg_count = 1;
            s_slip_rx_escape = false;
        }

        /* Fast path: search for next delimiter in the current block */
        const uint8_t* next_delim = NULL;
        if (!s_slip_rx_escape) {
            const uint8_t* next_end = (const uint8_t*)memchr(p, SLIP_END, end - p);
            const uint8_t* next_esc = (const uint8_t*)memchr(p, SLIP_ESC, end - p);
            
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
                    /* DoS Guard: Limit pool exhaustion. We already cap total length at 1500,
                     * but if the pool segment size is small, a frame could eat too many segments.
                     * For standard 1536-byte segments, 2 segments is the upper bound for one frame. */
                    if (s_slip_rx_seg_count >= 2) {
                        TINYPAN_LOG_ERROR("slip_rx: Frame exceeded segment limit, dropping");
                        pbuf_free(s_slip_rx_pbuf);
                        s_slip_rx_pbuf = NULL;
                        s_slip_rx_curr_pbuf = NULL;
                        s_slip_rx_curr_offset = 0;
                        s_slip_rx_total_offset = 0;
                        s_slip_rx_seg_count = 0;
                        s_slip_rx_escape = false;
                        s_slip_rx_seeking_end = true;
                        continue;
                    }

                    struct pbuf* next = pbuf_alloc(PBUF_RAW, PBUF_POOL_BUFSIZE, PBUF_POOL);
                    if (next == NULL) { /* OOM */
                        pbuf_free(s_slip_rx_pbuf);
                        s_slip_rx_pbuf = NULL;
                        return;
                    }
                    s_slip_rx_seg_count++;
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
                /* Frame too large: drop the buffer and seek forward to the next
                 * SLIP_END so any valid frames that follow in this same
                 * notification batch are not silently discarded. */
                pbuf_free(s_slip_rx_pbuf);
                s_slip_rx_pbuf = NULL;
                s_slip_rx_curr_pbuf = NULL;
                s_slip_rx_curr_offset = 0;
                s_slip_rx_total_offset = 0;
                s_slip_rx_seg_count = 0;
                s_slip_rx_escape = false;
                s_slip_rx_seeking_end = true;
                p += (chunk_len - written); /* advance past unwritten bytes */
                continue; /* outer while will handle the seek */
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
                } else {
                    /* DoS/Corruption Guard: Drop over-sized frame explicitly */
                    TINYPAN_LOG_ERROR("slip_rx: Escaped byte exceeded limit, dropping");
                    pbuf_free(s_slip_rx_pbuf);
                    s_slip_rx_pbuf = NULL;
                    s_slip_rx_curr_pbuf = NULL;
                    s_slip_rx_curr_offset = 0;
                    s_slip_rx_total_offset = 0;
                    s_slip_rx_seg_count = 0;
                    s_slip_rx_seeking_end = true;
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
            /* BLE-compliant Dynamic MTU: The operational chunk size is the minimum of
     * our static staging buffer and the current HAL-negotiated link MTU.
     * This ensures TinyPAN remains compatible with iOS (MTU 185) and Android 
     * (MTU 247) without requiring compile-time branching. */
    uint16_t hal_mtu = hal_bt_l2cap_get_mtu();
    uint16_t max_chunk = (hal_mtu < sizeof(s_slip_chunk_buf)) ? hal_mtu : sizeof(s_slip_chunk_buf);
    
    while (s_slip_tx_current != NULL && chunk_idx < max_chunk - 2) {
                uint8_t* payload = (uint8_t*)s_slip_tx_current->payload;
                while (s_slip_tx_offset < s_slip_tx_current->len && chunk_idx < max_chunk - 2) {
                    /* Bulk Copy Optimization: Use memchr to find next escape char */
                    uint16_t rem_payload = s_slip_tx_current->len - s_slip_tx_offset;
                    uint16_t rem_chunk = (max_chunk - 2) - chunk_idx;
                    uint16_t scan_len = (rem_payload < rem_chunk) ? rem_payload : rem_chunk;
                    
                    const uint8_t* next_esc = memchr(payload + s_slip_tx_offset, SLIP_END, scan_len);
                    const uint8_t* next_alt = memchr(payload + s_slip_tx_offset, SLIP_ESC, scan_len);
                    if (next_alt && (!next_esc || next_alt < next_esc)) next_esc = next_alt;

                    if (next_esc) {
                        uint16_t safe_copy = (uint16_t)(next_esc - (payload + s_slip_tx_offset));
                        if (safe_copy > 0) {
                            memcpy(s_slip_chunk_buf + chunk_idx, payload + s_slip_tx_offset, safe_copy);
                            chunk_idx += safe_copy;
                            s_slip_tx_offset += safe_copy;
                        }
                        
                        /* Escape the special character */
                        uint8_t c = payload[s_slip_tx_offset++];
                        if (c == SLIP_END) {
                            s_slip_chunk_buf[chunk_idx++] = SLIP_ESC;
                            s_slip_chunk_buf[chunk_idx++] = SLIP_ESC_END;
                        } else {
                            s_slip_chunk_buf[chunk_idx++] = SLIP_ESC;
                            s_slip_chunk_buf[chunk_idx++] = SLIP_ESC_ESC;
                        }
                    } else {
                        /* No escape characters in the remaining scan range */
                        memcpy(s_slip_chunk_buf + chunk_idx, payload + s_slip_tx_offset, scan_len);
                        chunk_idx += scan_len;
                        s_slip_tx_offset += scan_len;
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
        if (s_slip_tx_state == 2 && chunk_idx < max_chunk) {
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
    
    /* Incref the original pbuf. The drain loop will free it.
     * We avoid pbuf_clone(PBUF_RAM) because it would eat the heap. */
    pbuf_ref(p);
    struct pbuf* q = p;

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
