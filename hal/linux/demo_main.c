/*
 * TinyPAN Linux Demo
 * 
 * Connects to a phone via Bluetooth PAN and attempts to get an IP.
 * 
 * Usage: ./demo_linux AA:BB:CC:DD:EE:FF
 *        where AA:BB... is the phone's Bluetooth address
 * 
 * Before running:
 * 1. Enable Bluetooth Tethering on your Android phone
 * 2. Pair the phone with this computer
 * 3. Run this with the phone's Bluetooth address
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "../../include/tinypan.h"

/* Forward declaration for BlueZ HAL poll function */
extern void hal_bt_poll(void);

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    printf("\nInterrupted, stopping...\n");
    g_running = 0;
}

static void event_callback(tinypan_event_t event, void* user_data) {
    (void)user_data;
    
    switch (event) {
        case TINYPAN_EVENT_STATE_CHANGED:
            printf("[EVENT] State: %s\n", tinypan_state_to_string(tinypan_get_state()));
            break;
        case TINYPAN_EVENT_CONNECTED:
            printf("[EVENT] *** BNEP CONNECTED! ***\n");
            break;
        case TINYPAN_EVENT_DISCONNECTED:
            printf("[EVENT] Disconnected\n");
            break;
        case TINYPAN_EVENT_IP_ACQUIRED:
            {
                tinypan_ip_info_t info;
                if (tinypan_get_ip_info(&info) == TINYPAN_OK) {
                    printf("[EVENT] *** IP ACQUIRED! ***\n");
                    printf("  IP:      %d.%d.%d.%d\n",
                           (info.ip_addr >> 0) & 0xFF,
                           (info.ip_addr >> 8) & 0xFF,
                           (info.ip_addr >> 16) & 0xFF,
                           (info.ip_addr >> 24) & 0xFF);
                    printf("  Gateway: %d.%d.%d.%d\n",
                           (info.gateway >> 0) & 0xFF,
                           (info.gateway >> 8) & 0xFF,
                           (info.gateway >> 16) & 0xFF,
                           (info.gateway >> 24) & 0xFF);
                }
            }
            break;
        case TINYPAN_EVENT_ERROR:
            printf("[EVENT] Error!\n");
            break;
        default:
            break;
    }
}

static int parse_bdaddr(const char* str, uint8_t addr[6]) {
    unsigned int a[6];
    if (sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        addr[i] = (uint8_t)a[i];
    }
    return 0;
}

int main(int argc, char* argv[]) {
    printf("========================================\n");
    printf("  TinyPAN Linux Demo\n");
    printf("========================================\n\n");
    
    if (argc != 2) {
        printf("Usage: %s <phone-bluetooth-address>\n", argv[0]);
        printf("Example: %s AA:BB:CC:DD:EE:FF\n\n", argv[0]);
        printf("Before running:\n");
        printf("  1. Enable Bluetooth Tethering on your Android phone\n");
        printf("  2. Pair the phone with this computer\n");
        printf("  3. Find phone's address with: bluetoothctl devices\n");
        return 1;
    }
    
    /* Parse phone address */
    uint8_t phone_addr[6];
    if (parse_bdaddr(argv[1], phone_addr) < 0) {
        printf("Invalid Bluetooth address: %s\n", argv[1]);
        printf("Expected format: AA:BB:CC:DD:EE:FF\n");
        return 1;
    }
    
    printf("Target phone: %s\n\n", argv[1]);
    
    /* Setup signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Configure TinyPAN */
    tinypan_config_t config;
    tinypan_config_init(&config);
    memcpy(config.remote_addr, phone_addr, 6);
    
    /* Initialize */
    printf("[INIT] Initializing TinyPAN...\n");
    tinypan_error_t err = tinypan_init(&config);
    if (err != TINYPAN_OK) {
        printf("[INIT] Failed: %d\n", err);
        return 1;
    }
    
    tinypan_set_event_callback(event_callback, NULL);
    
    /* Start connection */
    printf("[INIT] Starting connection...\n");
    err = tinypan_start();
    if (err != TINYPAN_OK) {
        printf("[INIT] Failed to start: %d\n", err);
        tinypan_deinit();
        return 1;
    }
    
    printf("[INIT] Running... (Ctrl+C to stop)\n\n");
    
    /* Main loop */
    while (g_running) {
        /* Poll BlueZ socket */
        hal_bt_poll();
        
        /* Process TinyPAN state machine */
        tinypan_process();
        
        /* Small sleep to avoid busy loop */
        usleep(10000);  /* 10ms */
    }
    
    /* Cleanup */
    printf("\n[EXIT] Cleaning up...\n");
    tinypan_stop();
    tinypan_deinit();
    
    printf("[EXIT] Done!\n");
    return 0;
}
