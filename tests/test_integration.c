/*
 * TinyPAN Integration Test - Full DHCP Flow
 * 
 * Demonstrates the complete connection and DHCP flow using simulated packets.
 * Shows what happens when TinyPAN connects to a phone and gets an IP address.
 */

#include <stdio.h>
#include <string.h>

#include "../include/tinypan.h"
#include "../include/tinypan_hal.h"
#include "../hal/mock/tinypan_hal_mock.h"
#include "../src/tinypan_bnep.h"
#include "dhcp_sim.h"

static dhcp_sim_config_t g_dhcp_config;

static tinypan_state_t g_state_history[16];
static int g_state_history_count = 0;
static int g_disconnect_count = 0;

/* Internal hook used by lwIP integration path when DHCP succeeds. */
extern void tinypan_internal_set_ip(uint32_t ip, uint32_t netmask, uint32_t gw, uint32_t dns);

/* ============================================================================
 * Event Callback
 * ============================================================================ */

static void event_callback(tinypan_event_t event, void* user_data) {
    (void)user_data;
    
    switch (event) {
        case TINYPAN_EVENT_STATE_CHANGED:
            if (g_state_history_count < (int)(sizeof(g_state_history) / sizeof(g_state_history[0]))) {
                g_state_history[g_state_history_count++] = tinypan_get_state();
            }
            printf("    State: %s\n", tinypan_state_to_string(tinypan_get_state()));
            break;
        case TINYPAN_EVENT_CONNECTED:
            printf("    *** BNEP CONNECTED ***\n");
            break;
        case TINYPAN_EVENT_IP_ACQUIRED:
            printf("    *** IP ACQUIRED! ***\n");
            break;
        case TINYPAN_EVENT_DISCONNECTED:
            g_disconnect_count++;
            printf("    Disconnected\n");
            break;
        default:
            break;
    }
}

/* ============================================================================
 * Main Test
 * ============================================================================ */

int main(void) {
    printf("=====================================================\n");
    printf("  TinyPAN Integration Test - Simulated DHCP Flow\n");
    printf("=====================================================\n\n");
    
    /* Setup DHCP simulation config */
    dhcp_sim_get_default_config(&g_dhcp_config);
    g_state_history_count = 0;
    g_disconnect_count = 0;
    
    printf("Simulated Network Configuration:\n");
    printf("  Server (NAP):  192.168.44.1\n");
    printf("  Client (PANU): 192.168.44.2\n");
    printf("  Gateway:       192.168.44.1\n");
    printf("  DNS:           8.8.8.8\n");
    printf("  Netmask:       255.255.255.0\n\n");
    
    /* Initialize TinyPAN */
    tinypan_config_t config;
    tinypan_config_init(&config);
    memcpy(config.remote_addr, g_dhcp_config.server_mac, 6);
    
    printf("[Step 1] Initialize TinyPAN\n");
    if (tinypan_init(&config) != TINYPAN_OK) {
        printf("    FAILED\n");
        return 1;
    }
    printf("    OK\n\n");
    
    tinypan_set_event_callback(event_callback, NULL);
    
    /* Start connection */
    printf("[Step 2] Start Connection\n");
    tinypan_start();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("    FAILED: Expected CONNECTING\n");
        tinypan_deinit();
        return 1;
    }
    printf("\n");
    
    /* Simulate L2CAP connect */
    printf("[Step 3] Phone Accepts L2CAP Connection\n");
    mock_hal_simulate_connect_success();
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_BNEP_SETUP) {
        printf("    FAILED: Expected BNEP_SETUP\n");
        tinypan_deinit();
        return 1;
    }
    printf("\n");
    
    /* Simulate BNEP setup */
    printf("[Step 4] Phone Accepts BNEP Setup\n");
    mock_hal_simulate_bnep_setup_success();
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_DHCP) {
        printf("    FAILED: Expected DHCP\n");
        tinypan_deinit();
        return 1;
    }
    printf("\n");
    
    /* Now in DHCP state - in a real scenario, lwIP would send DHCP Discover */
    printf("[Step 5] DHCP Exchange (Simulated)\n");
    printf("    Note: Full DHCP client runtime still depends on lwIP backend wiring.\n");
    printf("    Here we demonstrate packet format and final IP-acquired event path.\n\n");
    
    /* Manually show what packets would look like */
    printf("    DHCP DISCOVER would contain:\n");
    printf("      - BNEP header (General Ethernet)\n");
    printf("      - IP header (src=0.0.0.0, dst=255.255.255.255)\n");
    printf("      - UDP header (port 68 -> 67)\n");
    printf("      - BOOTP/DHCP message\n\n");
    
    printf("    DHCP OFFER response:\n");
    uint8_t dhcp_offer[512];
    uint8_t test_mac[] = {0x13, 0x22, 0x33, 0x44, 0x55, 0x66};
    int offer_len = dhcp_sim_build_offer(dhcp_offer, sizeof(dhcp_offer),
                                          &g_dhcp_config, 0x12345678, test_mac);
    if (offer_len > 0) {
        printf("      - Packet size: %d bytes\n", offer_len);
        printf("      - Your IP: 192.168.44.2\n");
        printf("      - Server IP: 192.168.44.1\n");
        printf("      - Lease: %lu seconds\n\n", (unsigned long)g_dhcp_config.lease_time);
    }
    
    /* Build complete BNEP-wrapped packet to show full format */
    uint8_t full_packet[1024];
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int pkt_len = dhcp_sim_build_bnep_packet(
        full_packet, sizeof(full_packet),
        g_dhcp_config.server_mac, broadcast,
        g_dhcp_config.server_ip, 0xFFFFFFFF,
        dhcp_offer, (uint16_t)offer_len
    );
    
    printf("    Complete BNEP/IP/UDP/DHCP packet:\n");
    printf("      - Total size: %d bytes\n", pkt_len);
    printf("      - BNEP header: bytes 0-14\n");
    printf("      - IP header:   bytes 15-34\n");
    printf("      - UDP header:  bytes 35-42\n");
    printf("      - DHCP:        bytes 43+\n\n");
    
    printf("    First 32 bytes of packet:\n      ");
    for (int i = 0; i < 32 && i < pkt_len; i++) {
        printf("%02X ", full_packet[i]);
        if ((i + 1) % 16 == 0) printf("\n      ");
    }
    printf("\n[Step 5] Awaiting Real DHCP IP Acquisition...\n");
    
    /* Loop and pump the TinyPAN stack. lwIP inside will realize the link is up
       and automatically format and attempt to send a real DHCP DISCOVER packet out
       to our mock L2CAP HAL! We simulate 5 seconds of time passing. */
    const uint32_t timeout_ms = 5000;
    uint32_t start_time = hal_get_tick_ms();
    bool offer_sent = false;
    bool ack_sent = false;
    
    while (!tinypan_is_online() && (hal_get_tick_ms() - start_time) < timeout_ms) {
        tinypan_process();
        
        if (!offer_sent || !ack_sent) {
            extern const uint8_t* mock_hal_get_last_tx_data(void);
            extern uint16_t mock_hal_get_last_tx_len(void);
            
            const uint8_t* tx_data = mock_hal_get_last_tx_data();
            uint16_t tx_len = mock_hal_get_last_tx_len();
            
            uint32_t xid = 0;
            uint8_t client_mac[6] = {0x12, 0x22, 0x33, 0x44, 0x55, 0x66};
            if (!offer_sent && dhcp_sim_is_discover(tx_data, tx_len, &xid, NULL)) {
                printf("[Step 5b] Intercepted lwIP DHCP DISCOVER (XID: 0x%08X)\n", xid);
                printf("[Step 5c] Automatically generating and injecting NAP DHCP OFFER response...\n");
                
                uint8_t dhcp_offer[512];
                int offer_len = dhcp_sim_build_offer(dhcp_offer, sizeof(dhcp_offer),
                                                      &g_dhcp_config, xid, client_mac);
                
                uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                uint8_t full_packet[1024];
                int pkt_len = dhcp_sim_build_bnep_packet(
                    full_packet, sizeof(full_packet),
                    g_dhcp_config.server_mac, broadcast,
                    g_dhcp_config.server_ip, 0xFFFFFFFF,
                    dhcp_offer, (uint16_t)offer_len
                );
                
                extern void tinypan_netif_input(const uint8_t* dst, const uint8_t* src, uint16_t ethertype, const uint8_t* payload, uint16_t payload_len);
                tinypan_netif_input(full_packet + 1, full_packet + 7, ((uint16_t)full_packet[13] << 8) | full_packet[14], full_packet + 15, pkt_len - 15);
                offer_sent = true;
                
                printf("[Step 5c] Injected OFFER Packet Hex (%d bytes):\n", pkt_len);
                for (int i = 0; i < pkt_len; i++) {
                    printf("%02X ", full_packet[i]);
                    if ((i + 1) % 16 == 0) printf("\n");
                }
                printf("\n");
                
                /* Fast-forward a bit to give lwIP time to process the OFFER */
                extern void mock_hal_advance_tick_ms(uint32_t delta_ms);
                mock_hal_advance_tick_ms(50);
                continue;
                
            } else if (offer_sent && !ack_sent && dhcp_sim_is_request(tx_data, tx_len, &xid)) {
                printf("[Step 5d] Intercepted lwIP DHCP REQUEST (XID: 0x%08X)\n", xid);
                printf("[Step 5e] Automatically generating and injecting NAP DHCP ACK response...\n");
                
                uint8_t dhcp_ack[512];
                int ack_len = dhcp_sim_build_ack(dhcp_ack, sizeof(dhcp_ack),
                                                  &g_dhcp_config, xid, client_mac);
                
                uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                uint8_t full_packet[1024];
                int pkt_len = dhcp_sim_build_bnep_packet(
                    full_packet, sizeof(full_packet),
                    g_dhcp_config.server_mac, broadcast,
                    g_dhcp_config.server_ip, 0xFFFFFFFF,
                    dhcp_ack, (uint16_t)ack_len
                );
                
                extern void tinypan_netif_input(const uint8_t* dst, const uint8_t* src, uint16_t ethertype, const uint8_t* payload, uint16_t payload_len);
                tinypan_netif_input(full_packet + 1, full_packet + 7, ((uint16_t)full_packet[13] << 8) | full_packet[14], full_packet + 15, pkt_len - 15);
                ack_sent = true;
                
                /* Fast-forward to let lwIP process ACK and setup IP */
                extern void mock_hal_advance_tick_ms(uint32_t delta_ms);
                mock_hal_advance_tick_ms(50);
                continue;
            }
        }
        
        /* Fast-forward simulated time by 10ms and pump again */
        extern void mock_hal_advance_tick_ms(uint32_t delta_ms);
        mock_hal_advance_tick_ms(10);
    }
    
    /* Validate state callback sequence */
    const tinypan_state_t expected_states[] = {
        TINYPAN_STATE_CONNECTING,
        TINYPAN_STATE_BNEP_SETUP,
        TINYPAN_STATE_DHCP,
        TINYPAN_STATE_ONLINE
    };
    if (g_state_history_count < (int)(sizeof(expected_states) / sizeof(expected_states[0]))) {
        printf("    FAILED: State event history too short (%d)\n", g_state_history_count);
        tinypan_deinit();
        return 1;
    }
    for (int i = 0; i < (int)(sizeof(expected_states) / sizeof(expected_states[0])); i++) {
        if (g_state_history[i] != expected_states[i]) {
            printf("    FAILED: State event %d mismatch (expected %s, got %s)\n",
                   i,
                   tinypan_state_to_string(expected_states[i]),
                   tinypan_state_to_string(g_state_history[i]));
            tinypan_deinit();
            return 1;
        }
    }
    
    printf("\n=====================================================\n");
    printf("  Summary\n");
    printf("=====================================================\n\n");
    
    printf("Current State: %s\n\n", tinypan_state_to_string(tinypan_get_state()));
    
    if (tinypan_is_online()) {
        tinypan_ip_info_t info;
        if (tinypan_get_ip_info(&info) == TINYPAN_OK) {
            printf("IP Acquisition SUCCESS! lwIP works!\n");
            printf("  IP Address: %d.%d.%d.%d\n",
                   (info.ip_addr & 0xFF), (info.ip_addr >> 8) & 0xFF,
                   (info.ip_addr >> 16) & 0xFF, (info.ip_addr >> 24) & 0xFF);
            printf("  Netmask:    %d.%d.%d.%d\n",
                   (info.netmask & 0xFF), (info.netmask >> 8) & 0xFF,
                   (info.netmask >> 16) & 0xFF, (info.netmask >> 24) & 0xFF);
            printf("  Gateway:    %d.%d.%d.%d\n",
                   (info.gateway & 0xFF), (info.gateway >> 8) & 0xFF,
                   (info.gateway >> 16) & 0xFF, (info.gateway >> 24) & 0xFF);
        }
    } else {
        printf("FAILED: Did not go ONLINE within %u ms.\n", timeout_ms);
        printf("(If expecting a full mock, the HAL needs to mock a DHCP OFFER response\n"
               " to the DHCP DISCOVER packet lwIP sent!).\n");
        tinypan_deinit();
        return 1;
    }
    
    printf("\nWhat we demonstrated:\n");
    printf("  [✓] L2CAP connection (PSM 0x000F)\n");
    printf("  [✓] BNEP handshake (PANU -> NAP)\n");
    printf("  [✓] lwIP dynamically bridged to BNEP\n");
    printf("  [✓] DHCP DISCOVER automatically transmitted\n\n");
    
    /* Cleanup */
    printf("[Step 6] Cleanup\n");
    tinypan_stop();

    if (g_disconnect_count != 1) {
        printf("    FAILED: Expected 1 disconnect event after stop, got %d\n", g_disconnect_count);
        tinypan_deinit();
        return 1;
    }

    tinypan_deinit();
    printf("    Done!\n\n");
    
    printf("=====================================================\n");
    printf("  Test Complete - All protocol layers working!\n");
    printf("=====================================================\n");
    
    return 0;
}
