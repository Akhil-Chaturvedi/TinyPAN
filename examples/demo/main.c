/*
 * TinyPAN Demo - BNEP Handshake Test
 * 
 * Simple demo that validates the BNEP connection handshake works.
 * Uses mock HAL to simulate phone responses.
 */

#include <stdio.h>
#include <string.h>

#include "../include/tinypan.h"
#include "../hal/mock/tinypan_hal_mock.h"

/* Event callback */
static void event_callback(tinypan_event_t event, void* user_data) {
    (void)user_data;
    
    switch (event) {
        case TINYPAN_EVENT_STATE_CHANGED:
            printf("[APP] State: %s\n", tinypan_state_to_string(tinypan_get_state()));
            break;
        case TINYPAN_EVENT_CONNECTED:
            printf("[APP] *** BNEP CONNECTED! ***\n");
            break;
        case TINYPAN_EVENT_DISCONNECTED:
            printf("[APP] Disconnected\n");
            break;
        case TINYPAN_EVENT_IP_ACQUIRED:
            printf("[APP] IP Acquired!\n");
            break;
        default:
            break;
    }
}

int main(void) {
    printf("===========================================\n");
    printf("  TinyPAN BNEP Handshake Demo\n");
    printf("===========================================\n\n");
    
    /* Configure */
    tinypan_config_t config;
    tinypan_config_init(&config);
    
    uint8_t phone[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    memcpy(config.remote_addr, phone, 6);
    
    printf("[1] Initialize TinyPAN\n");
    if (tinypan_init(&config) != TINYPAN_OK) {
        printf("    FAIL\n");
        return 1;
    }
    printf("    OK\n\n");
    
    tinypan_set_event_callback(event_callback, NULL);
    
    printf("[2] Start connection to NAP\n");
    if (tinypan_start() != TINYPAN_OK) {
        printf("    FAIL\n");
        return 1;
    }
    printf("    State: %s\n\n", tinypan_state_to_string(tinypan_get_state()));
    
    printf("[3] Simulate: Phone accepts L2CAP\n");
    mock_hal_simulate_connect_success();
    tinypan_process();
    printf("    State: %s\n\n", tinypan_state_to_string(tinypan_get_state()));
    
    printf("[4] Simulate: Phone accepts BNEP setup\n");
    mock_hal_simulate_bnep_setup_success();
    tinypan_process();
    printf("    State: %s\n\n", tinypan_state_to_string(tinypan_get_state()));
    
    /* Verify */
    if (tinypan_get_state() == TINYPAN_STATE_DHCP) {
        printf("===========================================\n");
        printf("  SUCCESS! BNEP Handshake Complete!\n");
        printf("===========================================\n");
        printf("\n");
        printf("Protocol flow completed:\n");
        printf("  [+] L2CAP connection (PSM 0x000F)\n");
        printf("  [+] BNEP Setup Request (PANU -> NAP)\n");
        printf("  [+] BNEP Setup Response (Success 0x0000)\n");
        printf("  [+] Now in DHCP state (awaiting IP)\n");
        printf("\n");
        printf("Next: With real phone, DHCP would give us IP!\n");
    } else {
        printf("FAIL: Expected DHCP state, got %s\n", 
               tinypan_state_to_string(tinypan_get_state()));
        return 1;
    }
    
    printf("\n[5] Cleanup\n");
    tinypan_stop();
    tinypan_deinit();
    printf("    Done!\n");
    
    return 0;
}
