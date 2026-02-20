/*
 * TinyPAN lwIP Stub Hooks
 *
 * Provides link-safe no-op implementations for lwIP netif hooks while
 * full lwIP backend wiring is still in progress.
 */

#include "tinypan_lwip_netif.h"
#include "../include/tinypan_config.h"
#include <stddef.h>

int tinypan_netif_init(void) {
    TINYPAN_LOG_WARN("lwIP hook enabled, but backend is stubbed (netif init no-op)");
    return 0;
}

void tinypan_netif_deinit(void) {
    /* no-op */
}

int tinypan_netif_start_dhcp(void) {
    TINYPAN_LOG_WARN("lwIP hook enabled, but backend is stubbed (DHCP start no-op)");
    return 0;
}

void tinypan_netif_stop_dhcp(void) {
    /* no-op */
}

void tinypan_netif_set_link(bool up) {
    (void)up;
}

void tinypan_netif_input(const uint8_t* data, uint16_t len) {
    (void)data;
    (void)len;
}

struct netif* tinypan_netif_get(void) {
    return NULL;
}

bool tinypan_netif_has_ip(void) {
    return false;
}

uint32_t tinypan_netif_get_ip(void) {
    return 0;
}

uint32_t tinypan_netif_get_gateway(void) {
    return 0;
}

uint32_t tinypan_netif_get_netmask(void) {
    return 0;
}

void tinypan_netif_process(void) {
    /* no-op */
}
