/*
 * TinyPAN Network Diagnostics Tool
 * 
 * Provides an optional API for engineers to monitor link state,
 * IP assignment, and packet paths without hooking callbacks manually.
 */

#include "tinypan.h"
#include "tinypan_transport.h"
#include <stdio.h>

void tinypan_diag_print_status(void) {
    printf("=== TinyPAN Diagnostics ===\n");
    
    /* 1. Transport Information */
    const tinypan_transport_t* transport = tinypan_transport_get();
    printf("Active Transport: %s\n", transport ? transport->name : "None");
    
    /* 2. Link State */
    tinypan_state_t state = tinypan_get_state();
    const char* str_state = "UNKNOWN";
    switch (state) {
        case TINYPAN_STATE_DISCONNECTED: str_state = "DISCONNECTED"; break;
        case TINYPAN_STATE_CONNECTING:   str_state = "CONNECTING"; break;
        case TINYPAN_STATE_BNEP_SETUP:   str_state = "SETUP"; break;
        case TINYPAN_STATE_DHCP:          str_state = "DHCP"; break;
        case TINYPAN_STATE_ONLINE:        str_state = "ONLINE"; break;
    }
    printf("Link State:       %s\n", str_state);
    
    /* 3. IP Configuration */
    uint32_t ip = tinypan_get_ip();
    if (ip != 0) {
        printf("IP Address:       %u.%u.%u.%u\n",
            (unsigned int)(ip & 0xFF),
            (unsigned int)((ip >> 8) & 0xFF),
            (unsigned int)((ip >> 16) & 0xFF),
            (unsigned int)((ip >> 24) & 0xFF));
    } else {
        printf("IP Address:       0.0.0.0 (Unassigned)\n");
    }
    
    printf("===========================\n");
}
