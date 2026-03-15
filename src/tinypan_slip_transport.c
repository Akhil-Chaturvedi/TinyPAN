/*
 * TinyPAN SLIP Transport Backend
 *
 * Implements tinypan_transport_t for BLE SLIP companion mode.
 * Only compiled when TINYPAN_USE_BLE_SLIP is 1.
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
#include "netif/slipif.h"
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

#if TINYPAN_ENABLE_LWIP

/* RX ring buffer: incoming BLE UART bytes queued here until slipif drains them */
static uint8_t s_slip_rx_queue[TINYPAN_RX_BUFFER_SIZE];
static uint16_t s_slip_rx_head = 0;
static uint16_t s_slip_rx_tail = 0;

/* TX ring buffer: fully framed SLIP packets waiting for L2CAP readiness */
static struct pbuf* s_slip_tx_queue[TINYPAN_TX_QUEUE_LEN] = {0};
static uint8_t s_slip_tx_head = 0;
static uint8_t s_slip_tx_tail = 0;

/* lwIP serial I/O adapter required by slipif */
#include "lwip/sio.h"

sio_fd_t sio_open(u8_t devnum) {
    (void)devnum;
    return (sio_fd_t)1; /* Dummy handle */
}

u32_t sio_read(sio_fd_t fd, u8_t *data, u32_t len) {
    (void)fd;
    u32_t copied = 0;
    while (copied < len && s_slip_rx_head != s_slip_rx_tail) {
        data[copied++] = s_slip_rx_queue[s_slip_rx_tail];
        s_slip_rx_tail = (s_slip_rx_tail + 1) % TINYPAN_RX_BUFFER_SIZE;
    }
    return copied;
}

void sio_send(u8_t c, sio_fd_t fd) {
    /* slipif uses netif->linkoutput for TX, not sio_send. This
       stub exists only to satisfy the linker. */
    (void)c;
    (void)fd;
}

#endif /* TINYPAN_ENABLE_LWIP */

static void slip_transport_handle_incoming(const uint8_t* data, uint16_t len) {
#if TINYPAN_ENABLE_LWIP
    if (len == 0 || data == NULL) return;

    for (uint16_t i = 0; i < len; i++) {
        uint16_t next_head = (s_slip_rx_head + 1) % TINYPAN_RX_BUFFER_SIZE;
        if (next_head == s_slip_rx_tail) {
            TINYPAN_LOG_WARN("transport_slip: RX queue overflow");
            break;
        }
        s_slip_rx_queue[s_slip_rx_head] = data[i];
        s_slip_rx_head = next_head;
    }

    struct netif* netif = tinypan_netif_get();
    if (netif) {
        slipif_process_rxqueue(netif);
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
        int result = hal_bt_l2cap_send(q->payload, q->tot_len);
        if (result == 0) {
            s_slip_tx_head = (s_slip_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        } else if (result > 0) {
            hal_bt_l2cap_request_can_send_now();
            break;
        } else {
            s_slip_tx_head = (s_slip_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
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
}

static int slip_transport_output(struct netif* netif, struct pbuf* p) {
    (void)netif;
    if (p == NULL) return ERR_ARG;

    bool can_send_now = (s_slip_tx_head == s_slip_tx_tail) && hal_bt_l2cap_can_send();
    if (can_send_now) {
        struct pbuf* send_p = p;
        if (p->next != NULL) {
            send_p = pbuf_clone(PBUF_LINK, PBUF_RAM, p);
            if (send_p == NULL) return ERR_MEM;
        }

        int result = hal_bt_l2cap_send(send_p->payload, send_p->tot_len);
        if (send_p != p) { pbuf_free(send_p); }

        if (result == 0) return ERR_OK;
        if (result < 0) return ERR_IF;
        /* Fallthrough: radio became busy (race condition). Queue it below. */
    }

    struct pbuf* q = pbuf_clone(PBUF_LINK, PBUF_RAM, p);
    if (!q) return ERR_MEM;

    if (s_slip_tx_head == s_slip_tx_tail) {
        if (!hal_bt_l2cap_can_send()) {
            hal_bt_l2cap_request_can_send_now();
        } else {
            int result = hal_bt_l2cap_send(q->payload, q->tot_len);
            if (result == 0) { pbuf_free(q); return ERR_OK; }
            else if (result < 0) { pbuf_free(q); return ERR_IF; }
            else hal_bt_l2cap_request_can_send_now();
        }
    }

    uint8_t next_tail = (s_slip_tx_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    if (next_tail == s_slip_tx_head) {
        pbuf_free(q);
        return ERR_MEM;
    }

    s_slip_tx_queue[s_slip_tx_tail] = q;
    s_slip_tx_tail = next_tail;
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
