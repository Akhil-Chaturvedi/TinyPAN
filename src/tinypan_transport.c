/*
 * TinyPAN Transport Factory
 */

#include "tinypan_transport.h"
#include "../include/tinypan_config.h"

extern const tinypan_transport_t transport_bnep;
extern const tinypan_transport_t transport_slip;

const tinypan_transport_t* tinypan_transport_get(void) {
#if TINYPAN_USE_BLE_SLIP
    return &transport_slip;
#else
    return &transport_bnep;
#endif
}
