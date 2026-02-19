/*
 * TinyPAN Test - BNEP Unit Tests
 * 
 * Tests for BNEP packet building and parsing.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../src/tinypan_bnep.h"

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  Testing: %s... ", #name); \
        tests_run++; \
        if (test_##name()) { \
            printf("PASSED\n"); \
            tests_passed++; \
        } else { \
            printf("FAILED\n"); \
        } \
    } while(0)

static void print_hex(const uint8_t* data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

/* ============================================================================
 * Test Cases
 * ============================================================================ */

/**
 * Test building a BNEP setup request
 */
static int test_build_setup_request(void) {
    uint8_t buffer[32];
    
    int len = bnep_build_setup_request(buffer, sizeof(buffer),
                                         BNEP_UUID_PANU, BNEP_UUID_NAP);
    
    if (len != 7) {
        printf("\n    Expected length 7, got %d\n", len);
        return 0;
    }
    
    /* Expected: 0x01 (Control), 0x01 (Setup Request), 0x02 (16-bit UUIDs), 
                 0x11 0x16 (NAP), 0x11 0x15 (PANU) */
    uint8_t expected[] = {0x01, 0x01, 0x02, 0x11, 0x16, 0x11, 0x15};
    
    if (memcmp(buffer, expected, 7) != 0) {
        printf("\n    Packet mismatch:\n    Expected: ");
        print_hex(expected, 7);
        printf("    Got:      ");
        print_hex(buffer, 7);
        return 0;
    }
    
    return 1;
}

/**
 * Test building a BNEP setup response
 */
static int test_build_setup_response(void) {
    uint8_t buffer[32];
    
    int len = bnep_build_setup_response(buffer, sizeof(buffer),
                                          BNEP_SETUP_RESPONSE_SUCCESS);
    
    if (len != 4) {
        printf("\n    Expected length 4, got %d\n", len);
        return 0;
    }
    
    /* Expected: 0x01 (Control), 0x02 (Setup Response), 0x00 0x00 (Success) */
    uint8_t expected[] = {0x01, 0x02, 0x00, 0x00};
    
    if (memcmp(buffer, expected, 4) != 0) {
        printf("\n    Packet mismatch\n");
        return 0;
    }
    
    return 1;
}

/**
 * Test building a BNEP general Ethernet packet
 */
static int test_build_general_ethernet(void) {
    uint8_t buffer[64];
    uint8_t dst_addr[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t src_addr[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    
    int len = bnep_build_general_ethernet(buffer, sizeof(buffer),
                                           dst_addr, src_addr,
                                           BNEP_ETHERTYPE_IPV4,
                                           payload, sizeof(payload));
    
    /* Header: 1 + 6 + 6 + 2 = 15, plus 4 bytes payload = 19 */
    if (len != 19) {
        printf("\n    Expected length 19, got %d\n", len);
        return 0;
    }
    
    /* Check type byte */
    if (buffer[0] != BNEP_PKT_TYPE_GENERAL_ETHERNET) {
        printf("\n    Wrong type byte: 0x%02X\n", buffer[0]);
        return 0;
    }
    
    /* Check addresses */
    if (memcmp(&buffer[1], dst_addr, 6) != 0) {
        printf("\n    Destination address mismatch\n");
        return 0;
    }
    if (memcmp(&buffer[7], src_addr, 6) != 0) {
        printf("\n    Source address mismatch\n");
        return 0;
    }
    
    /* Check EtherType (big-endian 0x0800) */
    if (buffer[13] != 0x08 || buffer[14] != 0x00) {
        printf("\n    EtherType mismatch: %02X %02X\n", buffer[13], buffer[14]);
        return 0;
    }
    
    /* Check payload */
    if (memcmp(&buffer[15], payload, 4) != 0) {
        printf("\n    Payload mismatch\n");
        return 0;
    }
    
    return 1;
}

/**
 * Test building a BNEP compressed Ethernet packet
 */
static int test_build_compressed_ethernet(void) {
    uint8_t buffer[64];
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    
    int len = bnep_build_compressed_ethernet(buffer, sizeof(buffer),
                                               BNEP_ETHERTYPE_ARP,
                                               payload, sizeof(payload));
    
    /* Header: 1 + 2 = 3, plus 4 bytes payload = 7 */
    if (len != 7) {
        printf("\n    Expected length 7, got %d\n", len);
        return 0;
    }
    
    /* Check type byte */
    if (buffer[0] != BNEP_PKT_TYPE_COMPRESSED_ETHERNET) {
        printf("\n    Wrong type byte: 0x%02X\n", buffer[0]);
        return 0;
    }
    
    /* Check EtherType (big-endian 0x0806) */
    if (buffer[1] != 0x08 || buffer[2] != 0x06) {
        printf("\n    EtherType mismatch: %02X %02X\n", buffer[1], buffer[2]);
        return 0;
    }
    
    return 1;
}

/**
 * Test parsing a BNEP general Ethernet packet
 */
static int test_parse_general_ethernet(void) {
    /* Build a packet first */
    uint8_t packet[] = {
        0x00,  /* Type: General Ethernet */
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,  /* Dst addr */
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,  /* Src addr */
        0x08, 0x00,  /* EtherType: IPv4 */
        0x45, 0x00, 0x00, 0x14  /* Payload (fake IP header start) */
    };
    
    bnep_ethernet_frame_t frame;
    uint8_t local_addr[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t remote_addr[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    int result = bnep_parse_ethernet_frame(packet, sizeof(packet),
                                            local_addr, remote_addr, &frame);
    
    if (result != 0) {
        printf("\n    Parse failed: %d\n", result);
        return 0;
    }
    
    /* Check parsed values */
    if (frame.ethertype != BNEP_ETHERTYPE_IPV4) {
        printf("\n    Wrong EtherType: 0x%04X\n", frame.ethertype);
        return 0;
    }
    
    if (frame.payload_len != 4) {
        printf("\n    Wrong payload length: %u\n", frame.payload_len);
        return 0;
    }
    
    uint8_t expected_dst[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    if (memcmp(frame.dst_addr, expected_dst, 6) != 0) {
        printf("\n    Dst addr mismatch\n");
        return 0;
    }
    
    return 1;
}

/**
 * Test parsing a BNEP setup response
 */
static int test_parse_setup_response(void) {
    /* Setup response: Control type (0x02) + response code */
    uint8_t data[] = {0x02, 0x00, 0x00};  /* Success */
    
    bnep_setup_response_t response;
    int result = bnep_parse_setup_response(data, sizeof(data), &response);
    
    if (result != 0) {
        printf("\n    Parse failed: %d\n", result);
        return 0;
    }
    
    if (response.response_code != BNEP_SETUP_RESPONSE_SUCCESS) {
        printf("\n    Wrong response code: 0x%04X\n", response.response_code);
        return 0;
    }
    
    return 1;
}

/**
 * Test parsing a BNEP compressed Ethernet packet
 */
static int test_parse_compressed_ethernet(void) {
    uint8_t packet[] = {
        0x02,  /* Type: Compressed Ethernet */
        0x08, 0x06,  /* EtherType: ARP */
        0x00, 0x01, 0x02, 0x03  /* Payload */
    };
    
    bnep_ethernet_frame_t frame;
    uint8_t local_addr[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t remote_addr[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    int result = bnep_parse_ethernet_frame(packet, sizeof(packet),
                                            local_addr, remote_addr, &frame);
    
    if (result != 0) {
        printf("\n    Parse failed: %d\n", result);
        return 0;
    }
    
    if (frame.ethertype != BNEP_ETHERTYPE_ARP) {
        printf("\n    Wrong EtherType: 0x%04X\n", frame.ethertype);
        return 0;
    }
    
    /* In compressed mode, dst=local, src=remote */
    if (memcmp(frame.dst_addr, local_addr, 6) != 0) {
        printf("\n    Dst addr should be local addr\n");
        return 0;
    }
    if (memcmp(frame.src_addr, remote_addr, 6) != 0) {
        printf("\n    Src addr should be remote addr\n");
        return 0;
    }
    
    return 1;
}

/**
 * Test buffer size validation
 */
static int test_buffer_overflow_protection(void) {
    uint8_t small_buffer[4];
    
    /* Try to build a packet into a too-small buffer */
    int len = bnep_build_setup_request(small_buffer, sizeof(small_buffer),
                                         BNEP_UUID_PANU, BNEP_UUID_NAP);
    
    if (len >= 0) {
        printf("\n    Should have failed due to small buffer\n");
        return 0;
    }
    
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("TinyPAN BNEP Unit Tests\n");
    printf("=======================\n\n");
    
    /* Initialize BNEP layer */
    bnep_init();
    
    printf("Running tests:\n");
    
    TEST(build_setup_request);
    TEST(build_setup_response);
    TEST(build_general_ethernet);
    TEST(build_compressed_ethernet);
    TEST(parse_general_ethernet);
    TEST(parse_setup_response);
    TEST(parse_compressed_ethernet);
    TEST(buffer_overflow_protection);
    
    printf("\n=======================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
    
    return (tests_passed == tests_run) ? 0 : 1;
}
