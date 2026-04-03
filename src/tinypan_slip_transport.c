/*
 * TinyPAN SLIP Transport Backend
 *
 * Implements tinypan_transport_t for BLE SLIP companion mode.
 *
 * TX: Encodes outgoing pbuf chains into a staging buffer using a
 * single-pass C loop. Flushed via hal_bt_l2cap_send().
 *
 * RX: Accumulates incoming bytes from the HAL into a static accumulator.
 * Frame completion triggers a single pbuf_alloc (PBUF_POOL) and pbuf_take.
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
    if (s_slip_tx_mutex == NULL) {
        s_slip_tx_mutex = hal_mutex_create();
    }
    return 0;
}

static void slip_transport_on_connected(void) {
    /* No setup phase for SLIP */
}

static void slip_transport_on_disconnected(void) {
#if TINYPAN_ENABLE_LWIP
    /* Reset RX framing */
    s_slip_rx_len = 0;
    s_slip_rx_escape = false;
    s_slip_rx_seeking_end = false;
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
static uint8_t  s_slip_rx_buf[TINYPAN_RX_BUFFER_SIZE]; /* Static accumulator sized for maximum BNEP MTU */
static uint16_t s_slip_rx_len = 0;
static bool s_slip_rx_escape = false;
static bool s_slip_rx_seeking_end = false;

/* SLIP Interface variables */
static struct pbuf* s_slip_tx_queue[TINYPAN_TX_QUEUE_LEN] = {0};
static uint8_t s_slip_tx_head = 0;
static uint8_t s_slip_tx_tail = 0;
static hal_mutex_t s_slip_tx_mutex = NULL;

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

        /* Process character */
        uint8_t c = *p++;
        
        if (s_slip_rx_escape) {
            s_slip_rx_escape = false;
            if (c == SLIP_ESC_END) c = SLIP_END;
            else if (c == SLIP_ESC_ESC) c = SLIP_ESC;
            else {
                TINYPAN_LOG_ERROR("slip_rx: Invalid escape sequence");
                s_slip_rx_len = 0;
                s_slip_rx_seeking_end = true;
                continue;
            }
        } else if (c == SLIP_ESC) {
            s_slip_rx_escape = true;
            continue;
        } else if (c == SLIP_END) {
            /* Frame delimiter: allocate exactly once and pass to lwIP */
            if (s_slip_rx_len > 0) {
                struct pbuf* p = pbuf_alloc(PBUF_RAW, s_slip_rx_len, PBUF_POOL);
                if (p) {
                    pbuf_take(p, s_slip_rx_buf, s_slip_rx_len);
                    struct netif* netif = tinypan_netif_get();
                    if (netif && netif->input(p, netif) != ERR_OK) {
                        pbuf_free(p);
                    } else if (!netif) {
                        pbuf_free(p);
                    }
                } else {
                    TINYPAN_LOG_ERROR("slip_rx: pbuf_alloc failed");
                }
                s_slip_rx_len = 0;
            }
            continue;
        }

        /* Buffer the byte directly */
        if (s_slip_rx_len < sizeof(s_slip_rx_buf)) {
            s_slip_rx_buf[s_slip_rx_len++] = c;
        } else {
            TINYPAN_LOG_ERROR("slip_rx: Frame too large, dropping");
            s_slip_rx_len = 0;
            s_slip_rx_seeking_end = true;
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
    hal_mutex_lock(s_slip_tx_mutex);
    if (s_slip_tx_head == s_slip_tx_tail) {
        hal_mutex_unlock(s_slip_tx_mutex);
        return;
    }

    while (s_slip_tx_head != s_slip_tx_tail) {
        if (!hal_bt_l2cap_can_send()) {
            hal_bt_l2cap_request_can_send_now();
            hal_mutex_unlock(s_slip_tx_mutex);
            break;
        }

        /* If we have a pending chunk from a previous busy state, try sending it first */
        if (s_slip_chunk_len > 0) {
            int result = hal_bt_l2cap_send(s_slip_chunk_buf, s_slip_chunk_len);
            if (result > 0) {
                hal_bt_l2cap_request_can_send_now();
                hal_mutex_unlock(s_slip_tx_mutex);
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
    
    /* Prevent runtime integer underflow/overflow if MTU is abnormally small. 
     * If link is unusable, drop the packet to prevent queue stalls. */
    if (max_chunk < 4) {
        TINYPAN_LOG_ERROR("slip_tx: MTU %u too small for SLIP, dropping packet", hal_mtu);
        goto drop_packet;
    }
    
    /* Worst-Case Expansion Note
     * SLIP guarantees frame integrity by escaping 0xC0 (END) and 0xDB (ESC). 
     * In the absolute worst case where an entire IP payload consists exclusively 
     * of these bytes, the payload size will exactly double during encoding. 
     * The `max_chunk - 2` logic below ensures the buffer will never overflow 
     * even with peak back-to-back escapes, but integrators pushing maximum UDP 
     * throughput should be aware that worst-case payloads will take twice as 
     * long to transmit over the BLE link due to this expansion.
     */
    /* Efficient single-pass encoder. Relies on compiler optimization
     * of branch patterns for small BLE frames. */
    while (s_slip_tx_current != NULL && chunk_idx < max_chunk - 2) {
        uint8_t c = *((uint8_t*)s_slip_tx_current->payload + s_slip_tx_offset);
        if (c == SLIP_END) {
            s_slip_chunk_buf[chunk_idx++] = SLIP_ESC;
            s_slip_chunk_buf[chunk_idx++] = SLIP_ESC_END;
        } else if (c == SLIP_ESC) {
            s_slip_chunk_buf[chunk_idx++] = SLIP_ESC;
            s_slip_chunk_buf[chunk_idx++] = SLIP_ESC_ESC;
        } else {
            s_slip_chunk_buf[chunk_idx++] = c;
        }
        
        s_slip_tx_offset++;
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
                hal_mutex_unlock(s_slip_tx_mutex);
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
    hal_mutex_unlock(s_slip_tx_mutex);
}

static void slip_transport_process(void) {
    /* Maintenance hook for future use (e.g. flow control timeouts) */
}

void slip_transport_flush_tx_queue(void) {
    hal_mutex_lock(s_slip_tx_mutex);
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
    hal_mutex_unlock(s_slip_tx_mutex);
}

static int slip_transport_output(struct netif* netif, struct pbuf* p) {
    (void)netif;
    if (p == NULL) return ERR_ARG;
    
    /* Incref the original pbuf. The drain loop will free it.
     * We avoid pbuf_clone(PBUF_RAM) because it would eat the heap. */
    pbuf_ref(p);
    struct pbuf* q = p;

    uint8_t next_tail = (s_slip_tx_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    hal_mutex_lock(s_slip_tx_mutex);
    if (next_tail == s_slip_tx_head) {
        hal_mutex_unlock(s_slip_tx_mutex);
        pbuf_free(q);
        return ERR_MEM;
    }

    s_slip_tx_queue[s_slip_tx_tail] = q;
    s_slip_tx_tail = next_tail;
    hal_mutex_unlock(s_slip_tx_mutex);
    
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
    .process = slip_transport_process,
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
