/*
 * TinyPAN lwIP Configuration
 * 
 * This file configures lwIP for minimal footprint with TinyPAN.
 * It enables only what we need: IPv4, DHCP, UDP, TCP, and the raw API.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ============================================================================
 * Platform and Architecture
 * ============================================================================ */

/* NO_SYS: Run without an OS (bare-metal or polling mode) */
#define NO_SYS                          1

/* Use lightweight memory management */
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0

/* Checksum options - let CPU calculate (no hardware offload) */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_ICMP             1

/* ============================================================================
 * Memory Configuration
 * ============================================================================ */

/* Memory alignment */
#define MEM_ALIGNMENT                   4

/* Heap size */
#define MEM_SIZE                        (8 * 1024)

/* Pool sizes - keep minimal */
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_NETBUF                 4
#define MEMP_NUM_NETCONN                4
#define MEMP_NUM_SYS_TIMEOUT            8

/* Pbuf options */
#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               1600

/* ============================================================================
 * Protocol Configuration
 * ============================================================================ */

/* IPv4 */
#define LWIP_IPV4                       1
#define IP_FORWARD                      0
#define IP_REASSEMBLY                   0
#define IP_FRAG                         0

/* IPv6 - disabled to save space */
#define LWIP_IPV6                       0

/* ICMP - enabled for ping */
#define LWIP_ICMP                       1

/* DHCP - essential for getting IP */
#define LWIP_DHCP                       1
#define DHCP_DOES_ARP_CHECK             0

/* ARP */
#define LWIP_ARP                        1
#define ARP_TABLE_SIZE                  10
#define ARP_QUEUEING                    1

/* UDP */
#define LWIP_UDP                        1

/* TCP */
#define LWIP_TCP                        1
#define TCP_MSS                         1460
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                8
#define TCP_QUEUE_OOSEQ                 0
#define LWIP_TCP_KEEPALIVE              1

/* DNS - useful for hostname resolution */
#define LWIP_DNS                        1
#define DNS_TABLE_SIZE                  4

/* ============================================================================
 * API Configuration
 * ============================================================================ */

/* Disable sequential/socket API (we use raw API) */
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0

/* Enable raw API */
#define LWIP_RAW                        1

/* ============================================================================
 * Network Interface Configuration
 * ============================================================================ */

/* Only one network interface */
#define LWIP_SINGLE_NETIF               1

/* MTU */
#define NETIF_MTU                       1500

/* Enable link callback for up/down events */
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_STATUS_CALLBACK      1

/* ============================================================================
 * Debug Options
 * ============================================================================ */

/* Enable debugging output */
#ifdef TINYPAN_LWIP_DEBUG
#define LWIP_DEBUG                      1
#define ETHARP_DEBUG                    LWIP_DBG_ON
#define NETIF_DEBUG                     LWIP_DBG_ON
#define DHCP_DEBUG                      LWIP_DBG_ON
#define IP_DEBUG                        LWIP_DBG_ON
#define TCP_DEBUG                       LWIP_DBG_ON
#define UDP_DEBUG                       LWIP_DBG_ON
#else
#define LWIP_DEBUG                      0
#endif

/* Use printf for debug output */
#define LWIP_PLATFORM_DIAG(x)           do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x)         do { printf("Assertion \"%s\" failed at line %d in %s\n", \
                                                     x, __LINE__, __FILE__); while(1); } while(0)

/* ============================================================================
 * Statistics (disabled to save space)
 * ============================================================================ */
#define LWIP_STATS                      0

/* ============================================================================
 * Threading (disabled - NO_SYS mode)
 * ============================================================================ */
#define LWIP_TCPIP_CORE_LOCKING         0
#define SYS_LIGHTWEIGHT_PROT            0

#endif /* LWIPOPTS_H */
