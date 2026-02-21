/*
 * TinyPAN lwIP Configuration (lwipopts.h)
 * 
 * Defines the features and memory footprint used by the lwIP stack
 * statically compiled into TinyPAN.
 */

#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* We are running without an OS, bare-metal */
#define NO_SYS                      1

/* Core features required for DHCP/IP */
#define LWIP_IPV4                   1
#define LWIP_UDP                    1
#define LWIP_DHCP                   1
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_NETIF_STATUS_CALLBACK  1

#include "tinypan_config.h"
#if TINYPAN_USE_BLE_SLIP
#define LWIP_HAVE_SLIPIF            1
#else
#define LWIP_HAVE_SLIPIF            0
#endif

/* Disable unused features to save ROM/RAM */
#define LWIP_IPV6                   0
#define LWIP_TCP                    0
#define LWIP_IGMP                   0
#define LWIP_ICMP                   0
#define LWIP_DNS                    0
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_STATS                  0
#define LWIP_DEBUG                  1

#define DHCP_DEBUG                  LWIP_DBG_ON
#define ETHARP_DEBUG                LWIP_DBG_ON
#define UDP_DEBUG                   LWIP_DBG_ON

/* Memory configuration */
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (4 * 1024)  /* 4KB heap for packets/state */

/* PBUF pool sizing */
#define PBUF_POOL_SIZE              4
#define PBUF_POOL_BUFSIZE           1536

/* Reserve space at the head of every pbuf for the BNEP encapsulation header */
#define PBUF_LINK_ENCAPSULATION_HLEN 15

/* System timers (using TinyPAN HAL) */
#define SYS_LIGHTWEIGHT_PROT        0

/** Resolve implicit declarations since we bypass sys.h */
#include <stdint.h>
extern uint32_t hal_get_tick_ms(void);

/* We will implement sys_now() in tinypan_lwip_netif.c */

#endif /* LWIP_LWIPOPTS_H */
