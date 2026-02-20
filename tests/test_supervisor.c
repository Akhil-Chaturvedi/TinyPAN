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

extern void tinypan_internal_set_ip(uint32_t ip, uint32_t netmask, uint32_t gw, uint32_t dns);
extern void tinypan_internal_clear_ip(void);

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

static tinypan_state_t last_state = TINYPAN_STATE_IDLE;
static int event_count = 0;
static int state_change_event_count = 0;
static int disconnect_event_count = 0;

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
    state_change_event_count = 0;
    disconnect_event_count = 0;
}

static void event_callback(tinypan_event_t event, void* user_data) {
    (void)user_data;
    event_count++;
    if (event == TINYPAN_EVENT_STATE_CHANGED) {
        state_change_event_count++;
        last_state = tinypan_get_state();
    } else if (event == TINYPAN_EVENT_DISCONNECTED) {
        disconnect_event_count++;
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


static void setup_mock_time(uint32_t start_ms) {
    mock_hal_use_mock_time(true);
    mock_hal_set_tick_ms(start_ms);
}

static void teardown_mock_time(void) {
    mock_hal_use_mock_time(false);
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
 * Test: L2CAP connect timeout works correctly across 32-bit tick wrap-around
 */
static int test_l2cap_connect_timeout_wraparound(void) {
    tinypan_config_t config = get_test_config();
    setup_mock_time(0xFFFFFF00u);

    tinypan_init(&config);
    tinypan_start();

    /* Stay just below timeout first (should not fire). */
    mock_hal_advance_tick_ms(TINYPAN_L2CAP_CONNECT_TIMEOUT_MS - 1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Timeout fired too early across tick wrap-around\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* Cross the timeout threshold after wrap-around. */
    mock_hal_advance_tick_ms(1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after timeout across wrap-around, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    tinypan_deinit();
    teardown_mock_time();
    return 1;
}

/**
 * Test: L2CAP connect timeout transitions to RECONNECTING
 */
static int test_l2cap_connect_timeout_triggers_reconnect(void) {
    tinypan_config_t config = get_test_config();
    setup_mock_time(1000);

    tinypan_init(&config);
    tinypan_start();

    mock_hal_advance_tick_ms(TINYPAN_L2CAP_CONNECT_TIMEOUT_MS + 1);
    tinypan_process();

    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after connect timeout, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    tinypan_deinit();
    teardown_mock_time();
    return 1;
}

/**
 * Test: BNEP setup timeout exhausts retries then reconnects
 */
static int test_bnep_setup_timeout_exhausts_retries(void) {
    tinypan_config_t config = get_test_config();
    setup_mock_time(2000);

    tinypan_init(&config);
    tinypan_start();
    mock_hal_simulate_connect_success();
    tinypan_process();

    for (int i = 0; i < TINYPAN_BNEP_SETUP_RETRIES; i++) {
        mock_hal_advance_tick_ms(TINYPAN_BNEP_SETUP_TIMEOUT_MS + 1);
        tinypan_process();
    }

    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after setup retries exhausted, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    tinypan_deinit();
    teardown_mock_time();
    return 1;
}

/**
 * Test: Reconnect delay timing works across 32-bit tick wrap-around
 */
static int test_reconnect_delay_wraparound(void) {
    tinypan_config_t config = get_test_config();
    config.reconnect_interval_ms = 100;
    config.reconnect_max_ms = 1000;
    config.max_reconnect_attempts = 0; /* infinite for timing check */

    setup_mock_time(0xFFFFFFF0u);
    tinypan_init(&config);
    tinypan_start();

    /* Enter RECONNECTING and schedule retry from near-wrap tick. */
    mock_hal_simulate_connect_failure(-1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after failure near tick wrap\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* Still below reconnect delay after wrap. */
    mock_hal_advance_tick_ms(99);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Reconnect triggered too early across wrap-around delay\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* Reach exact delay threshold across wrap. */
    mock_hal_advance_tick_ms(1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING at wrapped reconnect threshold, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    tinypan_deinit();
    teardown_mock_time();
    return 1;
}

/**
 * Test: Reconnect waits for delay and uses exponential backoff capped by config
 */
static int test_reconnect_backoff_timing_and_cap(void) {
    tinypan_config_t config = get_test_config();
    config.reconnect_interval_ms = 100;
    config.reconnect_max_ms = 250;
    config.max_reconnect_attempts = 0; /* Infinite for backoff timing checks */

    setup_mock_time(4000);
    tinypan_init(&config);
    tinypan_start();

    /* Initial failure -> schedule first retry at +100ms */
    mock_hal_simulate_connect_failure(-1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after initial failure\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(99);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Reconnect triggered too early for first delay\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING exactly at first reconnect delay\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* Second failure -> next delay should be 200ms */
    mock_hal_simulate_connect_failure(-2);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after second failure\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(199);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Reconnect triggered too early for second delay\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING exactly at second reconnect delay\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* Third failure -> exponential backoff would be 400, but cap should hold at 250 */
    mock_hal_simulate_connect_failure(-3);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after third failure\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(249);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Reconnect triggered too early for capped delay\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING exactly at capped reconnect delay\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    tinypan_deinit();
    teardown_mock_time();
    return 1;
}

/**
 * Test: Reconnect backoff resets to initial interval after successful reconnect
 */
static int test_reconnect_backoff_resets_after_success(void) {
    tinypan_config_t config = get_test_config();
    config.reconnect_interval_ms = 100;
    config.reconnect_max_ms = 1000;
    config.max_reconnect_attempts = 0;

    setup_mock_time(5000);
    tinypan_init(&config);
    tinypan_start();

    /* Fail once: schedule reconnect delay at 100ms. */
    mock_hal_simulate_connect_failure(-1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after first failure\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(100);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING at first reconnect threshold\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* Complete successful connection path to reset reconnect backoff state. */
    mock_hal_simulate_connect_success();
    tinypan_process();
    mock_hal_simulate_bnep_setup_success();
    tinypan_process();

    if (tinypan_get_state() != TINYPAN_STATE_DHCP) {
        printf("\n    Expected DHCP after successful reconnect setup, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* Disconnect again: delay should reset to initial interval (100ms), not doubled. */
    mock_hal_simulate_disconnect();
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after post-success disconnect\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(99);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Reconnect triggered too early after backoff reset\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING at reset reconnect interval, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    tinypan_deinit();
    teardown_mock_time();
    return 1;
}

/**
 * Test: max_reconnect_attempts = 0 allows unlimited reconnect attempts
 */
static int test_reconnect_infinite_attempts(void) {
    tinypan_config_t config = get_test_config();
    config.reconnect_interval_ms = 100;
    config.reconnect_max_ms = 400;
    config.max_reconnect_attempts = 0; /* Infinite */

    setup_mock_time(6000);
    tinypan_init(&config);
    tinypan_start();

    /* Enter reconnect loop after initial failure. */
    mock_hal_simulate_connect_failure(-1);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after initial failure\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* Attempt #1 at +100ms, then fail again. */
    mock_hal_advance_tick_ms(100);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING on reconnect attempt #1\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }
    mock_hal_simulate_connect_failure(-2);
    tinypan_process();

    /* Attempt #2 at +200ms, then fail again. */
    mock_hal_advance_tick_ms(200);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING on reconnect attempt #2\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }
    mock_hal_simulate_connect_failure(-3);
    tinypan_process();

    /* Attempt #3 at +400ms (capped), should still be allowed (not ERROR). */
    mock_hal_advance_tick_ms(400);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING on reconnect attempt #3 with infinite retries, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    tinypan_deinit();
    teardown_mock_time();
    return 1;
}

/**
 * Test: Reconnect path honors max_reconnect_attempts
 */
static int test_reconnect_honors_max_attempts(void) {
    tinypan_config_t config = get_test_config();
    config.max_reconnect_attempts = 1;
    setup_mock_time(3000);

    tinypan_init(&config);
    tinypan_start();

    mock_hal_simulate_connect_failure(-1);
    tinypan_process();

    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after initial failure\n");
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* First retry should be allowed. */
    mock_hal_advance_tick_ms(config.reconnect_interval_ms);
    tinypan_process();

    if (tinypan_get_state() != TINYPAN_STATE_CONNECTING) {
        printf("\n    Expected CONNECTING for first reconnect attempt, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    /* Force retry failure, then verify next retry budget is exhausted. */
    mock_hal_simulate_connect_failure(-2);
    tinypan_process();
    if (tinypan_get_state() != TINYPAN_STATE_RECONNECTING) {
        printf("\n    Expected RECONNECTING after failed reconnect attempt, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    mock_hal_advance_tick_ms(config.reconnect_interval_ms * 2);
    tinypan_process();

    if (tinypan_get_state() != TINYPAN_STATE_ERROR) {
        printf("\n    Expected ERROR after max reconnect attempts reached, got %s\n",
               tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        teardown_mock_time();
        return 0;
    }

    tinypan_deinit();
    teardown_mock_time();
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

/**
 * Test: State change callbacks are emitted across runtime transitions
 */
static int test_state_change_event_sequence(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_set_event_callback(event_callback, NULL);

    tinypan_start();
    mock_hal_simulate_connect_success();
    tinypan_process();
    mock_hal_simulate_bnep_setup_success();
    tinypan_process();
    tinypan_internal_set_ip(0x0202A8C0u, 0x00FFFFFFu, 0x0102A8C0u, 0x08080808u);
    tinypan_stop();

    /* Expected state changes: CONNECTING, BNEP_SETUP, DHCP, ONLINE, IDLE */
    if (state_change_event_count < 5) {
        printf("\n    Expected >=5 state change events, got %d\n", state_change_event_count);
        tinypan_deinit();
        return 0;
    }

    if (disconnect_event_count != 1) {
        printf("\n    Expected exactly 1 disconnect event, got %d\n", disconnect_event_count);
        tinypan_deinit();
        return 0;
    }

    tinypan_deinit();
    return 1;
}

/**
 * Test: IP loss transitions ONLINE back to DHCP and emits IP_LOST path events
 */
static int test_ip_loss_transitions_to_dhcp(void) {
    tinypan_config_t config = get_test_config();
    tinypan_init(&config);
    tinypan_set_event_callback(event_callback, NULL);

    tinypan_start();
    mock_hal_simulate_connect_success();
    tinypan_process();
    mock_hal_simulate_bnep_setup_success();
    tinypan_process();
    tinypan_internal_set_ip(0x0202A8C0u, 0x00FFFFFFu, 0x0102A8C0u, 0x08080808u);

    if (tinypan_get_state() != TINYPAN_STATE_ONLINE) {
        printf("\n    Expected ONLINE before IP loss\n");
        tinypan_deinit();
        return 0;
    }

    int state_changes_before = state_change_event_count;
    tinypan_internal_clear_ip();

    if (tinypan_get_state() != TINYPAN_STATE_DHCP) {
        printf("\n    Expected DHCP after IP loss, got %s\n", tinypan_state_to_string(tinypan_get_state()));
        tinypan_deinit();
        return 0;
    }

    if (state_change_event_count <= state_changes_before) {
        printf("\n    Expected additional STATE_CHANGED event on IP loss\n");
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
    TEST(l2cap_connect_timeout_triggers_reconnect);
    TEST(l2cap_connect_timeout_wraparound);
    TEST(bnep_setup_timeout_exhausts_retries);
    TEST(reconnect_delay_wraparound);
    TEST(reconnect_backoff_timing_and_cap);
    TEST(reconnect_backoff_resets_after_success);
    TEST(reconnect_infinite_attempts);
    TEST(reconnect_honors_max_attempts);
    TEST(disconnect_during_dhcp_triggers_reconnect);
    TEST(stop_resets_to_idle);
    TEST(bnep_rejection_triggers_reconnect);
    TEST(state_to_string);
    TEST(config_defaults);
    TEST(full_connection_flow);
    TEST(state_change_event_sequence);
    TEST(ip_loss_transitions_to_dhcp);
    
    printf("\n========================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
    
    return (tests_passed == tests_run) ? 0 : 1;
}
