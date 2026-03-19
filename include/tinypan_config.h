/*
 * TinyPAN Configuration
 * 
 * Compile-time configuration options for TinyPAN library.
 * Edit this file to customize the library for your platform.
 */

#ifndef TINYPAN_CONFIG_H
#define TINYPAN_CONFIG_H

/* ============================================================================
 * Memory Configuration
 * ============================================================================ */

/**
 * Maximum size of Ethernet frame payload.
 * Standard Ethernet MTU is 1500 bytes.
 * BNEP requires minimum L2CAP MTU of 1691 bytes.
 */
#ifndef TINYPAN_MAX_FRAME_SIZE
#define TINYPAN_MAX_FRAME_SIZE          1500
#endif

/**
 * Maximum Transmission Unit for the L2CAP BNEP channel.
 * BNEP standard requires a minimum of 1691 bytes.
 */
#ifndef TINYPAN_L2CAP_MTU
#define TINYPAN_L2CAP_MTU               1691
#endif

/**
 * Size limit for a single incoming SLIP frame (bytes).
 * The SLIP transport backend builds pbuf chains from raw incoming bytes
 * using a streaming FSM; it does not use a static ring buffer.
 * This define is retained for integrator reference but is not used
 * internally. Unused in BNEP mode.
 */
#ifndef TINYPAN_RX_BUFFER_SIZE
#define TINYPAN_RX_BUFFER_SIZE          1700
#endif

/* ============================================================================
 * Queue Configuration
 * ============================================================================ */

/**
 * Maximum number of frames in the TX queue before dropping.
 * Due to the ring buffer design, the usable capacity is one less than
 * this value (e.g., 3 slots hold up to 2 frames in flight).
 *
 * Tuning: TCP congestion control handles backpressure from ERR_MEM
 * returns naturally, but very small queues with high BLE round-trip latency
 * can reduce throughput through excessive retransmissions. 3 is the default
 * to limit peak memory use during TCP burst scenarios on low-RAM targets.
 */
#ifndef TINYPAN_TX_QUEUE_LEN
#define TINYPAN_TX_QUEUE_LEN                3
#endif

/* ============================================================================
 * Timeout Configuration (in milliseconds)
 * ============================================================================ */

/**
 * Timeout waiting for L2CAP connection to establish.
 */
#ifndef TINYPAN_L2CAP_CONNECT_TIMEOUT_MS
#define TINYPAN_L2CAP_CONNECT_TIMEOUT_MS    10000
#endif

/**
 * Timeout waiting for BNEP setup response.
 */
#ifndef TINYPAN_BNEP_SETUP_TIMEOUT_MS
#define TINYPAN_BNEP_SETUP_TIMEOUT_MS       5000
#endif

/**
 * Timeout waiting for BNEP multicast filter response.
 */
#ifndef TINYPAN_BNEP_FILTER_TIMEOUT_MS
#define TINYPAN_BNEP_FILTER_TIMEOUT_MS      2000
#endif

/**
 * Number of retries for BNEP setup request.
 */
#ifndef TINYPAN_BNEP_SETUP_RETRIES
#define TINYPAN_BNEP_SETUP_RETRIES          3
#endif

/**
 * Timeout waiting for DHCP to complete.
 */
#ifndef TINYPAN_DHCP_TIMEOUT_MS
#define TINYPAN_DHCP_TIMEOUT_MS             30000
#endif

/* ============================================================================
 * Feature Configuration
 * ============================================================================ */

/**
 * BNEP Header Compression.
 * 
 * NOTE: For stability and compatibility with modern mobile OS networking stacks, 
 * TinyPAN permanently uses General Ethernet headers (15 bytes). Compression 
 * is disabled to prevent packet drops on globally routed traffic (DNS, TCP).
 */
#ifndef TINYPAN_ENABLE_COMPRESSION
#define TINYPAN_ENABLE_COMPRESSION          0
#endif

#ifndef TINYPAN_FORCE_UNCOMPRESSED_TX
#define TINYPAN_FORCE_UNCOMPRESSED_TX       1
#endif

/**
 * Handle lwIP initialization internally.
 * Set to 0 if your host application or OS (e.g. ESP-IDF) already initializes lwIP.
 */
#ifndef TINYPAN_AUTO_INIT_LWIP
#define TINYPAN_AUTO_INIT_LWIP              1
#endif

/**
 * Enable automatic reconnection on disconnect.
 */
#ifndef TINYPAN_ENABLE_AUTO_RECONNECT
#define TINYPAN_ENABLE_AUTO_RECONNECT       1
#endif

/**
 * Operating Mode: Dual-Path Architecture
 * 0: Native Bluetooth Classic (BNEP). Requires a BT Classic radio. Connects directly
 *    to iOS/Android Personal Hotspot menus.
 * 1: BLE Bridge Mode (SLIP). For pure BLE chips (nRF52, ESP32-C3). Requires a custom 
 *    companion app on the phone to act as a VPN tunnel.
 */
#ifndef TINYPAN_USE_BLE_SLIP
#define TINYPAN_USE_BLE_SLIP                0
#endif

/**
 * Enable heartbeat/link monitoring.
 * Reserved for future use. The supervisor currently operates on transport-layer
 * connectivity events.
 */
#ifndef TINYPAN_ENABLE_HEARTBEAT
#define TINYPAN_ENABLE_HEARTBEAT            0
#endif

/**
 * Enable debug logging.
 * Set to 0 to disable all debug output and reduce code size.
 */
#ifndef TINYPAN_ENABLE_DEBUG
#define TINYPAN_ENABLE_DEBUG                1
#endif

/* ============================================================================
 * Debug/Logging Configuration
 * ============================================================================ */

#if TINYPAN_ENABLE_DEBUG

/**
 * Debug log function.
 * Override this macro to direct debug output to your platform's logging system.
 * 
 * Default: printf (requires <stdio.h>)
 */
#ifndef TINYPAN_LOG
#include <stdio.h>
#define TINYPAN_LOG(fmt, ...)   printf("[TinyPAN] " fmt "\n", ##__VA_ARGS__)
#endif

/**
 * Debug log levels
 */
#ifndef TINYPAN_LOG_ERROR
#define TINYPAN_LOG_ERROR(fmt, ...)   TINYPAN_LOG("[ERROR] " fmt, ##__VA_ARGS__)
#endif

#ifndef TINYPAN_LOG_WARN
#define TINYPAN_LOG_WARN(fmt, ...)    TINYPAN_LOG("[WARN] " fmt, ##__VA_ARGS__)
#endif

#ifndef TINYPAN_LOG_INFO
#define TINYPAN_LOG_INFO(fmt, ...)    TINYPAN_LOG("[INFO] " fmt, ##__VA_ARGS__)
#endif

#ifndef TINYPAN_LOG_DEBUG
#define TINYPAN_LOG_DEBUG(fmt, ...)   TINYPAN_LOG("[DEBUG] " fmt, ##__VA_ARGS__)
#endif

#else /* !TINYPAN_ENABLE_DEBUG */

#define TINYPAN_LOG(fmt, ...)
#define TINYPAN_LOG_ERROR(fmt, ...)
#define TINYPAN_LOG_WARN(fmt, ...)
#define TINYPAN_LOG_INFO(fmt, ...)
#define TINYPAN_LOG_DEBUG(fmt, ...)

#endif /* TINYPAN_ENABLE_DEBUG */

/* ============================================================================
 * Platform-Specific Configuration
 * ============================================================================ */

/**
 * Byte order helpers.
 * When lwIP is enabled, these map to lwIP's byte-swap macros.
 * Otherwise, portable fallback implementations are used.
 */
#if TINYPAN_ENABLE_LWIP
#include "lwip/def.h"
#define TINYPAN_HTONS(x)    lwip_htons(x)
#define TINYPAN_NTOHS(x)    lwip_ntohs(x)
#define TINYPAN_HTONL(x)    lwip_htonl(x)
#define TINYPAN_NTOHL(x)    lwip_ntohl(x)
#else

#ifndef TINYPAN_HTONS
#define TINYPAN_HTONS(x)    ((((x) & 0xFF) << 8) | (((x) >> 8) & 0xFF))
#endif

#ifndef TINYPAN_NTOHS
#define TINYPAN_NTOHS(x)    TINYPAN_HTONS(x)
#endif

#ifndef TINYPAN_HTONL
#define TINYPAN_HTONL(x)    ((((x) & 0xFF) << 24) | (((x) & 0xFF00) << 8) | \
                             (((x) >> 8) & 0xFF00) | (((x) >> 24) & 0xFF))
#endif

#ifndef TINYPAN_NTOHL
#define TINYPAN_NTOHL(x)    TINYPAN_HTONL(x)
#endif

#endif

#endif /* TINYPAN_CONFIG_H */
