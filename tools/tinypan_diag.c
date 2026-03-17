/*
 * TinyPAN Network Diagnostics Tool
 * 
 * Provides an optional API for engineers to monitor link state,
 * IP assignment, and transport activity without hooking callbacks manually.
 *
 * Usage: Call tinypan_diag_print_status() from your application loop
 * or a debug command handler. Requires <stdio.h> (printf).
 */

#include "tinypan.h"
#include <stdio.h>

void tinypan_diag_print_status(void) {
    printf("=== TinyPAN Diagnostics ===\n");
    
    /* 1. Link State */
    tinypan_state_t state = tinypan_get_state();
    printf("Link State:       %s\n", tinypan_state_to_string(state));
    printf("Online:           %s\n", tinypan_is_online() ? "yes" : "no");
    
    /* 2. IP Configuration */
    tinypan_ip_info_t info;
    if (tinypan_get_ip_info(&info) == TINYPAN_OK) {
        uint32_t ip = info.ip_addr;
        uint32_t gw = info.gateway;
        printf("IP Address:       %u.%u.%u.%u\n",
            (unsigned)(ip & 0xFF),
            (unsigned)((ip >> 8) & 0xFF),
            (unsigned)((ip >> 16) & 0xFF),
            (unsigned)((ip >> 24) & 0xFF));
        printf("Gateway:          %u.%u.%u.%u\n",
            (unsigned)(gw & 0xFF),
            (unsigned)((gw >> 8) & 0xFF),
            (unsigned)((gw >> 16) & 0xFF),
            (unsigned)((gw >> 24) & 0xFF));
    } else {
        printf("IP Address:       0.0.0.0 (Unassigned)\n");
    }
    
    printf("===========================\n");
}
