#ifndef TINYPAN_INTERNAL_H
#define TINYPAN_INTERNAL_H

#include "../include/tinypan.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Internal callback to notify the core application of IP acquisition.
 * 
 * Used by the netif layer to push IP state changes back into the supervisor.
 */
void tinypan_internal_set_ip(uint32_t ip, uint32_t netmask, uint32_t gw, uint32_t dns);

/**
 * @brief Internal callback to clear IP state on disconnect.
 *
 * Called by the supervisor when Bluetooth disconnects, ensuring
 * tinypan_is_online() returns false until DHCP completes again.
 */
void tinypan_internal_clear_ip(void);

const tinypan_config_t* tinypan_internal_get_config(void);

/**
 * @brief Returns the exact number of milliseconds until the next supervisor state timeout.
 */
uint32_t supervisor_get_next_timeout_ms(void);

#ifdef __cplusplus
}
#endif

#endif // TINYPAN_INTERNAL_H
