/*
 * TinyPAN Test - Supervisor State Machine Tests
 * 
 * Tests for connection supervisor, state transitions, timeouts, and reconnection.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../include/tinypan.h"
#include "../include/tinypan_hal.h"
#include "../hal/mock/tinypan_hal_mock.h"
#include "../src/tinypan_bnep.h"
#include "../src/tinypan_supervisor.h"

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

static tinypan_state_t last_state = TINYPAN_STATE_IDLE;
static int event_count = 0;

#define TEST(name) \
    do { \
        printf("  Testing: %s... ", #name); \
        tests_run++; \
        reset_test_state(); \
        if (test_##name()) { \
            printf("PASSED\n"); \
            tests_passed++; \
        } else { \
            printf("FAILED\n"); \
        } \
    } while(0)

static void reset_test_state(void) {
    last_state = TINYPAN_STATE_IDLE;
    event_count = 0;
}

static void event_callback(tinypan_event_t event, void* user_data) {
    (void)user_data;
    event_count++;
    if (event == TINYPAN_EVENT_STATE_CHANGED) {
        last_state = tinypan_get_state();
    }
}

static tinypan_config_t get_test_config(void) {
    tinypan_config_t config;
    tinypan_config_init(&config);
    uint8_t addr[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    memcpy(config.remote_addr, addr, 6);
    config.reconnect_interval_ms = 100;  /* Fast for testing */
    config.reconnect_max_ms = 1000;
    config.max_reconnect_attempts = 3;
    return config;
}

/* ============================================================================
 * Test Cases
 * ============================================================================ */

/**
 * Test: Initial state is IDLE after init
 */
static int test_initial_state_idle(void) {
    tinypan_config_t config = get_test_config();
    
    if (tinypan_init(&config) != TINYPAN_OK) {
        printf("\n    Init failed\n");
        return 0;
    }
    
    if (tinypan_get_state() != TINYPAN_STATE_IDLE) {
        printf("\n    Expected IDLE, got %s\n", 
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        return 0;
    }
    
    tinypan_deinit();
    return 1;
}

/**
 * Test: Start transitions to CONNECTING
 */
static int test_start_transitions_to_connecting(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_set_event_callback(event_callback, NULL);
    
    tinypan_start();
    
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING, got %s\n", 
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        return 0;
    }
    
    tinypan_deinit();
    return 1;
}

/**
 * Test: L2CAP connect success transitions to BNEP_SETUP
 */
static int test_l2cap_connect_transitions_to_bnep_setup(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_start();
    
    /* Simulate L2CAP connect success */
    mock_hal_simulate_connect_success();
    tinypan_process();
    
    if (tinypan_get_state() != TINYPAN_STATE_BNEP_SETUP) {
        printf("\n    Expected BNEP_SETUP, got %s\n", 
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        return 0;
    }
    
    tinypan_deinit();
    return 1;
}

/**
 * Test: BNEP setup success transitions to DHCP
 */
static int test_bnep_setup_success_transitions_to_dhcp(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_start();
    
    mock_hal_simulate_connect_success();
    tinypan_process();
    
    mock_hal_simulate_bnep_setup_success();
    tinypan_process();
    
    if (tinypan_get_state() != TINYPAN_STATE_DHCP) {
        printf("\n    Expected DHCP, got %s\n", 
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        return 0;
    }
    
    tinypan_deinit();
    return 1;
}

/**
 * Test: L2CAP connect failure triggers reconnection
 */
static int test_l2cap_failure_triggers_reconnect(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_start();
    
    /* Simulate connect failure */
    mock_hal_simulate_connect_failure(-1);
    tinypan_process();
    
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING, got %s\n", 
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        return 0;
    }
    
    tinypan_deinit();
    return 1;
}

/**
 * Test: Disconnect during DHCP triggers reconnection
 */
static int test_disconnect_during_dhcp_triggers_reconnect(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_start();
    
    /* Get to DHCP state */
    mock_hal_simulate_connect_success();
    tinypan_process();
    mock_hal_simulate_bnep_setup_success();
    tinypan_process();
    
    /* Now disconnect */
    mock_hal_simulate_disconnect();
    tinypan_process();
    
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING, got %s\n", 
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        return 0;
    }
    
    tinypan_deinit();
    return 1;
}

/**
 * Test: Stop resets to IDLE
 */
static int test_stop_resets_to_idle(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_start();
    
    mock_hal_simulate_connect_success();
    tinypan_process();
    mock_hal_simulate_bnep_setup_success();
    tinypan_process();
    
    /* Now stop */
    tinypan_stop();
    
    if (tinypan_get_state() != TINYPAN_STATE_IDLE) {
        printf("\n    Expected IDLE, got %s\n", 
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        return 0;
    }
    
    tinypan_deinit();
    return 1;
}

/**
 * Test: BNEP setup rejection triggers reconnection
 */
static int test_bnep_rejection_triggers_reconnect(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_start();
    
    mock_hal_simulate_connect_success();
    tinypan_process();
    
    /* Send BNEP setup rejection (0x0004 = Not Allowed) */
    uint8_t rejection[] = {0x01, 0x02, 0x00, 0x04};
    mock_hal_simulate_receive(rejection, sizeof(rejection));
    tinypan_process();
    
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING, got %s\n", 
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        return 0;
    }
    
    tinypan_deinit();
    return 1;
}

/**
 * Test: State string conversion
 */
static int test_state_to_string(void) {
    if (strcmp(tinypan_state_to_string(TINYPAN_STATE_IDLE), "IDLE") != 0) {
        printf("\n    IDLE mismatch\n");
        return 0;
    }
    if (strcmp(tinypan_state_to_string(TINYPAN_STATE_CONNECTING), "CONNECTING") != 0) {
        printf("\n    CONNECTING mismatch\n");
        return 0;
    }
    if (strcmp(tinypan_state_to_string(TINYPAN_STATE_BNEP_SETUP), "BNEP_SETUP") != 0) {
        printf("\n    BNEP_SETUP mismatch\n");
        return 0;
    }
    if (strcmp(tinypan_state_to_string(TINYPAN_STATE_DHCP), "DHCP") != 0) {
        printf("\n    DHCP mismatch\n");
        return 0;
    }
    if (strcmp(tinypan_state_to_string(TINYPAN_STATE_ONLINE), "ONLINE") != 0) {
        printf("\n    ONLINE mismatch\n");
        return 0;
    }
    return 1;
}

/**
 * Test: Config defaults are set correctly
 */
static int test_config_defaults(void) {
    tinypan_config_t config;
    tinypan_config_init(&config);
    
    if (config.reconnect_interval_ms != 1000) {
        printf("\n    reconnect_interval_ms wrong: %u\n", config.reconnect_interval_ms);
        return 0;
    }
    if (config.reconnect_max_ms != 30000) {
        printf("\n    reconnect_max_ms wrong: %u\n", config.reconnect_max_ms);
        return 0;
    }
    if (config.heartbeat_interval_ms != 15000) {
        printf("\n    heartbeat_interval_ms wrong: %u\n", config.heartbeat_interval_ms);
        return 0;
    }
    if (config.heartbeat_retries != 3) {
        printf("\n    heartbeat_retries wrong: %u\n", config.heartbeat_retries);
        return 0;
    }
    
    return 1;
}

/**
 * Test: Full connection flow
 */
static int test_full_connection_flow(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_set_event_callback(event_callback, NULL);
    
    /* Start */
    tinypan_start();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Step 1 failed\n");
        tinypan_deinit();
        return 0;
    }
    
    /* L2CAP connect */
    mock_hal_simulate_connect_success();
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_BNEP_SETUP) {
        printf("\n    Step 2 failed\n");
        tinypan_deinit();
        return 0;
    }
    
    /* BNEP setup */
    mock_hal_simulate_bnep_setup_success();
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_DHCP) {
        printf("\n    Step 3 failed\n");
        tinypan_deinit();
        return 0;
    }
    
    tinypan_deinit();
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("TinyPAN Supervisor Tests\n");
    printf("========================\n\n");
    
    printf("Running tests:\n");
    
    TEST(initial_state_idle);
    TEST(start_transitions_to_connecting);
    TEST(l2cap_connect_transitions_to_bnep_setup);
    TEST(bnep_setup_success_transitions_to_dhcp);
    TEST(l2cap_failure_triggers_reconnect);
    TEST(disconnect_during_dhcp_triggers_reconnect);
    TEST(stop_resets_to_idle);
    TEST(bnep_rejection_triggers_reconnect);
    TEST(state_to_string);
    TEST(config_defaults);
    TEST(full_connection_flow);
    
    printf("\n========================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
    
    return (tests_passed == tests_run) ? 0 : 1;
}
